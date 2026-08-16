// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/editor/EditorHost.hpp"

#include "gargantuan/InstanceProperty.hpp"
#include "gargantuan/classes/LuaSourceContainer.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/Snapshot.hpp"
#include "gargantuan/runtime/WireCodec.hpp"
#include "gargantuan/runtime/WireJournal.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "serialization/JsonCodec.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <vector>

namespace gargantuan {
	namespace {
		using Json = JsonCodec::Json;

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
			auto Serialized = JsonCodec::Encode(response, "EditorHost response");
			if (Serialized && Serialized->size() <= EditorHostMaximumResponseBytes) return std::move(*Serialized);
			auto Fallback = JsonCodec::Encode(
				ErrorResponse(nullptr, "ResponseTooLarge", "EditorHost response exceeded its byte limit"),
				"EditorHost error response"
			);
			return Fallback ? std::move(*Fallback) :
				R"({"Version":1,"RequestId":null,"Ok":false,"Error":{"Code":"InternalError","Message":"Response encoding failed"}})";
		}

		Json ParseGeneratedJson(std::string_view Encoded, std::string_view Name) {
			auto Parsed = JsonCodec::Parse(Encoded, EditorHostMaximumResponseBytes, Name);
			if (!Parsed) throw std::runtime_error(Parsed.error().Format());
			return std::move(*Parsed);
		}

		const char *MutationStatusName(MutationStatus status) {
			switch (status) {
				case MutationStatus::Success: return "Success";
				case MutationStatus::WrongExecutionDomain: return "WrongExecutionDomain";
				case MutationStatus::StaleObject: return "StaleObject";
				case MutationStatus::InvalidClass: return "InvalidClass";
				case MutationStatus::InvalidProperty: return "InvalidProperty";
				case MutationStatus::InvalidParent: return "InvalidParent";
				case MutationStatus::ProtectedObject: return "ProtectedObject";
				case MutationStatus::ResourceLimit: return "ResourceLimit";
				case MutationStatus::RevisionExhausted: return "RevisionExhausted";
				case MutationStatus::ReadOnly: return "ReadOnly";
				case MutationStatus::Unauthorized: return "Unauthorized";
				case MutationStatus::ValidationFailed: return "ValidationFailed";
				case MutationStatus::Conflict: return "SourceConflict";
				case MutationStatus::Rejected: return "Rejected";
				case MutationStatus::InternalError: return "InternalError";
			case MutationStatus::TransactionNotFound:
				return "TransactionNotFound";
			case MutationStatus::TransactionLimit:
				return "TransactionLimit";
			}
			return "InternalError";
		}

		Json EncodeCursor(ChangeCursor cursor) {
			return Json{
				{"Scope", JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(cursor.Scope))},
				{"NextSequence", cursor.NextSequence},
			};
		}

		Json EncodeDomains(const ScriptDomainSet &domains) {
			Json result = Json::array();
			for (const auto domain : {
				ScriptExecutionDomain::Core,
				ScriptExecutionDomain::PreRun,
				ScriptExecutionDomain::Studio,
				ScriptExecutionDomain::Server,
				ScriptExecutionDomain::Client,
			}) {
				if (domains.Contains(domain)) result.push_back(GetScriptExecutionDomainName(domain));
			}
			return result;
		}

		Json EncodeCapabilities(const ScriptCapabilitySet &capabilities) {
			Json result = Json::array();
			for (const auto capability : {
				ScriptCapability::ReadDataModel,
				ScriptCapability::MutateDataModel,
				ScriptCapability::EditorCommands,
				ScriptCapability::SelectionAccess,
				ScriptCapability::ViewportControl,
				ScriptCapability::FilesystemRead,
				ScriptCapability::FilesystemWrite,
				ScriptCapability::ProcessControl,
				ScriptCapability::NetworkSend,
				ScriptCapability::NetworkReceive,
				ScriptCapability::DefineSchema,
			}) {
				if (capabilities.Contains(capability)) result.push_back(GetScriptCapabilityName(capability));
			}
			return result;
		}

		Json EncodeProjectState(
			const std::shared_ptr<DataModel> &world,
			std::uint64_t persistedRevision,
			const std::optional<Project> &project
		) {
			if (!world || !project) return {
				{"AuthoritativeRevision", 0}, {"PersistedRevision", 0},
				{"Dirty", false}, {"CurrentDestination", nullptr},
				{"History", {{"CanUndo", false}, {"CanRedo", false}, {"UndoLabel", nullptr}, {"RedoLabel", nullptr}}}
			};
			const auto authoritativeRevision = world->GetAuthoritativeRevision();
			const auto History = world->Transactions.GetStatus();
			return {
				{"AuthoritativeRevision", authoritativeRevision},
				{"PersistedRevision", persistedRevision},
				{"Dirty", authoritativeRevision != persistedRevision},
				{"CurrentDestination", project->Root.generic_string()},
				{"History", {
					{"CanUndo", History.CanUndo}, {"CanRedo", History.CanRedo},
					{"UndoLabel", History.UndoLabel ? Json(*History.UndoLabel) : Json(nullptr)},
					{"RedoLabel", History.RedoLabel ? Json(*History.RedoLabel) : Json(nullptr)},
					{"RetainedCount", History.RetainedCount}, {"SemanticBytes", History.SemanticBytes}
				}},
			};
		}

		std::optional<std::filesystem::path> ValidateProjectDestination(const Json &value) {
			if (!value.is_string()) return std::nullopt;
			const auto &encoded = value.get_ref<const std::string &>();
			if (encoded.empty() || encoded.size() > 32768 || encoded.find('\0') != std::string::npos ||
				encoded.find("://") != std::string::npos)
				return std::nullopt;
			std::filesystem::path candidate(encoded);
			if (!candidate.is_absolute() || candidate.filename().empty()) return std::nullopt;
			std::error_code error;
			auto normalized = std::filesystem::weakly_canonical(candidate, error);
			if (error || normalized.empty() || !normalized.is_absolute()) return std::nullopt;
			if (std::filesystem::exists(normalized, error) && !std::filesystem::is_directory(normalized, error))
				return std::nullopt;
			return normalized;
		}

		bool IsValidProjectName(std::string_view Name) {
			if (Name.empty() || Name.size() > 100 || !IsValidProtocolUtf8(Name) ||
				Name.find('\0') != std::string_view::npos)
				return false;
			bool HasVisibleCharacter = false;
			for (const auto Character : Name) {
				const auto Byte = static_cast<unsigned char>(Character);
				if (Byte < 0x20 || Byte == 0x7f) return false;
				if (Byte >= 0x80 || !std::isspace(Byte)) HasVisibleCharacter = true;
			}
			return HasVisibleCharacter;
		}

		std::filesystem::path CreateOwnedStagingDirectory(const std::filesystem::path &Destination) {
			static std::atomic_uint64_t Counter = 1;
			for (std::size_t Attempt = 0; Attempt < 32; ++Attempt) {
				const auto Candidate = Destination.parent_path() / std::format(
					".{}.creating-{}-{}", Destination.filename().string(),
					std::chrono::steady_clock::now().time_since_epoch().count(),
					Counter.fetch_add(1, std::memory_order_relaxed)
				);
				std::error_code Error;
				if (std::filesystem::create_directory(Candidate, Error)) return Candidate;
				if (Error && Error != std::errc::file_exists) throw std::filesystem::filesystem_error(
					"Could not create project staging directory", Candidate, Error
				);
			}
			throw std::runtime_error("Could not allocate a unique project staging directory");
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

	EditorHost::EditorHost(std::string sessionToken) :
		EditorHost(std::move(sessionToken), ScriptSecurityContext::StudioCoreUi()) {}

	EditorHost::EditorHost(std::string sessionToken, ScriptSecurityContext studioSecurity) :
		SessionToken(std::move(sessionToken)), StudioSecurity(std::move(studioSecurity)) {
		if (SessionToken.empty() || SessionToken.size() > 256)
			throw std::invalid_argument("EditorHost requires a nonempty bounded session token");
	}

	EditorHost::~EditorHost() {
		try {
			if (World) (void)World->Transactions.TerminateOwner(*World, TransactionOwner);
		} catch (...) {
			if (World) World->Transactions.Reset();
		}
	}

	std::string EditorHost::HandleRequest(std::string_view request) {
		Json requestId = nullptr;
		try {
			if (request.empty() || request.size() > EditorHostMaximumRequestBytes)
				return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Request byte length is invalid"));
			try {
				ValidateProtocolJsonDocument(request, EditorHostMaximumRequestBytes);
			} catch (const std::invalid_argument &Error) {
				return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", Error.what()));
			}

			auto ParsedMessage = JsonCodec::Parse(request, EditorHostMaximumRequestBytes, "EditorHost request");
			if (!ParsedMessage)
				return SerializeBoundedResponse(ErrorResponse(
					requestId, "MalformedRequest", ParsedMessage.error().Format()
				));
			auto message = std::move(*ParsedMessage);
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
			if (World) (void)World->Transactions.ExpireOwner(*World, TransactionOwner);

			const auto method = message["Method"].get<std::string>();
			const auto &parameters = message["Params"];
			std::optional<TransactionId> RequestTransaction;
			if (parameters.contains("TransactionId")) {
				if (!parameters["TransactionId"].is_string())
					return SerializeBoundedResponse(
						ErrorResponse(requestId, "MalformedRequest", "TransactionId must be a canonical decimal string")
					);
				const auto &Encoded = parameters["TransactionId"].get_ref<const std::string &>();
				std::uint64_t Value = 0;
				const auto [End, Error] = std::from_chars(Encoded.data(), Encoded.data() + Encoded.size(), Value);
				if (Encoded.empty() || Encoded.front() == '0' || Error != std::errc{} ||
					End != Encoded.data() + Encoded.size() || Value == 0)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "TransactionId must be a canonical nonzero decimal string"
					));
				RequestTransaction = TransactionId{Value};
			}
			const bool viewportMethod = method == "OpenViewportTransport" || method == "CloseViewportTransport" ||
				method == "ConfigureViewport" || method == "SetViewportCamera" ||
				method == "CaptureViewport" || method == "PickViewport";
			if (viewportMethod && !StudioSecurity.HasCapability(ScriptCapability::ViewportControl))
				return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Viewport access requires ViewportControl"));
			if (method == "Handshake") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Handshake takes no parameters"));
				Json capabilities = {
					"OpenProject", "CreateProject", "Schema", "Snapshot", "Journal", "SetProperty", "SetAttribute", "SetExtensionProperty",
					"AddTag", "RemoveTag", "SaveProject", "SaveProjectAs", "AuthoritativeRevision",
					"CreateInstance", "DestroyInstance", "DuplicateInstance", "ReparentInstance",
					"BeginTransaction",
					"CommitTransaction",
					"AuthoritativeTransactions",
					"Undo", "Redo", "AuthoritativeHistoryStatus",
					"ReadScriptSource", "WriteScriptSource",
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
						{"ScriptSecurityVersion", 1},
						{"StudioExecutionDomain", GetScriptExecutionDomainName(StudioSecurity.Domain)},
						{"StudioCapabilities", EncodeCapabilities(StudioSecurity.Capabilities)},
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
				if (World) {
					(void)World->Transactions.TerminateOwner(*World, TransactionOwner); World->Destroy();
				}
				World.reset();
				Filesystem.reset();
				CurrentProject.reset();
				PersistedRevision = 0;
				ViewportCamera.reset();
				LastViewportSnapshot.reset();
				Cursor.reset();
				ViewportWidth = 0;
				ViewportHeight = 0;
				ViewportFrameNumber = 0;
				BootstrapProjectRuntimeSchema(root);
				auto filesystem = std::make_unique<DiskFilesystem>(root);
				auto project = Project::fromExisting(filesystem.get());
				auto world = project.DeserializeGame();
				world->InitializeLoadedProjectRevision();
				Filesystem = std::move(filesystem);
				CurrentProject = std::move(project);
				World = std::move(world);
				World->Filesystem = Filesystem.get();
				PersistedRevision = World->GetAuthoritativeRevision();
				auto workspace = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
				if (!workspace)
					return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidProject", "Project has no valid Workspace"));
				ViewportCamera = RenderCameraInput{};
				return SerializeBoundedResponse(SuccessResponse(
					requestId,
					{
						{"Root", JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(World->GetObjectId()))},
						{"ProjectState", EncodeProjectState(World, PersistedRevision, CurrentProject)},
					}
				));
			}

			if (method == "CreateProject") {
				if (!StudioSecurity.HasCapability(ScriptCapability::EditorCommands))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Unauthorized", "Project creation requires EditorCommands"
					));
				if (!HasOnlyFields(parameters, {"Destination", "Name"}) ||
					!parameters.contains("Destination") || !parameters.contains("Name") ||
					!parameters["Name"].is_string())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "CreateProject requires Destination and Name"
					));
				const auto Name = parameters["Name"].get<std::string>();
				if (!IsValidProjectName(Name))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "InvalidProjectName", "Project Name must be valid visible UTF-8 within 100 bytes"
					));
				auto Destination = ValidateProjectDestination(parameters["Destination"]);
				if (!Destination)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "InvalidDestination", "Project destination is not a normalized absolute local directory path"
					));
				std::error_code FilesystemError;
				if (std::filesystem::exists(*Destination, FilesystemError)) {
					const auto ProjectConfiguration = *Destination / ".gargantuan";
					const bool ExistingProject =
						std::filesystem::is_regular_file(ProjectConfiguration / "project.instance.json", FilesystemError) ||
						std::filesystem::is_regular_file(ProjectConfiguration / "project.instance.bin", FilesystemError);
					return SerializeBoundedResponse(ErrorResponse(
						requestId,
						ExistingProject ? "ExistingProject" : "DestinationExists",
						"New Project requires a destination that does not already exist"
					));
				}
				if (FilesystemError || !std::filesystem::is_directory(Destination->parent_path(), FilesystemError))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "InvalidDestination", "Project destination parent is not an accessible directory"
					));
				if (World && World->Transactions.GetOpenCount() != 0)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "TransactionOpen", "Commit the open authoring transaction before creating a project"
					));

				std::filesystem::path Staging;
				bool StagingOwned = false;
				try {
					Staging = CreateOwnedStagingDirectory(*Destination);
					StagingOwned = true;
					auto StagingFilesystem = std::make_unique<DiskFilesystem>(Staging);
					auto StagingProject = Project::forDestination(
						StagingFilesystem.get(), InstanceSerialization::InstanceFormat::Json
					);
					auto NewWorld = std::make_shared<DataModel>();
					NewWorld->SetName(Name);
					if (!std::dynamic_pointer_cast<Workspace>(NewWorld->GetService("Workspace")))
						throw std::runtime_error("The canonical new project could not create Workspace");
					NewWorld->MarkPersistenceSubtreeArchivable();
					NewWorld->Root = Staging;
					NewWorld->Filesystem = StagingFilesystem.get();
					NewWorld->InitializeLoadedProjectRevision();
					auto Snapshot = StagingProject.CaptureGame(NewWorld, NewWorld->GetAuthoritativeRevision());
					StagingProject.PersistGameAtomically(Snapshot, PersistenceCheckpointForTesting);
					NewWorld->Destroy();
					NewWorld.reset();
					std::filesystem::rename(Staging, *Destination);
					StagingOwned = false;
				} catch (const std::filesystem::filesystem_error &) {
					if (StagingOwned) {
						std::error_code Ignored;
						std::filesystem::remove_all(Staging, Ignored);
					}
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "FilesystemFailure", "The new project could not be established atomically"
					));
				} catch (const std::exception &) {
					if (StagingOwned) {
						std::error_code Ignored;
						std::filesystem::remove_all(Staging, Ignored);
					}
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "PersistenceFailure", "The new project could not be serialized and persisted"
					));
				}

				try {
					if (World) {
						(void)World->Transactions.TerminateOwner(*World, TransactionOwner);
						World->Destroy();
					}
					World.reset();
					Filesystem.reset();
					CurrentProject.reset();
					PersistedRevision = 0;
					ViewportCamera.reset();
					LastViewportSnapshot.reset();
					Cursor.reset();
					ViewportWidth = 0;
					ViewportHeight = 0;
					ViewportFrameNumber = 0;
					BootstrapProjectRuntimeSchema(*Destination);
					auto NewFilesystem = std::make_unique<DiskFilesystem>(*Destination);
					auto NewProject = Project::fromExisting(NewFilesystem.get());
					auto LoadedWorld = NewProject.DeserializeGame();
					LoadedWorld->InitializeLoadedProjectRevision();
					Filesystem = std::move(NewFilesystem);
					CurrentProject = std::move(NewProject);
					World = std::move(LoadedWorld);
					World->Filesystem = Filesystem.get();
					PersistedRevision = World->GetAuthoritativeRevision();
					if (!std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace")))
						throw std::runtime_error("New project has no valid Workspace");
					ViewportCamera = RenderCameraInput{};
				} catch (const std::exception &) {
					if (World) {
						try { World->Destroy(); }
						catch (...) {}
					}
					World.reset();
					Filesystem.reset();
					CurrentProject.reset();
					PersistedRevision = 0;
					ViewportCamera.reset();
					Cursor.reset();
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "ProjectActivationFailure", "The persisted new project could not be activated"
					));
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"Root", JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(World->GetObjectId()))},
					{"ProjectState", EncodeProjectState(World, PersistedRevision, CurrentProject)},
				}));
			}

			if (method == "GetProjectState") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GetProjectState takes no parameters"));
				if (!World || !CurrentProject)
					return SerializeBoundedResponse(ErrorResponse(requestId, "NoProjectLoaded", "No project is loaded"));
				return SerializeBoundedResponse(SuccessResponse(
					requestId, EncodeProjectState(World, PersistedRevision, CurrentProject)
				));
			}

			if (method == "BeginTransaction") {
				if (!StudioSecurity.HasCapability(ScriptCapability::MutateDataModel))
					return SerializeBoundedResponse(
						ErrorResponse(requestId, "Unauthorized", "Transactions require MutateDataModel")
					);
				if (!World || !CurrentProject)
					return SerializeBoundedResponse(
						ErrorResponse(requestId, "NoProjectLoaded", "No project is loaded")
					);
				if (!HasOnlyFields(parameters, {"Label"}) || !parameters.contains("Label") ||
					!parameters["Label"].is_string())
					return SerializeBoundedResponse(
						ErrorResponse(requestId, "MalformedRequest", "BeginTransaction requires a bounded Label")
					);
				auto Result = World->Transactions.Begin(
					*World, TransactionOwner, parameters["Label"].get<std::string>(), TransactionOrigin::Studio
				);
				if (!Result.Succeeded())
					return SerializeBoundedResponse(ErrorResponse(
						requestId,
						Result.Status == TransactionStatus::LimitExceeded ? "TransactionLimit" : "TransactionRejected",
						Result.Message
					));
				return SerializeBoundedResponse(SuccessResponse(
					requestId,
					{
						{"TransactionId", std::to_string(Result.Id.Value)},
						{"Status", "Open"},
						{"StartingRevision", Result.StartingRevision},
					}
				));
			}

			if (method == "CommitTransaction") {
				if (!StudioSecurity.HasCapability(ScriptCapability::MutateDataModel))
					return SerializeBoundedResponse(
						ErrorResponse(requestId, "Unauthorized", "Transactions require MutateDataModel")
					);
				if (!World || !CurrentProject)
					return SerializeBoundedResponse(
						ErrorResponse(requestId, "NoProjectLoaded", "No project is loaded")
					);
				if (!HasOnlyFields(parameters, {"TransactionId"}) || !RequestTransaction)
					return SerializeBoundedResponse(
						ErrorResponse(requestId, "MalformedRequest", "CommitTransaction requires TransactionId")
					);
				auto Result = World->Transactions.Commit(*World, *RequestTransaction, TransactionOwner);
				if (!Result.Succeeded())
					return SerializeBoundedResponse(ErrorResponse(
						requestId,
						Result.Status == TransactionStatus::NotFound			? "TransactionNotFound"
						: Result.Status == TransactionStatus::WrongOwner		? "WrongTransactionOwner"
						: Result.Status == TransactionStatus::RevisionExhausted ? "RevisionExhausted"
																				: "TransactionRejected",
						Result.Message
					));
				return SerializeBoundedResponse(SuccessResponse(
					requestId,
					{
						{"TransactionId", std::to_string(Result.Id.Value)},
						{"Status", Result.Status == TransactionStatus::NoChanges ? "NoChanges" : "Committed"},
						{"StartingRevision", Result.StartingRevision},
						{"ResultingRevision", Result.ResultingRevision},
						{"ChangeCount", Result.ChangeCount},
						{"ProjectState", EncodeProjectState(World, PersistedRevision, CurrentProject)},
					}
				));
			}

			if (method == "Undo" || method == "Redo") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "Undo and Redo take no parameters"
					));
				if (!StudioSecurity.HasCapability(ScriptCapability::MutateDataModel))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Unauthorized", "History execution requires MutateDataModel"
					));
				if (!World || !CurrentProject)
					return SerializeBoundedResponse(ErrorResponse(requestId, "NoProjectLoaded", "No project is loaded"));
				if (!Cursor)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "SnapshotRequired", "GetSnapshot must establish a journal cursor"
					));
				auto Result = method == "Undo" ? Mutations.Undo(*World, StudioSecurity) : Mutations.Redo(*World, StudioSecurity);
				if (!Result.Succeeded()) {
					const char *Code = Result.Status == TransactionStatus::NothingToUndo ? "NothingToUndo" :
						Result.Status == TransactionStatus::NothingToRedo ? "NothingToRedo" :
						Result.Status == TransactionStatus::RevisionExhausted ? "RevisionExhausted" :
						Result.Status == TransactionStatus::InvalidState ? "TransactionOpen" : "HistoryExecutionFailed";
					return SerializeBoundedResponse(ErrorResponse(requestId, Code, Result.Message));
				}
				const auto Status = World->Transactions.GetStatus();
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"TransactionId", std::to_string(Result.Id.Value)},
					{"Action", method}, {"ResultingRevision", Result.ResultingRevision},
					{"CanUndo", Status.CanUndo}, {"CanRedo", Status.CanRedo},
					{"UndoLabel", Status.UndoLabel ? Json(*Status.UndoLabel) : Json(nullptr)},
					{"RedoLabel", Status.RedoLabel ? Json(*Status.RedoLabel) : Json(nullptr)},
					{"ProjectState", EncodeProjectState(World, PersistedRevision, CurrentProject)},
				}));
			}

			if (method == "SaveProject" || method == "SaveProjectAs") {
				if (!StudioSecurity.HasCapability(ScriptCapability::EditorCommands))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Project persistence requires EditorCommands"));
				if (!World || !CurrentProject || !Filesystem)
					return SerializeBoundedResponse(ErrorResponse(requestId, "NoProjectLoaded", "No project is loaded")
					);
				if (World->Transactions.GetOpenCount() != 0)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "TransactionOpen", "Commit the open authoring transaction before saving"));
				const bool saveAs = method == "SaveProjectAs";
				if ((!saveAs && !parameters.empty()) ||
					(saveAs && (!HasOnlyFields(parameters, {"Destination"}) || !parameters.contains("Destination"))))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Project persistence parameters are invalid"));

				std::unique_ptr<DiskFilesystem> destinationFilesystem;
				std::optional<Project> destinationProject;
				if (saveAs) {
					auto destination = ValidateProjectDestination(parameters["Destination"]);
					if (!destination)
						return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidDestination", "Save As destination is not a valid local project directory"));
					destinationFilesystem = std::make_unique<DiskFilesystem>(*destination);
					destinationProject.emplace(Project::forDestination(
						destinationFilesystem.get(), CurrentProject->InstanceFileFormat
					));
				}
				auto &project = saveAs ? *destinationProject : *CurrentProject;
				Project::PersistenceSnapshot snapshot;
				try {
					snapshot = project.CaptureGame(World, World->GetAuthoritativeRevision());
				} catch (const std::exception &) {
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "SerializationFailure", "The authoritative project could not be serialized"
					));
				}
				try {
					if (saveAs && project.Root != CurrentProject->Root) {
						const auto sourcePreRun = CurrentProject->RootConfiguration / "prerun.luau";
						const auto destinationPreRun = project.RootConfiguration / "prerun.luau";
						if (std::filesystem::is_regular_file(sourcePreRun)) {
							std::filesystem::create_directories(project.RootConfiguration);
							std::filesystem::copy_file(
								sourcePreRun, destinationPreRun,
								std::filesystem::copy_options::overwrite_existing
							);
						}
					}
					project.PersistGameAtomically(snapshot, PersistenceCheckpointForTesting);
					PersistedRevision = snapshot.Revision;
					if (saveAs) {
						Filesystem = std::move(destinationFilesystem);
						CurrentProject = std::move(destinationProject);
						World->Root = CurrentProject->Root;
						World->Filesystem = Filesystem.get();
					}
					Json result = EncodeProjectState(World, PersistedRevision, CurrentProject);
					result["PersistedRevision"] = snapshot.Revision;
					return SerializeBoundedResponse(SuccessResponse(requestId, std::move(result)));
				} catch (const std::filesystem::filesystem_error &) {
					return SerializeBoundedResponse(ErrorResponse(requestId, "FilesystemFailure", "The project destination could not be written atomically"));
				} catch (const std::exception &) {
					return SerializeBoundedResponse(ErrorResponse(requestId, "PersistenceFailure", "The project could not be persisted atomically"));
				}
			}

			if (method == "GetSchema") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GetSchema takes no parameters"));
				if (!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Schema access requires ReadDataModel"));
				auto IsEditorConstructible = [&](const InstanceClassDefinition &Definition) {
					return InstanceClassRegistry::IsConstructible(Definition) && Definition.EditorVisible &&
						Definition.ClassName != "DataModel" && (!World || !World->IsProtectedServiceClass(Definition.Id));
				};
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
							{"ReadDomains", EncodeDomains(property->ReadDomains)},
							{"WriteDomains", EncodeDomains(property->WriteDomains)},
							{"RequiredReadCapability", GetScriptCapabilityName(property->RequiredReadCapability)},
							{"RequiredWriteCapability", GetScriptCapabilityName(property->RequiredWriteCapability)},
						};
						if (auto defaultValue = EncodeNativeWireValue(property->Unmodified))
							encoded["Default"] = JsonCodec::EncodeWireValue(*defaultValue);
						properties.push_back(std::move(encoded));
					}
					classes.push_back({
						{"Name", definition->ConstructionKind == SchemaClassConstructionKind::CustomData
							? definition->CanonicalName : definition->ClassName},
						{"Description", definition->Description},
						{"Superclass", definition->Superclass ? Json(*definition->Superclass) : Json(nullptr)},
						{"Constructible", IsEditorConstructible(*definition)},
						{"Properties", std::move(properties)},
					});
				}
				Json definitions = Json::array();
				for (const auto *entry : GetActiveRuntimeSchemaRegistry().EnumerateDefinitions()) {
					const auto kind = GetSchemaDefinitionKind(*entry);
					Json encoded{
						{"SchemaId", GetSchemaDefinitionId(*entry).ToString()},
						{"Kind", kind == SchemaDefinitionKind::Class ? "Class" :
							kind == SchemaDefinitionKind::Enum ? "Enum" : "Extension"},
						{"Namespace", GetSchemaDefinitionNamespace(*entry)},
						{"Name", GetSchemaDefinitionName(*entry)},
						{"CanonicalName", GetSchemaDefinitionCanonicalName(*entry)},
						{"DefinitionVersion", GetSchemaDefinitionVersion(*entry)},
						{"Provenance", GetSchemaProvenanceName(GetSchemaDefinitionProvenance(*entry))},
					};
					if (const auto *classDefinition = std::get_if<SchemaClassDefinition>(entry)) {
						encoded["BaseSchemaId"] = classDefinition->BaseSchemaId
							? Json(classDefinition->BaseSchemaId->ToString()) : Json(nullptr);
						encoded["ConstructionKind"] = classDefinition->ConstructionKind == SchemaClassConstructionKind::Native
							? "Native" : "CustomData";
						encoded["CustomSubclassPolicy"] = classDefinition->ProjectSubclassPolicy == CustomSubclassPolicy::DataOnly
							? "DataOnly" : "Forbidden";
						encoded["NativeHostClassSchemaId"] = classDefinition->ConstructionKind == SchemaClassConstructionKind::CustomData
							? Json(classDefinition->NativeHostClassId.ToString()) : Json(nullptr);
					encoded["Constructible"] = IsEditorConstructible(*classDefinition);
						Json properties = Json::array();
						for (const auto &property : classDefinition->DeclaredCustomProperties) {
							properties.push_back({
								{"Name", property.Name},
								{"CanonicalName", property.CanonicalName},
								{"Type", GetSchemaExtensionPropertyTypeName(property.Type)},
								{"Default", JsonCodec::EncodeWireValue(property.DefaultValue)},
								{"Readable", true},
								{"Writable", true},
								{"Editable", property.Editable},
								{"Persistence", "Saved"},
								{"Replication", "Replicated"},
								{"Authority", "Main"},
								{"RequiredReadCapability", "ReadDataModel"},
								{"RequiredWriteCapability", "MutateDataModel"},
							});
						}
						encoded["Properties"] = std::move(properties);
					} else if (const auto *enumDefinition = std::get_if<SchemaEnumDefinition>(entry)) {
						Json items = Json::array();
						for (const auto &item : enumDefinition->Items)
							items.push_back({{"Name", item.Name}, {"Value", item.Value}});
						encoded["Items"] = std::move(items);
					} else if (const auto *extension = std::get_if<SchemaExtensionDefinition>(entry)) {
						encoded["TargetClassSchemaId"] = extension->TargetClassId.ToString();
						Json properties = Json::array();
						for (const auto &property : extension->Properties) {
							properties.push_back({
								{"Name", property.Name},
								{"CanonicalName", property.CanonicalName},
								{"Type", GetSchemaExtensionPropertyTypeName(property.Type)},
								{"Default", JsonCodec::EncodeWireValue(property.DefaultValue)},
								{"Readable", true},
								{"Writable", true},
								{"Editable", property.Editable},
								{"Persistence", "Saved"},
								{"Replication", "Replicated"},
								{"Authority", "Main"},
								{"RequiredReadCapability", "ReadDataModel"},
								{"RequiredWriteCapability", "MutateDataModel"},
							});
						}
						encoded["Properties"] = std::move(properties);
					}
					definitions.push_back(std::move(encoded));
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"SchemaDiscoveryVersion", 4},
					{"RegistryGeneration", GetRuntimeSchemaLifecycle().GetActiveGeneration()},
					{"Definitions", std::move(definitions)},
					{"Classes", std::move(classes)},
				}));
			}

			if (method == "GetScriptSource" &&
				(!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel) ||
					!StudioSecurity.HasCapability(ScriptCapability::EditorCommands)))
				return SerializeBoundedResponse(ErrorResponse(
					requestId, "Unauthorized", "Script source access requires EditorCommands and ReadDataModel"
				));
			if (method == "SetScriptSource" &&
				(!StudioSecurity.HasCapability(ScriptCapability::MutateDataModel) ||
					!StudioSecurity.HasCapability(ScriptCapability::EditorCommands)))
				return SerializeBoundedResponse(ErrorResponse(
					requestId, "Unauthorized", "Script source mutation requires EditorCommands and MutateDataModel"
				));
			if (!World)
				return SerializeBoundedResponse(ErrorResponse(requestId, "ProjectRequired", "OpenProject must succeed first"));
			auto StudioMutationAuthority = [&] {
				return MutationAuthorityContext::Studio(
					StudioSecurity, World->GetObjectId(), RequestTransaction, TransactionOwner
				);
			};

			auto workspace = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
			if (!workspace || !ViewportCamera)
				return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidProject", "Project has no valid editor viewport state"));
			auto &camera = *ViewportCamera;

			if (method == "ConfigureViewport") {
				if (!HasOnlyFields(parameters, {"Width", "Height"}) ||
					!parameters.contains("Width") || !parameters["Width"].is_number_unsigned() ||
					!parameters.contains("Height") || !parameters["Height"].is_number_unsigned())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Viewport dimensions must be unsigned"));
				auto width = JsonCodec::DecodeUnsigned32(parameters["Width"]);
				auto height = JsonCodec::DecodeUnsigned32(parameters["Height"]);
				if (!width || !height || *width == 0 || *height == 0 ||
					*width > EditorHostMaximumViewportDimension || *height > EditorHostMaximumViewportDimension ||
					static_cast<std::uint64_t>(*width) * *height > EditorHostMaximumViewportPixels)
					return SerializeBoundedResponse(ErrorResponse(requestId, "ViewportTooLarge", "Viewport dimensions exceed protocol limits"));
				const bool firstConfiguration = ViewportWidth == 0 || ViewportHeight == 0;
				ViewportWidth = *width;
				ViewportHeight = *height;
				if (firstConfiguration)
					camera = MakeLookAtRenderCameraInput(
						{10.0f, 10.0f, 10.0f},
						{0.0f, 0.0f, 0.0f},
						{0.0f, 1.0f, 0.0f},
						camera.VerticalFieldOfView
					);
				LastViewportSnapshot.reset();
				if (ViewportRenderer) {
					try {
						ViewportRenderer->Resize(*width, *height);
					} catch (const std::exception &error) {
						ViewportRenderer.reset();
						return SerializeBoundedResponse(ErrorResponse(requestId, "ViewportUnavailable", error.what()));
					}
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"ViewportVersion", 1}, {"Width", *width}, {"Height", *height}, {"Format", "RGB8"}
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
				float fieldOfView = camera.VerticalFieldOfView;
				if (parameters.contains("FieldOfView")) {
					if (!parameters["FieldOfView"].is_number())
						return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidCamera", "FieldOfView must be numeric"));
					fieldOfView = parameters["FieldOfView"].get<float>();
					if (!std::isfinite(fieldOfView) || fieldOfView < 1.0f || fieldOfView > 120.0f)
						return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidCamera", "FieldOfView is out of range"));
				}
				camera = MakeLookAtRenderCameraInput(*position, *target, {0.0f, 1.0f, 0.0f}, fieldOfView);
				LastViewportSnapshot.reset();
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
					LastViewportSnapshot = ViewportExtractor.Extract(
						*workspace, camera, ViewportWidth, ViewportHeight
					);
					auto frame = ViewportRenderer->Capture(LastViewportSnapshot);
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
				if (!LastViewportSnapshot)
					LastViewportSnapshot = ViewportExtractor.Extract(
						*workspace, camera, ViewportWidth, ViewportHeight
					);
				auto pick = PickEditorViewport(
					*LastViewportSnapshot, parameters["X"].get<float>(), parameters["Y"].get<float>()
				);
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"Object", pick ? Json(JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(pick->Object))) : Json(nullptr)},
					{"Distance", pick ? Json(pick->Distance) : Json(nullptr)},
				}));
			}

			if (method == "GetSnapshot") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GetSnapshot takes no parameters"));
				if (!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Snapshot access requires ReadDataModel"));
				if (!World) return SerializeBoundedResponse(ErrorResponse(requestId, "NoProjectLoaded", "No project is loaded"));
				auto snapshot = CaptureSnapshot(World);
				Cursor = snapshot.Cursor;
				return SerializeBoundedResponse(SuccessResponse(
					requestId,
					{
						{"Snapshot", ParseGeneratedJson(SerializeSnapshot(snapshot), "Snapshot response")},
						{"ProjectState", EncodeProjectState(World, PersistedRevision, CurrentProject)},
					}
				));
			}

			if (method == "PollChanges") {
				if (!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Journal access requires ReadDataModel"));
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
				auto wire = ParseGeneratedJson(SerializeWireJournalRecords(records), "Journal response");
				return SerializeBoundedResponse(SuccessResponse(
					requestId,
					{
						{"Records", std::move(wire["Records"])},
						{"Cursor", EncodeCursor(*Cursor)},
						{"ProjectState", EncodeProjectState(World, PersistedRevision, CurrentProject)},
					}
				));
			}

			if (method == "GetScriptSource") {
				if (!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel) ||
					!StudioSecurity.HasCapability(ScriptCapability::EditorCommands))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Unauthorized", "Script source access requires EditorCommands and ReadDataModel"
					));
				if (!HasOnlyFields(parameters, {"Object"}) || !parameters.contains("Object"))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "GetScriptSource requires an Object"
					));
				auto Object = JsonCodec::DecodeObjectId(parameters["Object"]);
				if (!Object)
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "ObjectId is invalid"));
				auto Target = ObjectRegistry::Get().Lookup(Object->ToObjectId());
				if (!Target || Target->GetReplicationScopeId() != World->GetObjectId())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "StaleObject", "Script identity is not live in the open project"
					));
				auto ScriptValue = std::dynamic_pointer_cast<LuaSourceContainer>(Target);
				if (!ScriptValue)
					return SerializeBoundedResponse(ErrorResponse(requestId, "NotScript", "Object is not a supported script"));
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"Object", JsonCodec::EncodeObjectId(*Object)},
					{"Source", ScriptValue->GetSource()},
					{"SourceVersion", ScriptValue->GetSourceVersion()},
					{"AuthoritativeRevision", World->GetAuthoritativeRevision()},
				}));
			}

			if (method == "SetScriptSource") {
				if (!StudioSecurity.HasCapability(ScriptCapability::MutateDataModel) ||
					!StudioSecurity.HasCapability(ScriptCapability::EditorCommands))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Unauthorized", "Script source mutation requires EditorCommands and MutateDataModel"
					));
				if (!Cursor)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"
					));
				if (!HasOnlyFields(parameters, {"Object", "ExpectedSourceVersion", "Source", "TransactionId"}) ||
					!parameters.contains("Object") || !parameters.contains("ExpectedSourceVersion") ||
					!parameters["ExpectedSourceVersion"].is_number_unsigned() ||
					!parameters.contains("Source") || !parameters["Source"].is_string())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "SetScriptSource fields are invalid"
					));
				auto Object = JsonCodec::DecodeObjectId(parameters["Object"]);
				const auto EncodedVersion = parameters["ExpectedSourceVersion"].get<std::uint64_t>();
				const auto &Source = parameters["Source"].get_ref<const std::string &>();
				if (!Object || EncodedVersion == 0 || EncodedVersion > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "ObjectId or expected source version is invalid"
					));
				try {
					ValidateProtocolString(Source, MaximumScriptSourceBytes, "Script source");
				} catch (const std::invalid_argument &Error) {
					return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidSource", Error.what()));
				}
				auto Target = ObjectRegistry::Get().Lookup(Object->ToObjectId());
				if (!Target || Target->GetReplicationScopeId() != World->GetObjectId())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "StaleObject", "Script identity is not live in the open project"
					));
				auto ScriptValue = std::dynamic_pointer_cast<LuaSourceContainer>(Target);
				if (!ScriptValue)
					return SerializeBoundedResponse(ErrorResponse(requestId, "NotScript", "Object is not a supported script"));
				auto Mutation = Mutations.Apply(UpdateScriptSourceCommand{
					Object->ToObjectId(), static_cast<int>(EncodedVersion), Source
				}, StudioMutationAuthority());
				if (!Mutation.Succeeded())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, MutationStatusName(Mutation.Status), Mutation.Message
					));
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"Status", MutationStatusName(Mutation.Status)},
					{"Object", JsonCodec::EncodeObjectId(*Object)},
					{"SourceVersion", ScriptValue->GetSourceVersion()},
					{"AuthoritativeRevision", World->GetAuthoritativeRevision()},
				}));
			}

			if (method == "SetProperty") {
				if (!StudioSecurity.HasCapability(ScriptCapability::MutateDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "SetProperty requires MutateDataModel"));
				if (!Cursor)
					return SerializeBoundedResponse(ErrorResponse(requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"));
				if (!HasOnlyFields(parameters, {"Object", "Property", "Value", "TransactionId"}) ||
					!parameters.contains("Object") || !parameters.contains("Property") ||
					!parameters["Property"].is_string() || parameters["Property"].get_ref<const std::string &>().empty() ||
					parameters["Property"].get_ref<const std::string &>().size() > 256 || !parameters.contains("Value"))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "SetProperty fields are invalid"));
				auto object = JsonCodec::DecodeObjectId(parameters["Object"]);
				auto value = JsonCodec::DecodeWireValue(parameters["Value"]);
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
				},
					StudioMutationAuthority());
				Json result{
					{"Status", MutationStatusName(mutation.Status)},
					{"Message", mutation.Message},
				};
				if (mutation.Object) result["Object"] = JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(*mutation.Object));
				return SerializeBoundedResponse(
					mutation.Succeeded() ? SuccessResponse(requestId, std::move(result))
										: ErrorResponse(requestId, MutationStatusName(mutation.Status), mutation.Message)
				);
			}

			if (method == "SetAttribute") {
				if (!StudioSecurity.HasCapability(ScriptCapability::MutateDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "SetAttribute requires MutateDataModel"));
				if (!Cursor)
					return SerializeBoundedResponse(ErrorResponse(requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"));
				if (!HasOnlyFields(parameters, {"Object", "Attribute", "Value", "TransactionId"}) ||
					!parameters.contains("Object") || !parameters.contains("Attribute") ||
					!parameters["Attribute"].is_string() || !parameters.contains("Value"))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "SetAttribute fields are invalid"));
				auto object = JsonCodec::DecodeObjectId(parameters["Object"]);
				auto value = JsonCodec::DecodeWireValue(parameters["Value"]);
				if (!object || !value)
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "SetAttribute Object or WireValue is invalid"));
				auto target = ObjectRegistry::Get().Lookup(object->ToObjectId());
				if (!target || target->GetReplicationScopeId() != World->GetObjectId())
					return SerializeBoundedResponse(ErrorResponse(requestId, "StaleObject", "Object is not live in the open project"));
				std::optional<WireValue> attributeValue = std::move(*value);
				if (std::holds_alternative<std::monostate>(*attributeValue)) attributeValue.reset();
				auto mutation = Mutations.Apply(UpdateAttributeCommand{
					object->ToObjectId(), parameters["Attribute"].get<std::string>(), std::move(attributeValue)
				},
					StudioMutationAuthority());
				Json result{{"Status", MutationStatusName(mutation.Status)}, {"Message", mutation.Message}};
				if (mutation.Object) result["Object"] = JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(*mutation.Object));
				return SerializeBoundedResponse(
					mutation.Succeeded() ? SuccessResponse(requestId, std::move(result))
										: ErrorResponse(requestId, MutationStatusName(mutation.Status), mutation.Message)
				);
			}

			if (method == "SetExtensionProperty") {
				if (!StudioSecurity.HasCapability(ScriptCapability::MutateDataModel))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Unauthorized", "SetExtensionProperty requires MutateDataModel"
					));
				if (!Cursor)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"
					));
				if (!HasOnlyFields(parameters, {
						"Object", "ExtensionSchemaId", "DefinitionVersion", "Property", "Value", "TransactionId"
					}) || !parameters.contains("Object") || !parameters.contains("ExtensionSchemaId") ||
					!parameters["ExtensionSchemaId"].is_string() || !parameters.contains("DefinitionVersion") ||
					!parameters["DefinitionVersion"].is_number_unsigned() || !parameters.contains("Property") ||
					!parameters["Property"].is_string() || !parameters.contains("Value"))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "SetExtensionProperty fields are invalid"
					));
				auto object = JsonCodec::DecodeObjectId(parameters["Object"]);
				const auto encodedId = parameters["ExtensionSchemaId"].get<std::string>();
				auto extensionId = SchemaId::Parse(encodedId);
				auto value = JsonCodec::DecodeWireValue(parameters["Value"]);
				if (!object || !extensionId || extensionId->ToString() != encodedId || !value)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "SetExtensionProperty identity or value is invalid"
					));
				auto target = ObjectRegistry::Get().Lookup(object->ToObjectId());
				if (!target || target->GetReplicationScopeId() != World->GetObjectId())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "StaleObject", "Object is not live in the open project"
					));
				auto *extension = GetActiveRuntimeSchemaRegistry().FindExtensionById(*extensionId);
				auto *property = GetActiveRuntimeSchemaRegistry().FindExtensionProperty(
					*extensionId, parameters["Property"].get<std::string>()
				);
				auto version = JsonCodec::DecodeUnsigned32(parameters["DefinitionVersion"]);
				if (!version || *version == 0)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "DefinitionVersion is outside the supported range"
					));
				if (!extension || extension->DefinitionVersion != *version || !property || !property->Editable)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "InvalidProperty", "Extension property identity is missing, incompatible, or not editable"
					));
				try { (void)ValidateSchemaExtensionPropertyValue(property->Type, *value); }
				catch (const std::exception &error) {
					return SerializeBoundedResponse(ErrorResponse(requestId, "ValidationFailed", error.what()));
				}
				auto mutation = Mutations.Apply(UpdateExtensionPropertyCommand{
					object->ToObjectId(),
					*extensionId,
					*version,
					parameters["Property"].get<std::string>(),
					std::move(*value),
				},
					StudioMutationAuthority());
				Json result{{"Status", MutationStatusName(mutation.Status)}, {"Message", mutation.Message}};
				if (mutation.Object) result["Object"] = JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(*mutation.Object));
				return SerializeBoundedResponse(
					mutation.Succeeded() ? SuccessResponse(requestId, std::move(result))
						: ErrorResponse(requestId, MutationStatusName(mutation.Status), mutation.Message)
				);
			}

			if (method == "SetCustomProperty") {
				if (!StudioSecurity.HasCapability(ScriptCapability::MutateDataModel))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Unauthorized", "SetCustomProperty requires MutateDataModel"
					));
				if (!Cursor)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"
					));
				if (!HasOnlyFields(parameters, {
						"Object", "DeclaringClassSchemaId", "DefinitionVersion", "Property", "Value", "TransactionId"
					}) || !parameters.contains("Object") || !parameters.contains("DeclaringClassSchemaId") ||
					!parameters["DeclaringClassSchemaId"].is_string() || !parameters.contains("DefinitionVersion") ||
					!parameters["DefinitionVersion"].is_number_unsigned() || !parameters.contains("Property") ||
					!parameters["Property"].is_string() || !parameters.contains("Value"))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "SetCustomProperty fields are invalid"
					));
				auto object = JsonCodec::DecodeObjectId(parameters["Object"]);
				const auto encodedId = parameters["DeclaringClassSchemaId"].get<std::string>();
				auto declaringId = SchemaId::Parse(encodedId);
				auto value = JsonCodec::DecodeWireValue(parameters["Value"]);
				if (!object || !declaringId || declaringId->ToString() != encodedId || !value)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "SetCustomProperty identity or value is invalid"
					));
				auto target = ObjectRegistry::Get().Lookup(object->ToObjectId());
				if (!target || target->GetReplicationScopeId() != World->GetObjectId())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "StaleObject", "Object is not live in the open project"
					));
				auto *declaringClass = GetActiveRuntimeSchemaRegistry().FindClassById(*declaringId);
				auto *property = GetActiveRuntimeSchemaRegistry().FindCustomClassProperty(
					*declaringId, parameters["Property"].get<std::string>()
				);
				auto *targetClass = InstanceClassRegistry::GetDefinition(target.get());
				auto version = JsonCodec::DecodeUnsigned32(parameters["DefinitionVersion"]);
				if (!version || *version == 0)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "DefinitionVersion is outside the supported range"
					));
				if (!declaringClass || declaringClass->ConstructionKind != SchemaClassConstructionKind::CustomData ||
					declaringClass->DefinitionVersion != *version || !property || !property->Editable || !targetClass ||
					!GetActiveRuntimeSchemaRegistry().IsClassDerivedFrom(targetClass->Id, *declaringId))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "InvalidProperty", "Custom property identity is missing, incompatible, or not editable"
					));
				try { (void)ValidateSchemaExtensionPropertyValue(property->Type, *value); }
				catch (const std::exception &error) {
					return SerializeBoundedResponse(ErrorResponse(requestId, "ValidationFailed", error.what()));
				}
				auto native = DecodeNativeWireValue(*value);
				if (!native)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "UnsupportedValue", "Custom property has no native mutation representation"
					));
				auto mutation = Mutations.Apply(UpdatePropertyCommand{
					object->ToObjectId(), parameters["Property"].get<std::string>(), std::move(*native)
				},
					StudioMutationAuthority());
				Json result{{"Status", MutationStatusName(mutation.Status)}, {"Message", mutation.Message}};
				if (mutation.Object) result["Object"] = JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(*mutation.Object));
				return SerializeBoundedResponse(
					mutation.Succeeded() ? SuccessResponse(requestId, std::move(result))
						: ErrorResponse(requestId, MutationStatusName(mutation.Status), mutation.Message)
				);
			}

			if (method == "CreateInstance" || method == "DestroyInstance" ||
				method == "DuplicateInstance" || method == "ReparentInstance") {
				if (!StudioSecurity.HasCapability(ScriptCapability::MutateDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Structural editing requires MutateDataModel"));
				if (!Cursor)
					return SerializeBoundedResponse(ErrorResponse(requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"));
				MutationResult Mutation;
				if (method == "CreateInstance") {
					if (!HasOnlyFields(parameters, {"ClassSchemaId", "DefinitionVersion", "Parent", "Name", "TransactionId"}) ||
						!parameters.contains("ClassSchemaId") || !parameters["ClassSchemaId"].is_string() ||
						!parameters.contains("DefinitionVersion") || !parameters["DefinitionVersion"].is_number_unsigned() ||
						!parameters.contains("Parent") || (parameters.contains("Name") && !parameters["Name"].is_string()))
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "CreateInstance fields are invalid"));
					const auto EncodedId = parameters["ClassSchemaId"].get<std::string>();
					auto ClassId = SchemaId::Parse(EncodedId);
					auto Version = JsonCodec::DecodeUnsigned32(parameters["DefinitionVersion"]);
					auto Parent = JsonCodec::DecodeObjectId(parameters["Parent"]);
					if (!ClassId || ClassId->ToString() != EncodedId || !Version || *Version == 0 || !Parent)
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "CreateInstance identity is invalid"));
					std::optional<std::string> Name;
					if (parameters.contains("Name")) Name = parameters["Name"].get<std::string>();
					Mutation = Mutations.Apply(CreateObjectCommand{*ClassId, *Version, Parent->ToObjectId(), std::move(Name)},
						StudioMutationAuthority());
				} else {
					const bool Reparent = method == "ReparentInstance";
					if (!HasOnlyFields(parameters, Reparent ? std::initializer_list<std::string_view>{"Object", "Parent", "TransactionId"}
						: std::initializer_list<std::string_view>{"Object", "TransactionId"}) || !parameters.contains("Object") ||
						(Reparent && !parameters.contains("Parent")))
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Structural identity fields are invalid"));
					auto Object = JsonCodec::DecodeObjectId(parameters["Object"]);
					auto Parent = Reparent ? JsonCodec::DecodeObjectId(parameters["Parent"]) : std::optional<WireObjectId>{};
					if (!Object || (Reparent && !Parent))
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Structural ObjectId is invalid"));
					if (method == "DestroyInstance")
						Mutation = Mutations.Apply(DestroyObjectCommand{Object->ToObjectId()}, StudioMutationAuthority());
					else if (method == "DuplicateInstance")
						Mutation = Mutations.Apply(DuplicateObjectCommand{Object->ToObjectId()}, StudioMutationAuthority());
					else Mutation = Mutations.Apply(ReparentObjectCommand{Object->ToObjectId(), Parent->ToObjectId()}, StudioMutationAuthority());
				}
				Json Result{{"Status", MutationStatusName(Mutation.Status)}, {"Message", Mutation.Message},
					{"AuthoritativeRevision", World->GetAuthoritativeRevision()}};
				if (Mutation.Object) Result["Object"] = JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(*Mutation.Object));
				return SerializeBoundedResponse(Mutation.Succeeded() ? SuccessResponse(requestId, std::move(Result))
					: ErrorResponse(requestId, MutationStatusName(Mutation.Status), Mutation.Message));
			}

			if (method == "AddTag" || method == "RemoveTag") {
				if (!StudioSecurity.HasCapability(ScriptCapability::MutateDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Tag mutation requires MutateDataModel"));
				if (!Cursor)
					return SerializeBoundedResponse(ErrorResponse(requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"));
				if (!HasOnlyFields(parameters, {"Object", "Tag", "TransactionId"}) || !parameters.contains("Object") ||
					!parameters.contains("Tag") || !parameters["Tag"].is_string())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Tag mutation fields are invalid"));
				auto object = JsonCodec::DecodeObjectId(parameters["Object"]);
				if (!object)
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Tag mutation Object is invalid"));
				auto target = ObjectRegistry::Get().Lookup(object->ToObjectId());
				if (!target || target->GetReplicationScopeId() != World->GetObjectId())
					return SerializeBoundedResponse(ErrorResponse(requestId, "StaleObject", "Object is not live in the open project"));
				MutationResult mutation = method == "AddTag"
					? Mutations.Apply(AddTagCommand{object->ToObjectId(), parameters["Tag"].get<std::string>(), World->GetObjectId()},
							  StudioMutationAuthority())
					: Mutations.Apply(RemoveTagCommand{object->ToObjectId(), parameters["Tag"].get<std::string>(), World->GetObjectId()},
							  StudioMutationAuthority());
				Json result{{"Status", MutationStatusName(mutation.Status)}, {"Message", mutation.Message}};
				if (mutation.Object) result["Object"] = JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(*mutation.Object));
				return SerializeBoundedResponse(
					mutation.Succeeded() ? SuccessResponse(requestId, std::move(result))
										: ErrorResponse(requestId, MutationStatusName(mutation.Status), mutation.Message)
				);
			}

			return SerializeBoundedResponse(ErrorResponse(requestId, "UnknownMethod", "EditorHost method is not supported"));
		} catch (const nlohmann::json::exception &error) {
			(void)error;
			return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "EditorHost JSON fields are malformed"));
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
