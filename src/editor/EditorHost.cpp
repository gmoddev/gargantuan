// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/editor/EditorHost.hpp"

#include "gargantuan/InstanceProperty.hpp"
#include "gargantuan/classes/Camera.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/runtime/Snapshot.hpp"
#include "gargantuan/runtime/WireCodec.hpp"
#include "gargantuan/runtime/WireJournal.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
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

		std::optional<glm::vec3> DecodeVector3(const Json &value) {
			if (!value.is_array() || value.size() != 3) return std::nullopt;
			glm::vec3 result;
			for (std::size_t index = 0; index < 3; ++index) {
				if (!value[index].is_number()) return std::nullopt;
				const auto component = value[index].get<double>();
				if (!std::isfinite(component) || std::abs(component) > 1'000'000.0) return std::nullopt;
				result[index] = static_cast<float>(component);
			}
			return result;
		}

		std::string EncodeBase64(const std::vector<std::uint8_t> &bytes) {
			static constexpr std::string_view Alphabet =
				"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string result;
			result.reserve(((bytes.size() + 2) / 3) * 4);
			for (std::size_t index = 0; index < bytes.size(); index += 3) {
				const std::uint32_t first = bytes[index];
				const std::uint32_t second = index + 1 < bytes.size() ? bytes[index + 1] : 0;
				const std::uint32_t third = index + 2 < bytes.size() ? bytes[index + 2] : 0;
				const std::uint32_t value = (first << 16) | (second << 8) | third;
				result.push_back(Alphabet[(value >> 18) & 0x3f]);
				result.push_back(Alphabet[(value >> 12) & 0x3f]);
				result.push_back(index + 1 < bytes.size() ? Alphabet[(value >> 6) & 0x3f] : '=');
				result.push_back(index + 2 < bytes.size() ? Alphabet[value & 0x3f] : '=');
			}
			return result;
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
				Json capabilities = {
					"OpenProject", "Schema", "Snapshot", "Journal", "SetProperty",
					"ConfigureViewport", "SetViewportCamera", "CaptureViewport", "PickViewport"
				};
				Json viewportTransports = Json::array({{
					{"Name", "Base64"}, {"Version", 1}, {"PixelFormats", {"RGB8"}}
				}});
				if (SharedFrameRing::IsSupported()) {
					capabilities.push_back("SharedMemoryViewport");
					viewportTransports.push_back({
						{"Name", "SharedMemoryRing"},
						{"Version", SharedFrameRingLayout::Version},
						{"PixelFormats", {"RGB8"}},
						{"SlotCount", SharedFrameRingLayout::SlotCount},
					});
				}
				return SerializeBoundedResponse(SuccessResponse(
					requestId,
					{
						{"Engine", "Gargantuan"},
						{"ProtocolVersion", EditorHostProtocolVersion},
						{"Capabilities", std::move(capabilities)},
						{"ViewportWireVersion", 1},
						{"ViewportTransports", std::move(viewportTransports)},
					}
				));
			}

			if (method == "OpenViewportTransport") {
				if (!HasOnlyFields(parameters, {"Transport", "Version", "PixelFormat"}) ||
					parameters.value("Transport", "") != "SharedMemoryRing" ||
					parameters.value("Version", 0u) != SharedFrameRingLayout::Version ||
					parameters.value("PixelFormat", "") != "RGB8")
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "UnsupportedViewportTransport", "Requested viewport transport contract is unsupported"
					));
				if (!SharedFrameRing::IsSupported())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "UnsupportedViewportTransport", "Shared-memory viewport transport is unavailable"
					));
				try {
					if (!ViewportFrameRing) ViewportFrameRing = std::make_unique<SharedFrameRing>();
					return SerializeBoundedResponse(SuccessResponse(requestId, {
						{"Transport", "SharedMemoryRing"},
						{"Version", SharedFrameRingLayout::Version},
						{"Name", ViewportFrameRing->GetName()},
						{"MappingBytes", ViewportFrameRing->GetMappingBytes()},
						{"HeaderBytes", SharedFrameRingLayout::HeaderBytes},
						{"SlotCount", SharedFrameRingLayout::SlotCount},
						{"SlotHeaderBytes", SharedFrameRingLayout::SlotHeaderBytes},
						{"SlotStride", SharedFrameRingLayout::SlotStride},
						{"MaximumPayloadBytes", SharedFrameRingLayout::MaximumPayloadBytes},
						{"PixelFormat", "RGB8"},
					}));
				} catch (const std::exception &error) {
					ViewportFrameRing.reset();
					return SerializeBoundedResponse(ErrorResponse(requestId, "ViewportTransportUnavailable", error.what()));
				}
			}

			if (method == "CloseViewportTransport") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "CloseViewportTransport takes no parameters"
					));
				ViewportFrameRing.reset();
				return SerializeBoundedResponse(SuccessResponse(requestId, {{"Closed", true}}));
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
				auto workspace = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
				if (!workspace)
					return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidProject", "Project has no valid Workspace"));
				{
					ScopedChangeJournalSuppression suppressSessionState;
					ViewportCamera = std::make_shared<Camera>();
				}
				Cursor.reset();
				ViewportWidth = 0;
				ViewportHeight = 0;
				ViewportFrameNumber = 0;
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

			auto workspace = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
			auto camera = ViewportCamera;
			if (!workspace || !camera)
				return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidProject", "Project has no valid editor viewport state"));

			if (method == "ConfigureViewport") {
				if (!HasOnlyFields(parameters, {"Width", "Height"}) ||
					!parameters.contains("Width") || !parameters["Width"].is_number_unsigned() ||
					!parameters.contains("Height") || !parameters["Height"].is_number_unsigned())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Viewport dimensions must be unsigned"));
				const auto width = parameters["Width"].get<std::uint32_t>();
				const auto height = parameters["Height"].get<std::uint32_t>();
				if (width == 0 || height == 0 || width > EditorHostMaximumViewportDimension ||
					height > EditorHostMaximumViewportDimension ||
					static_cast<std::uint64_t>(width) * height > EditorHostMaximumViewportPixels)
					return SerializeBoundedResponse(ErrorResponse(requestId, "ViewportTooLarge", "Viewport dimensions exceed protocol limits"));
				const bool firstConfiguration = ViewportWidth == 0 || ViewportHeight == 0;
				ViewportWidth = width;
				ViewportHeight = height;
				{
					ScopedChangeJournalSuppression suppressSessionState;
					camera->SetViewportSize(Vector2(static_cast<float>(width), static_cast<float>(height)));
					if (firstConfiguration)
						camera->SetCFrame(CFrame::lookAt({10.0f, 10.0f, 10.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}));
				}
				if (ViewportRenderer) {
					try {
						ViewportRenderer->Resize(width, height);
					} catch (const std::exception &error) {
						ViewportRenderer.reset();
						return SerializeBoundedResponse(ErrorResponse(requestId, "ViewportUnavailable", error.what()));
					}
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"ViewportVersion", 1}, {"Width", width}, {"Height", height}, {"Format", "RGB8"}
				}));
			}

			if (method == "SetViewportCamera") {
				if (!HasOnlyFields(parameters, {"Position", "Target", "FieldOfView"}) ||
					!parameters.contains("Position") || !parameters.contains("Target"))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Camera Position and Target are required"));
				auto position = DecodeVector3(parameters["Position"]);
				auto target = DecodeVector3(parameters["Target"]);
				if (!position || !target || glm::length(*target - *position) < 1e-4f)
					return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidCamera", "Camera vectors are invalid or coincident"));
				float fieldOfView = camera->GetFieldOfView();
				if (parameters.contains("FieldOfView")) {
					if (!parameters["FieldOfView"].is_number())
						return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidCamera", "FieldOfView must be numeric"));
					fieldOfView = parameters["FieldOfView"].get<float>();
					if (!std::isfinite(fieldOfView) || fieldOfView < 1.0f || fieldOfView > 120.0f)
						return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidCamera", "FieldOfView is out of range"));
				}
				{
					ScopedChangeJournalSuppression suppressSessionState;
					camera->SetCFrame(CFrame::lookAt(*position, *target, {0.0f, 1.0f, 0.0f}));
					camera->SetFieldOfView(fieldOfView);
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"Position", {position->x, position->y, position->z}},
					{"Target", {target->x, target->y, target->z}},
					{"FieldOfView", fieldOfView},
				}));
			}

			if (method == "CaptureViewport") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "CaptureViewport takes no parameters"));
				if (ViewportWidth == 0 || ViewportHeight == 0)
					return SerializeBoundedResponse(ErrorResponse(requestId, "ViewportRequired", "ConfigureViewport must succeed first"));
				try {
					if (!ViewportRenderer)
						ViewportRenderer = std::make_unique<EditorViewportRenderer>(ViewportWidth, ViewportHeight);
					auto frame = ViewportRenderer->Capture({.WorldRoot = workspace, .Camera = camera});
					if (ViewportFrameRing) {
						const auto timestamp = static_cast<std::uint64_t>(
							std::chrono::duration_cast<std::chrono::nanoseconds>(
								std::chrono::steady_clock::now().time_since_epoch()
							).count()
						);
						const auto sequence = ViewportFrameRing->Publish(
							frame.Width, frame.Height, frame.RgbPixels, timestamp
						);
						ViewportFrameNumber = sequence;
						return SerializeBoundedResponse(SuccessResponse(requestId, {
							{"ViewportVersion", 1}, {"FrameNumber", sequence},
							{"Width", frame.Width}, {"Height", frame.Height}, {"Format", "RGB8"},
							{"Transport", "SharedMemoryRing"}, {"TransportVersion", SharedFrameRingLayout::Version},
						}));
					}
					return SerializeBoundedResponse(SuccessResponse(requestId, {
						{"ViewportVersion", 1}, {"FrameNumber", ++ViewportFrameNumber},
						{"Width", frame.Width}, {"Height", frame.Height}, {"Format", "RGB8"},
						{"Transport", "Base64"}, {"Encoding", "Base64"}, {"Pixels", EncodeBase64(frame.RgbPixels)},
					}));
				} catch (const std::exception &error) {
					return SerializeBoundedResponse(ErrorResponse(requestId, "ViewportUnavailable", error.what()));
				}
			}

			if (method == "PickViewport") {
				if (!HasOnlyFields(parameters, {"X", "Y"}) || !parameters.contains("X") ||
					!parameters["X"].is_number() || !parameters.contains("Y") || !parameters["Y"].is_number())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "PickViewport requires numeric X and Y"));
				if (ViewportWidth == 0 || ViewportHeight == 0)
					return SerializeBoundedResponse(ErrorResponse(requestId, "ViewportRequired", "ConfigureViewport must succeed first"));
				auto pick = PickEditorViewport(
					workspace, camera, ViewportWidth, ViewportHeight,
					parameters["X"].get<float>(), parameters["Y"].get<float>()
				);
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"Object", pick ? Json(EncodeWireObjectId(WireObjectId::FromObjectId(pick->Object))) : Json(nullptr)},
					{"Distance", pick ? Json(pick->Distance) : Json(nullptr)},
				}));
			}

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
