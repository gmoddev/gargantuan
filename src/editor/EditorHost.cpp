// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/editor/EditorHost.hpp"

#include "gargantuan/InstanceProperty.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/runtime/Snapshot.hpp"
#include "gargantuan/runtime/WireCodec.hpp"
#include "gargantuan/runtime/WireJournal.hpp"

#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

namespace gargantuan {
	namespace {
		using Json = nlohmann::ordered_json;

		enum class LineReadStatus { Complete, End, Oversized };

		LineReadStatus ReadBoundedLine(std::istream &input, std::string &line) {
			line.clear();
			bool oversized = false;
			for (;;) {
				const auto next = input.get();
				if (next == std::char_traits<char>::eof()) {
					if (line.empty() && !oversized) return LineReadStatus::End;
					return oversized ? LineReadStatus::Oversized : LineReadStatus::Complete;
				}
				if (next == '\n') return oversized ? LineReadStatus::Oversized : LineReadStatus::Complete;
				if (next == '\0') oversized = true;
				if (!oversized && line.size() < EditorHostMaximumRequestBytes) line.push_back(static_cast<char>(next));
				else oversized = true;
			}
		}

		bool HasOnlyFields(const Json &object, std::initializer_list<std::string_view> allowed) {
			if (!object.is_object()) return false;
			for (const auto &[name, value] : object.items()) {
				(void)value;
				if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) return false;
			}
			return true;
		}

		Json ErrorResponse(const Json &requestId, std::string code, std::string message) {
			return Json{
				{"Version", EditorHostProtocolVersion},
				{"RequestId", requestId},
				{"Ok", false},
				{"Error", {{"Code", std::move(code)}, {"Message", std::move(message)}}},
			};
		}

		Json SuccessResponse(const Json &requestId, Json result) {
			return Json{
				{"Version", EditorHostProtocolVersion},
				{"RequestId", requestId},
				{"Ok", true},
				{"Result", std::move(result)},
			};
		}

		std::string SerializeBoundedResponse(Json response) {
			auto serialized = response.dump();
			if (serialized.size() <= EditorHostMaximumResponseBytes) return serialized;
			return ErrorResponse(nullptr, "ResponseTooLarge", "EditorHost response exceeded its byte limit").dump();
		}

		const char *MutationStatusName(MutationStatus status) {
			switch (status) {
				case MutationStatus::Success: return "Success";
				case MutationStatus::WrongExecutionDomain: return "WrongExecutionDomain";
				case MutationStatus::StaleObject: return "StaleObject";
				case MutationStatus::InvalidClass: return "InvalidClass";
				case MutationStatus::InvalidProperty: return "InvalidProperty";
				case MutationStatus::ReadOnly: return "ReadOnly";
				case MutationStatus::Unauthorized: return "Unauthorized";
				case MutationStatus::ValidationFailed: return "ValidationFailed";
				case MutationStatus::Rejected: return "Rejected";
				case MutationStatus::InternalError: return "InternalError";
			}
			return "InternalError";
		}

		Json EncodeCursor(ChangeCursor cursor) {
			return Json{
				{"Scope", EncodeWireObjectId(WireObjectId::FromObjectId(cursor.Scope))},
				{"NextSequence", cursor.NextSequence},
			};
		}
	}

	EditorHost::EditorHost(std::string sessionToken) : SessionToken(std::move(sessionToken)) {
		if (SessionToken.empty() || SessionToken.size() > 256)
			throw std::invalid_argument("EditorHost requires a nonempty bounded session token");
	}

	std::string EditorHost::HandleRequest(std::string_view request) {
		Json requestId = nullptr;
		try {
			if (request.empty() || request.size() > EditorHostMaximumRequestBytes)
				return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Request byte length is invalid"));

			auto message = Json::parse(request);
			if (!HasOnlyFields(message, {"Version", "RequestId", "SessionToken", "Method", "Params"}) ||
				message.value("Version", 0u) != EditorHostProtocolVersion ||
				!message.contains("RequestId") || !message["RequestId"].is_string() ||
				message["RequestId"].get_ref<const std::string &>().size() > 128 ||
				!message.contains("SessionToken") || !message["SessionToken"].is_string() ||
				!message.contains("Method") || !message["Method"].is_string() ||
				message["Method"].get_ref<const std::string &>().size() > 64 ||
				!message.contains("Params") || !message["Params"].is_object())
				return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Invalid request envelope"));

			requestId = message["RequestId"];
			if (message["SessionToken"].get<std::string>() != SessionToken)
				return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Session token was rejected"));

			const auto method = message["Method"].get<std::string>();
			const auto &parameters = message["Params"];
			if (method == "Handshake") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Handshake takes no parameters"));
				return SerializeBoundedResponse(SuccessResponse(
					requestId,
					{
						{"Engine", "Gargantuan"},
						{"ProtocolVersion", EditorHostProtocolVersion},
						{"Capabilities", {"OpenProject", "Schema", "Snapshot", "Journal", "SetProperty"}},
					}
				));
			}

			if (method == "OpenProject") {
				if (!HasOnlyFields(parameters, {"Root"}) || !parameters.contains("Root") ||
					!parameters["Root"].is_string() || parameters["Root"].get_ref<const std::string &>().empty() ||
					parameters["Root"].get_ref<const std::string &>().size() > 32768)
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "OpenProject requires a bounded Root"));

				auto root = std::filesystem::weakly_canonical(std::filesystem::path(parameters["Root"].get<std::string>()));
				if (!std::filesystem::is_directory(root))
					return SerializeBoundedResponse(ErrorResponse(requestId, "ProjectNotFound", "Project root is not a directory"));
				auto filesystem = std::make_unique<DiskFilesystem>(root);
				auto project = Project::fromExisting(filesystem.get());
				auto world = project.DeserializeGame();
				if (World) World->Destroy();
				Filesystem = std::move(filesystem);
				World = std::move(world);
				World->Filesystem = Filesystem.get();
				Cursor.reset();
				return SerializeBoundedResponse(SuccessResponse(
					requestId,
					{{"Root", EncodeWireObjectId(WireObjectId::FromObjectId(World->GetObjectId()))}}
				));
			}

			if (method == "GetSchema") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GetSchema takes no parameters"));
				auto classNames = InstanceClassRegistry::GetClassNames();
				std::ranges::sort(classNames);
				Json classes = Json::array();
				for (const auto &className : classNames) {
					auto *definition = InstanceClassRegistry::GetDefinitionByName(className);
					if (!definition) continue;
					std::vector<std::string> propertyNames;
					propertyNames.reserve(definition->AllProperties.size());
					for (const auto &[name, property] : definition->AllProperties) {
						(void)property;
						propertyNames.push_back(name);
					}
					std::ranges::sort(propertyNames);
					Json properties = Json::array();
					for (const auto &name : propertyNames) {
						const auto *property = definition->AllProperties.at(name);
						Json encoded{
							{"Name", name},
							{"Type", property->ReflectedTypedef},
							{"Readable", static_cast<bool>(property->Read)},
							{"Writable", static_cast<bool>(property->Write)},
							{"Editable", property->Editable},
							{"Persistence", property->PersistencePolicy == InstanceProperty::Persistence::Saved ? "Saved" : "Transient"},
							{"Replication", property->ReplicationPolicy == InstanceProperty::Replication::FutureReplicated ? "Replicated" : "None"},
							{"Authority", property->WriteAuthority == InstanceProperty::Authority::Main ? "Main" : "Any"},
							{"HasValidator", static_cast<bool>(property->Validate)},
						};
						if (auto defaultValue = EncodeNativeWireValue(property->Unmodified))
							encoded["Default"] = EncodeWireValue(*defaultValue);
						properties.push_back(std::move(encoded));
					}
					classes.push_back({
						{"Name", definition->ClassName},
						{"Description", definition->Description},
						{"Superclass", definition->Superclass ? Json(*definition->Superclass) : Json(nullptr)},
						{"Constructible", definition->Constructor != nullptr},
						{"Properties", std::move(properties)},
					});
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, {{"Classes", std::move(classes)}}));
			}

			if (!World)
				return SerializeBoundedResponse(ErrorResponse(requestId, "ProjectRequired", "OpenProject must succeed first"));

			if (method == "GetSnapshot") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GetSnapshot takes no parameters"));
				auto snapshot = CaptureSnapshot(World);
				Cursor = snapshot.Cursor;
				return SerializeBoundedResponse(SuccessResponse(
					requestId,
					{{"Snapshot", Json::parse(SerializeSnapshot(snapshot))}}
				));
			}

			if (method == "PollChanges") {
				if (!Cursor)
					return SerializeBoundedResponse(ErrorResponse(requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"));
				if (!HasOnlyFields(parameters, {"MaximumRecords"}))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "PollChanges has unknown parameters"));
				std::size_t maximumRecords = 256;
				if (parameters.contains("MaximumRecords")) {
					if (!parameters["MaximumRecords"].is_number_unsigned())
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "MaximumRecords must be unsigned"));
					maximumRecords = parameters["MaximumRecords"].get<std::size_t>();
					if (maximumRecords == 0 || maximumRecords > 256)
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "MaximumRecords is out of range"));
				}
				auto changes = ChangeJournal::Get().Read(*Cursor, maximumRecords);
				if (changes.Status == ChangeReadStatus::ResnapshotRequired)
					return SerializeBoundedResponse(ErrorResponse(requestId, "ResnapshotRequired", "Journal cursor was evicted"));
				std::vector<WireJournalRecord> records;
				records.reserve(changes.Records.size());
				for (const auto &record : changes.Records) records.push_back(EncodeChangeRecord(record));
				Cursor = changes.Cursor;
				auto wire = Json::parse(SerializeWireJournalRecords(records));
				return SerializeBoundedResponse(SuccessResponse(
					requestId,
					{{"Records", std::move(wire["Records"])}, {"Cursor", EncodeCursor(*Cursor)}}
				));
			}

			if (method == "SetProperty") {
				if (!Cursor)
					return SerializeBoundedResponse(ErrorResponse(requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"));
				if (!HasOnlyFields(parameters, {"Object", "Property", "Value"}) ||
					!parameters.contains("Object") || !parameters.contains("Property") ||
					!parameters["Property"].is_string() || parameters["Property"].get_ref<const std::string &>().empty() ||
					parameters["Property"].get_ref<const std::string &>().size() > 256 || !parameters.contains("Value"))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "SetProperty fields are invalid"));
				auto object = DecodeWireObjectId(parameters["Object"]);
				auto value = DecodeWireValue(parameters["Value"]);
				if (!object || !value)
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "SetProperty Object or WireValue is invalid"));
				auto target = ObjectRegistry::Get().Lookup(object->ToObjectId());
				if (!target || target->GetReplicationScopeId() != World->GetObjectId())
					return SerializeBoundedResponse(ErrorResponse(requestId, "StaleObject", "Object is not live in the open project"));
				auto native = DecodeNativeWireValue(*value);
				if (!native)
					return SerializeBoundedResponse(ErrorResponse(requestId, "UnsupportedValue", "EditorHost v0 accepts closed value properties only"));
				auto mutation = Mutations.Apply(UpdatePropertyCommand{
					object->ToObjectId(), parameters["Property"].get<std::string>(), std::move(*native)
				});
				Json result{
					{"Status", MutationStatusName(mutation.Status)},
					{"Message", mutation.Message},
				};
				if (mutation.Object) result["Object"] = EncodeWireObjectId(WireObjectId::FromObjectId(*mutation.Object));
				return SerializeBoundedResponse(
					mutation.Succeeded() ? SuccessResponse(requestId, std::move(result))
										: ErrorResponse(requestId, MutationStatusName(mutation.Status), mutation.Message)
				);
			}

			return SerializeBoundedResponse(ErrorResponse(requestId, "UnknownMethod", "EditorHost method is not supported"));
		} catch (const nlohmann::json::exception &error) {
			return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", error.what()));
		} catch (const std::exception &error) {
			return SerializeBoundedResponse(ErrorResponse(requestId, "RequestRejected", error.what()));
		} catch (...) {
			return SerializeBoundedResponse(ErrorResponse(requestId, "InternalError", "Unknown EditorHost failure"));
		}
	}

	int EditorHost::Run(std::istream &input, std::ostream &output) {
		std::string line;
		for (;;) {
			const auto status = ReadBoundedLine(input, line);
			if (status == LineReadStatus::End) return 0;
			const auto response = status == LineReadStatus::Oversized
				? SerializeBoundedResponse(ErrorResponse(nullptr, "RequestTooLarge", "Request exceeded its byte limit"))
				: HandleRequest(line);
			output << EditorHostResponsePrefix << response << '\n';
			output.flush();
			if (!output) return 1;
		}
	}
}
