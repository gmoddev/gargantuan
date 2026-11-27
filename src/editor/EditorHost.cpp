// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/editor/AnimationPreview.hpp"
#include "gargantuan/editor/CharacterCompositionInspection.hpp"
#include "gargantuan/animation/AnimationTrack.hpp"

#include "gargantuan/InstanceProperty.hpp"
#include "gargantuan/assets/GasDependencies.hpp"
#include "gargantuan/assets/GasInstanceSerialization.hpp"
#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/LuaSourceContainer.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "terrain/RenderStage.hpp"
#include "terrain/ScenePersistence.hpp"
#include "terrain/TerrainMutation.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/gui/GuiRuntime.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/persistence/LocalPersistenceProvider.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/reflection/PreRunRegistration.hpp"
#include "gargantuan/reflection/PreRunCollection.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/Snapshot.hpp"
#include "gargantuan/runtime/SemanticSpatialResolver.hpp"
#include "gargantuan/runtime/WireCodec.hpp"
#include "gargantuan/runtime/WireJournal.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/InputService.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "serialization/JsonCodec.hpp"
#include "runtime/PreparedPropertyCommit.hpp"
#include "editor/StreamingDiagnosticsEncoding.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <limits>
#include <magic_enum/magic_enum.hpp>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace gargantuan {
	namespace {
		using Json = JsonCodec::Json;
		constexpr std::size_t MaximumGasTransferBytes = 512 * 1024;
		constexpr std::size_t MaximumGuiPreviewFrames = 8;

		struct GuiPreviewPlacement {
			std::uint32_t Width = 0;
			std::uint32_t Height = 0;
			std::uint32_t SurfaceWidth = 0;
			std::uint32_t SurfaceHeight = 0;
			float Scale = 0.0f;
			GuiRect Bounds;

			bool operator==(const GuiPreviewPlacement &Other) const {
				return Width == Other.Width && Height == Other.Height &&
					SurfaceWidth == Other.SurfaceWidth && SurfaceHeight == Other.SurfaceHeight;
			}
		};

		GuiPreviewPlacement GetGuiPreviewPlacement(std::uint32_t Width, std::uint32_t Height,
			std::uint32_t SurfaceWidth, std::uint32_t SurfaceHeight) {
			GuiPreviewPlacement Result{Width ? Width : SurfaceWidth, Height ? Height : SurfaceHeight,
				SurfaceWidth, SurfaceHeight, 0.0f, {}};
			if (SurfaceWidth == 0 || SurfaceHeight == 0) return Result;
			Result.Scale = std::min(static_cast<float>(SurfaceWidth) / Result.Width,
				static_cast<float>(SurfaceHeight) / Result.Height);
			const float DisplayWidth = Result.Width * Result.Scale;
			const float DisplayHeight = Result.Height * Result.Scale;
			Result.Bounds = {(SurfaceWidth - DisplayWidth) * 0.5f, (SurfaceHeight - DisplayHeight) * 0.5f,
				DisplayWidth, DisplayHeight};
			return Result;
		}

		GuiRect PlaceGuiRect(const GuiRect &Rect, const GuiPreviewPlacement &Placement) {
			return {Placement.Bounds.X + Rect.X * Placement.Scale, Placement.Bounds.Y + Rect.Y * Placement.Scale,
				Rect.Width * Placement.Scale, Rect.Height * Placement.Scale};
		}

		std::shared_ptr<const RenderUiFrame> PlaceGuiPreview(const GuiPresentationSnapshot &Presentation,
			const GuiPreviewPlacement &Placement) {
			auto Frame = std::make_shared<RenderUiFrame>(Presentation.Frame);
			Frame->ViewportWidth = Placement.SurfaceWidth;
			Frame->ViewportHeight = Placement.SurfaceHeight;
			for (auto &Batch : Frame->Batches) {
				auto Clip = Placement.Bounds;
				if (Batch.Clip) {
					const auto PlacedClip = PlaceGuiRect({Batch.Clip->X, Batch.Clip->Y, Batch.Clip->Width, Batch.Clip->Height}, Placement);
					Clip = GuiRect::Intersect(Clip, PlacedClip).value_or(GuiRect{});
				}
				Batch.Clip = RenderUiClipRect{Clip.X, Clip.Y, Clip.Width, Clip.Height};
				for (auto &Vertex : Batch.Vertices) {
					Vertex.Position.x = Placement.Bounds.X + Vertex.Position.x * Placement.Scale;
					Vertex.Position.y = Placement.Bounds.Y + Vertex.Position.y * Placement.Scale;
				}
			}
			return Frame;
		}

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

		[[nodiscard]] constexpr bool IsPlayPhysicalKey(std::uint32_t Value) {
			return Value == static_cast<std::uint32_t>(PhysicalKey::Unknown) ||
				(Value >= static_cast<std::uint32_t>(PhysicalKey::A) && Value <= static_cast<std::uint32_t>(PhysicalKey::Space)) ||
				(Value >= static_cast<std::uint32_t>(PhysicalKey::Right) && Value <= static_cast<std::uint32_t>(PhysicalKey::Up)) ||
				(Value >= static_cast<std::uint32_t>(PhysicalKey::LeftControl) && Value <= static_cast<std::uint32_t>(PhysicalKey::RightMeta));
		}

		[[nodiscard]] constexpr bool IsPlayLogicalKey(std::uint32_t Value) {
			return Value == static_cast<std::uint32_t>(LogicalKey::Backspace) ||
				Value == static_cast<std::uint32_t>(LogicalKey::Tab) ||
				(Value >= static_cast<std::uint32_t>(LogicalKey::Clear) && Value <= static_cast<std::uint32_t>(LogicalKey::Return)) ||
				Value == static_cast<std::uint32_t>(LogicalKey::Pause) ||
				Value == static_cast<std::uint32_t>(LogicalKey::Escape) ||
				Value == static_cast<std::uint32_t>(LogicalKey::Space) ||
				(Value >= static_cast<std::uint32_t>(LogicalKey::QuotedDouble) && Value <= static_cast<std::uint32_t>(LogicalKey::At)) ||
				(Value >= static_cast<std::uint32_t>(LogicalKey::LeftBracket) && Value <= static_cast<std::uint32_t>(LogicalKey::Delete)) ||
				(Value >= static_cast<std::uint32_t>(LogicalKey::KeypadZero) && Value <= static_cast<std::uint32_t>(LogicalKey::F15)) ||
				(Value >= static_cast<std::uint32_t>(LogicalKey::NumLock) && Value <= static_cast<std::uint32_t>(LogicalKey::Undo)) ||
				Value == static_cast<std::uint32_t>(LogicalKey::Unknown);
		}

		Json ErrorResponse(const Json &requestId, std::string code, std::string message) {
			if (!IsValidProtocolUtf8(message)) message = "EditorHost could not report the failure in UTF-8";
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

		std::string SerializeBoundedResponse(Json response, bool *Encoded = nullptr) {
			auto Serialized = JsonCodec::Encode(response, "EditorHost response");
			const bool Fits = Serialized && Serialized->size() <= EditorHostMaximumResponseBytes;
			if (Encoded) *Encoded = Fits;
			if (Fits) return std::move(*Serialized);
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

		bool AtomicBatchWritable(const InstanceProperty &Property, const ScriptSecurityContext &Security) {
			return PreparedPropertyCommit::SupportsProperty(Property) &&
				Security.HasCapability(ScriptCapability::MutateDataModel) &&
				Property.Read && Property.Write && Property.CanRead(Security) && Property.CanWrite(Security) &&
				static_cast<int>(Enums::Permission::None) >= static_cast<int>(Property.ReadPermission) &&
				static_cast<int>(Enums::Permission::None) >= static_cast<int>(Property.WritePermission);
		}

		// Reserve and encode everything fallible before Apply. Fixed-width JSON
		// number slots use trailing whitespace, so completing a committed response
		// needs neither allocation nor a JSON tree (including its allocating cleanup).
		struct PropertyBatchResponse {
			std::string Encoded;
			std::array<std::size_t, 5> Offsets{};
			explicit PropertyBatchResponse(const Json &RequestId) {
				Encoded = "{\"Version\":1,\"RequestId\":" + RequestId.dump() + ",\"Ok\":true,\"Result\":{";
				constexpr std::array Names{"StartingRevision", "ResultingRevision", "ChangedWriteCount",
					"TransactionId", "NotificationFailures"};
				for (std::size_t Index = 0; Index < Names.size(); ++Index) {
					if (Index != 0) Encoded += ',';
					Encoded += '"';
					Encoded += Names[Index];
					Encoded += "\":";
					Offsets[Index] = Encoded.size();
					Encoded.append(22, ' '); // uint64 plus optional string quotes
				}
				Encoded += "}}";
			}
			void Complete(const PreparedPropertyResult &Result) noexcept {
				const std::array<std::uint64_t, 5> Values{Result.StartingRevision, Result.ResultingRevision,
					Result.ChangedWrites, Result.HistoryId.Value, Result.NotificationFailures};
				for (std::size_t Index = 0; Index < Values.size(); ++Index) {
					auto *Slot = Encoded.data() + Offsets[Index];
					if (Index == 3 && Values[Index] == 0) {
						std::copy_n("null", 4, Slot);
						continue;
					}
					if (Index == 3) *Slot++ = '"';
					const auto Converted = std::to_chars(Slot, Slot + 20, Values[Index]);
					if (Index == 3) *Converted.ptr = '"';
				}
			}
		};

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

		std::string_view GetEditorDataTypeName(InstanceProperty::DataType Type) {
			switch (Type) {
				case InstanceProperty::DataType::Unsupported: return "Unsupported";
				case InstanceProperty::DataType::Bool: return "Bool";
				case InstanceProperty::DataType::Integer: return "Integer";
				case InstanceProperty::DataType::Number: return "Number";
				case InstanceProperty::DataType::String: return "String";
				case InstanceProperty::DataType::Vector2: return "Vector2";
				case InstanceProperty::DataType::Vector3: return "Vector3";
				case InstanceProperty::DataType::Color3: return "Color3";
				case InstanceProperty::DataType::UDim: return "UDim";
				case InstanceProperty::DataType::UDim2: return "UDim2";
				case InstanceProperty::DataType::CFrame: return "CFrame";
				case InstanceProperty::DataType::NativeEnum: return "NativeEnum";
				case InstanceProperty::DataType::SchemaEnum: return "SchemaEnum";
				case InstanceProperty::DataType::ObjectReference: return "ObjectReference";
			}
			return "Unsupported";
		}

		Json EncodeNativePropertyMetadata(
			const SchemaClassDefinition &Owner,
			const InstanceProperty &Property,
			const ScriptSecurityContext &Security
		) {
			const auto Permission = static_cast<int>(Enums::Permission::None);
			const bool Readable = Property.Read && Property.CanRead(Security) &&
				Permission >= static_cast<int>(Property.ReadPermission);
			const bool Writable = Property.Write && Property.CanWrite(Security) &&
				Property.WritePermission != Enums::Permission::Never &&
				Permission >= static_cast<int>(Property.WritePermission);
			const bool Supported = Property.SemanticType != InstanceProperty::DataType::Unsupported;
			Json Encoded{
				{"Name", Property.Name},
				{"CanonicalName", Owner.CanonicalName + "." + Property.Name},
				{"Type", Property.ReflectedTypedef},
				{"DataType", GetEditorDataTypeName(Property.SemanticType)},
				{"WireType", Property.WireType},
				{"Readable", Readable},
				{"Writable", Writable},
				{"Editable", Supported && Property.Editable && Readable && Writable},
				{"AtomicBatchWritable", AtomicBatchWritable(Property, Security)},
				{"Category", Property.Category},
				{"NumericRange", nullptr},
				{"EditorHint", Property.EditorHint ? Json(*Property.EditorHint) : Json(nullptr)},
				{"CompoundType", Property.CompoundType ? Json(*Property.CompoundType) : Json(nullptr)},
				{"EnumKind", nullptr},
				{"EnumType", nullptr},
				{"EnumSchemaId", Property.EnumSchemaId ? Json(Property.EnumSchemaId->ToString()) : Json(nullptr)},
				{"EnumDefinitionVersion", Property.EnumDefinitionVersion == 0 ? Json(nullptr) : Json(Property.EnumDefinitionVersion)},
				{"EnumItems", Json::array()},
				{"ObjectReferenceClassSchemaId", Property.ObjectReferenceClassSchemaId
					? Json(Property.ObjectReferenceClassSchemaId->ToString()) : Json(nullptr)},
				{"Nullable", Property.Nullable},
				{"Persistence", Property.PersistencePolicy == InstanceProperty::Persistence::Saved ? "Saved" : "Transient"},
				{"Replication", Property.ReplicationPolicy == InstanceProperty::Replication::FutureReplicated ? "Replicated" : "None"},
				{"Authority", Property.WriteAuthority == InstanceProperty::Authority::Main ? "Main" : "Any"},
				{"HasValidator", static_cast<bool>(Property.Validate) || Property.Range.has_value()},
				{"ReadDomains", EncodeDomains(Property.ReadDomains)},
				{"WriteDomains", EncodeDomains(Property.WriteDomains)},
				{"RequiredReadCapability", GetScriptCapabilityName(Property.RequiredReadCapability)},
				{"RequiredWriteCapability", GetScriptCapabilityName(Property.RequiredWriteCapability)},
			};
			if (Property.Range) Encoded["NumericRange"] = {
				{"Minimum", Property.Range->Minimum ? Json(*Property.Range->Minimum) : Json(nullptr)},
				{"Maximum", Property.Range->Maximum ? Json(*Property.Range->Maximum) : Json(nullptr)},
			};
			if (Property.SemanticType == InstanceProperty::DataType::NativeEnum && Property.NativeEnumType) {
				Encoded["EnumKind"] = "Native";
				Encoded["EnumType"] = *Property.NativeEnumType;
				if (auto Type = Enums::GetEnums().find(*Property.NativeEnumType); Type != Enums::GetEnums().end()) {
					std::vector<EnumItem> Items = Type->second->Items;
					std::ranges::sort(Items, {}, [](const EnumItem &Item) { return Item.Name; });
					for (const auto &Item : Items)
						Encoded["EnumItems"].push_back({{"Name", Item.Name}, {"Value", Item.Value}});
				}
			} else if (Property.SemanticType == InstanceProperty::DataType::SchemaEnum) {
				Encoded["EnumKind"] = "Schema";
			}
			if (auto Default = EncodePropertyDefault(Property, Property.Unmodified))
				Encoded["Default"] = JsonCodec::EncodeWireValue(*Default);
			return Encoded;
		}

		Json EncodeSchemaScalarPropertyMetadata(
			std::string_view Name,
			std::string_view CanonicalName,
			SchemaExtensionPropertyType Type,
			const WireValue &Default,
			bool Editable
		) {
			const auto DataType = Type == SchemaExtensionPropertyType::Boolean ? "Bool" :
				Type == SchemaExtensionPropertyType::Integer ? "Integer" :
				Type == SchemaExtensionPropertyType::Number ? "Number" : "String";
			const auto WireType = Type == SchemaExtensionPropertyType::Boolean ? "Bool" :
				Type == SchemaExtensionPropertyType::Integer ? "Int" :
				Type == SchemaExtensionPropertyType::Number ? "Double" : "String";
			return {
				{"Name", Name}, {"CanonicalName", CanonicalName},
				{"Type", GetSchemaExtensionPropertyTypeName(Type)},
				{"DataType", DataType}, {"WireType", WireType},
				{"Default", JsonCodec::EncodeWireValue(Default)},
				{"Readable", true}, {"Writable", true}, {"Editable", Editable},
				{"AtomicBatchWritable", false},
				{"Category", "Data"}, {"NumericRange", nullptr}, {"EditorHint", nullptr},
				{"CompoundType", nullptr}, {"EnumKind", nullptr}, {"EnumType", nullptr},
				{"EnumSchemaId", nullptr}, {"EnumDefinitionVersion", nullptr}, {"EnumItems", Json::array()},
				{"ObjectReferenceClassSchemaId", nullptr}, {"Nullable", false},
				{"Persistence", "Saved"}, {"Replication", "Replicated"}, {"Authority", "Main"},
				{"HasValidator", true},
				{"ReadDomains", Json::array({"Core", "PreRun", "Studio", "Server", "Client"})},
				{"WriteDomains", Json::array({"Core", "PreRun", "Studio", "Server", "Client"})},
				{"RequiredReadCapability", "ReadDataModel"},
				{"RequiredWriteCapability", "MutateDataModel"},
			};
		}

		void AddEditorPropertyValues(
			Json &Snapshot,
			const ScriptSecurityContext &Security,
			ObjectId Scope
		) {
			if (!Snapshot.contains("Objects") || !Snapshot["Objects"].is_array())
				throw std::runtime_error("Editor snapshot object collection is malformed");
			std::unordered_set<WireObjectId> Visible;
			for (const auto &Encoded : Snapshot["Objects"]) {
				auto Id = JsonCodec::DecodeObjectId(Encoded["Id"]);
				if (!Id) throw std::runtime_error("Editor snapshot object identity is malformed");
				Visible.insert(*Id);
			}
			for (auto &Encoded : Snapshot["Objects"]) {
				auto Id = JsonCodec::DecodeObjectId(Encoded["Id"]);
				auto InstanceValue = Id ? ObjectRegistry::Get().Lookup(Id->ToObjectId()) : nullptr;
				auto *Definition = InstanceValue ? InstanceClassRegistry::GetDefinition(InstanceValue.get()) : nullptr;
				if (!InstanceValue || !Definition || InstanceValue->GetReplicationScopeId() != Scope)
					throw std::runtime_error("Editor snapshot object is not live in the requested scope");
				Json Values = Json::object();
				std::vector<std::string> Names;
				Names.reserve(Definition->AllProperties.size());
				for (const auto &[Name, Property] : Definition->AllProperties) {
					if (!Property->Signal && !Property->CustomSchemaPropertyType &&
						Property->SemanticType != InstanceProperty::DataType::Unsupported)
						Names.push_back(Name);
				}
				std::ranges::sort(Names);
				for (const auto &Name : Names) {
					const auto *Property = Definition->AllProperties.at(Name);
					if (!Property->Read || !Property->CanRead(Security) ||
						static_cast<int>(Enums::Permission::None) < static_cast<int>(Property->ReadPermission)) continue;
					auto Value = InstanceValue->ReadPropertyWireValue(Name);
					if (!Value) continue;
					if (const auto *Reference = std::get_if<WireObjectReference>(&*Value);
						Reference && !Visible.contains(Reference->Object)) continue;
					Values[Name] = JsonCodec::EncodeWireValue(*Value);
				}
				Encoded["EditorProperties"] = std::move(Values);
			}
			Snapshot["EditorPropertyValuesVersion"] = 1;
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

		bool IsAuthoringMethodWhilePlaying(std::string_view Method) {
			return Method == "OpenProject" || Method == "CreateProject" || Method == "SaveProject" ||
				Method == "SaveProjectAs" || Method == "BeginTransaction" || Method == "CommitTransaction" ||
				Method == "Undo" || Method == "Redo" || Method == "SetScriptSource" ||
				Method == "SetPreRunSource" || Method == "ValidatePreRun" ||
				Method == "SetProperty" || Method == "SetPropertyBatch" || Method == "SetTransform" || Method == "SetAttribute" || Method == "SetExtensionProperty" ||
				Method == "SetCustomProperty" || Method == "AddTag" || Method == "RemoveTag" ||
				Method == "CreateInstance" || Method == "DestroyInstance" ||
				Method == "ImportGasModel" || Method == "OpenGasPlace" ||
				Method == "DuplicateInstance" || Method == "ReparentInstance" ||
				Method == "ImportAsset" || Method == "ReimportAsset" || Method == "DeleteAsset" ||
				Method == "StartPackageBuild" || Method == "SubmitTerrainBrush" ||
				Method == "UndoTerrainEdit" || Method == "RedoTerrainEdit" || Method == "CancelTerrainEdit";
		}

		std::optional<std::uint64_t> DecodeCanonicalJobId(const Json &Value, bool AllowZero = false) {
			if (!Value.is_string()) return std::nullopt;
			const auto &Encoded = Value.get_ref<const std::string &>();
			if (AllowZero && Encoded == "0") return 0;
			std::uint64_t Parsed = 0;
			const auto [End, Error] = std::from_chars(Encoded.data(), Encoded.data() + Encoded.size(), Parsed);
			if (Encoded.empty() || Encoded.front() == '0' || Error != std::errc{} ||
				End != Encoded.data() + Encoded.size() || Parsed == 0) return std::nullopt;
			return Parsed;
		}

		Json EncodePackageDiagnostics(const std::vector<PackageDiagnostic> &Diagnostics) {
			Json Result = Json::array();
			for (const auto &Diagnostic : Diagnostics) {
				std::string_view Severity = "Info";
				if (Diagnostic.Severity == PackageDiagnosticSeverity::Warning) Severity = "Warning";
				else if (Diagnostic.Severity == PackageDiagnosticSeverity::Error) Severity = "Error";
				Result.push_back({
					{"Severity", Severity}, {"Category", Diagnostic.Category}, {"Code", Diagnostic.Code},
					{"Message", Diagnostic.Message}, {"Item", Diagnostic.Item},
				});
			}
			return Result;
		}

		std::optional<std::uint64_t> DecodeNonzeroDecimal(const Json &Value) {
			if (!Value.is_string()) return std::nullopt;
			const auto &Encoded = Value.get_ref<const std::string &>();
			std::uint64_t Parsed = 0;
			const auto [End, Error] = std::from_chars(Encoded.data(), Encoded.data() + Encoded.size(), Parsed);
			if (Encoded.empty() || Encoded.front() == '0' || Error != std::errc{} ||
				End != Encoded.data() + Encoded.size() || Parsed == 0) return std::nullopt;
			return Parsed;
		}

		std::optional<PlaySessionId> DecodePlaySessionId(const Json &Value) {
			const auto Parsed = DecodeNonzeroDecimal(Value);
			return Parsed ? std::optional(PlaySessionId{*Parsed}) : std::nullopt;
		}

		struct ScriptResourceLookup {
			std::shared_ptr<LuaSourceContainer> Script;
			const char *Error = nullptr;
		};

		ScriptResourceLookup FindScriptResource(const std::shared_ptr<DataModel> &World, std::string_view SourceId) {
			// Disposable bounded traversal of the canonical graph, never a second
			// durable registry. Only a fully persistent ancestor chain can reopen.
			std::vector<std::pair<std::shared_ptr<Instance>, bool>> Pending{{World, true}};
			std::shared_ptr<LuaSourceContainer> Match;
			std::size_t Visited = 0;
			while (!Pending.empty()) {
				auto [Object, Persistent] = std::move(Pending.back());
				Pending.pop_back();
				if (++Visited > MaximumPersistenceObjects ||
					Object->Children.size() > MaximumPersistenceObjects - Visited - Pending.size())
					return {{}, "ResourceLimit"};
				if (Object->GetDestroyed() || Object->GetReplicationScopeId() != World->GetObjectId()) continue;
				Persistent = Persistent && Object->GetArchivable();
				if (Persistent) if (auto Script = std::dynamic_pointer_cast<LuaSourceContainer>(Object);
					Script && Script->GetSourceId() == SourceId) {
					if (Match) return {{}, "AmbiguousResource"};
					Match = std::move(Script);
				}
				for (const auto &Child : Object->Children) Pending.emplace_back(Child, Persistent);
			}
			return {std::move(Match), nullptr};
		}

		Json EncodeScriptBinding(const Project &ProjectValue, const std::shared_ptr<LuaSourceContainer> &Script) {
			const auto *Definition = InstanceClassRegistry::GetDefinition(Script.get());
			return {{"Version", 1}, {"ProjectId", ProjectValue.Identity.ToString()}, {"SourceId", Script->GetSourceId()},
				{"ClassSchemaId", Definition->Id.ToString()}, {"ClassDefinitionVersion", Definition->DefinitionVersion}};
		}

		Json EncodeScriptBindingResult(const Project &ProjectValue, const std::shared_ptr<DataModel> &World,
			std::uint64_t PersistedRevision, const std::shared_ptr<LuaSourceContainer> &Script) {
			return {{"Binding", EncodeScriptBinding(ProjectValue, Script)},
				{"Object", JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(Script->GetObjectId()))},
				{"Scope", JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(World->GetObjectId()))},
				{"ProjectId", ProjectValue.Identity.ToString()}, {"AuthoritativeRevision", World->GetAuthoritativeRevision()},
				{"PersistedRevision", PersistedRevision}, {"IdentityOrigin", Script->WasSourceIdLoaded() ? "Loaded" : "Generated"}};
		}

		PlayScriptSources CapturePlayScriptSources(const Project &ProjectValue, const std::shared_ptr<DataModel> &World) {
			PlayScriptSources Sources{.Project = ProjectValue.Identity, .Scope = World->GetObjectId()};
			std::vector<std::shared_ptr<Instance>> Pending{World};
			std::size_t Visited = 0;
			while (!Pending.empty()) {
				auto Object = std::move(Pending.back());
				Pending.pop_back();
				if (++Visited > MaximumPersistenceObjects) return {};
				if (Object->GetDestroyed() || !Object->GetArchivable() || Object->GetReplicationScopeId() != Sources.Scope)
					continue;
				if (auto ScriptValue = std::dynamic_pointer_cast<Script>(Object)) {
					const auto *Definition = InstanceClassRegistry::GetDefinition(ScriptValue.get());
					if (Sources.Bindings.size() == PlayScriptSources::MaximumBindings ||
						!Sources.Bindings.emplace(ScriptValue->GetSourceId(), PlayScriptBinding{
							Definition->Id, Definition->DefinitionVersion, ScriptValue->GetSourceVersion(), ScriptValue->GetObjectId()}).second)
						return {};
				}
				if (Object->Children.size() > MaximumPersistenceObjects - Visited - Pending.size()) return {};
				Pending.insert(Pending.end(), Object->Children.begin(), Object->Children.end());
			}
			return Sources;
		}

		Json EncodePlayInputSnapshot(const std::shared_ptr<DataModel> &RuntimeWorld, InputService &Input) {
			const auto Snapshot = Input.Inspect();
			const auto EncodeId = [](ObjectId Id) -> Json {
				return Id.IsValid() ? Json(JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(Id))) : Json(nullptr);
			};
			const auto Label = [&](ObjectId Id) -> std::string {
				const auto Object = ObjectRegistry::Get().Lookup(Id);
				if (!Object || Object->GetReplicationScopeId() != RuntimeWorld->GetObjectId()) return {};
				const auto &Name = Object->GetName();
				// Labels are presentation only. Never truncate a UTF-8 sequence or use a label as identity.
				return Name.size() <= 256 ? Name : "[name exceeds inspection limit]";
			};
			Json Result = {
				{"Version", 1}, {"Scope", EncodeId(RuntimeWorld->GetObjectId())},
				{"Revision", Snapshot.Stats.Revision}, {"MemoryBytes", Snapshot.Stats.MemoryBytes},
				{"Modifiers", Snapshot.Modifiers}, {"PressedKeys", Snapshot.PressedKeys},
				{"Contexts", Json::array()}, {"Actions", Json::array()}, {"Bindings", Json::array()},
				{"ValidationErrors", Input.GetValidationErrors()},
			};
			for (const auto &Context : Snapshot.Contexts) Result["Contexts"].push_back({
				{"Id", EncodeId(Context.Id)}, {"Name", Label(Context.Id)},
				{"Enabled", Context.Enabled}, {"Priority", Context.Priority}, {"Consume", Context.Consume},
			});
			for (const auto &Action : Snapshot.Actions) Result["Actions"].push_back({
				{"Id", EncodeId(Action.Definition.Id)}, {"Context", EncodeId(Action.Definition.Context)},
				{"Name", Label(Action.Definition.Id)},
				{"Enabled", Action.Definition.Enabled}, {"Type", magic_enum::enum_name(Action.Definition.ValueType)},
				{"Down", Action.State.Down}, {"Value", Action.State.Value},
				{"Vector", {{"X", Action.State.X}, {"Y", Action.State.Y}}},
				{"LastTransition", Action.State.LastTransition},
				{"LastReason", magic_enum::enum_name(Action.State.LastReason)},
			});
			for (const auto &Binding : Snapshot.Bindings) Result["Bindings"].push_back({
				{"Id", EncodeId(Binding.Definition.Id)}, {"Action", EncodeId(Binding.Definition.Action)},
				{"Name", Label(Binding.Definition.Id)},
				{"Enabled", Binding.Definition.Enabled}, {"Type", magic_enum::enum_name(Binding.Definition.InputKind)},
				{"Code", Binding.Definition.Code}, {"Modifiers", Binding.Definition.Modifiers},
				{"ExactModifiers", Binding.Definition.ExactModifiers},
				{"Priority", Binding.Definition.Priority}, {"Consume", Binding.Definition.Consume},
				{"Matched", Binding.Matched}, {"Active", Binding.Active},
				{"Suppressed", Binding.Suppressed}, {"Eligible", Binding.Eligible},
				{"WinningContext", EncodeId(Binding.WinningContext)},
			});
			return Result;
		}

		Json EncodePlayScriptLocation(const PlayScriptFailureLocation &Source) {
			return {
				{"Binding", {{"Version", 1}, {"ProjectId", Source.Project.ToString()}, {"SourceId", Source.SourceId},
					{"ClassSchemaId", Source.Binding.ClassSchemaId.ToString()},
					{"ClassDefinitionVersion", Source.Binding.ClassDefinitionVersion}}},
				{"Scope", JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(Source.Scope))},
				{"Object", JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(Source.Binding.AuthoringObject))},
				{"LaunchAuthoritativeRevision", Source.LaunchRevision},
				{"SourceVersion", Source.Binding.SourceVersion}, {"Line", Source.Line},
			};
		}

		Json EncodePlayRootDebugPause(const PlaySession &Session) {
			const auto Pause = Session.GetRootDebugPause();
			return Pause ? Json({{"Serial", Pause->Serial}, {"CanResume", Pause->CanResume},
				{"Source", EncodePlayScriptLocation(Pause->Source)}}) : Json(nullptr);
		}

		Json EncodePlayDiagnostics(std::vector<PlayDiagnostic> Diagnostics) {
			Json Result = Json::array();
			for (auto &Diagnostic : Diagnostics) {
				Json Entry = {
					{"Sequence", Diagnostic.Sequence}, {"TimestampMilliseconds", Diagnostic.TimestampMilliseconds},
					{"Severity", std::move(Diagnostic.Severity)}, {"Category", std::move(Diagnostic.Category)},
					{"Message", std::move(Diagnostic.Message)},
				};
				if (const auto &Source = Diagnostic.Source) Entry["Source"] = EncodePlayScriptLocation(*Source);
				Result.push_back(std::move(Entry));
			}
			return Result;
		}

		std::string EncodeProjectPath(const std::filesystem::path &Path) {
			const auto Utf8 = Path.generic_u8string();
			return std::string(reinterpret_cast<const char *>(Utf8.data()), Utf8.size());
		}

		Json EncodeProjectState(
			const std::shared_ptr<DataModel> &world,
			std::uint64_t persistedRevision,
			const std::optional<Project> &project,
			std::uint64_t TeamAuthor = 0
		) {
			if (!world || !project) return {
				{"AuthoritativeRevision", 0}, {"PersistedRevision", 0},
				{"Dirty", false}, {"CurrentDestination", nullptr},
				{"History", {{"CanUndo", false}, {"CanRedo", false}, {"UndoLabel", nullptr}, {"RedoLabel", nullptr}}}
			};
			const auto authoritativeRevision = world->GetAuthoritativeRevision();
			const auto History = world->Transactions.GetStatus();
			Json Result = {
				{"AuthoritativeRevision", authoritativeRevision},
				{"PersistedRevision", persistedRevision},
				{"Dirty", authoritativeRevision != persistedRevision},
				{"CurrentDestination", EncodeProjectPath(project->Root)},
				{"History", {
					{"CanUndo", History.CanUndo}, {"CanRedo", History.CanRedo},
					{"UndoLabel", History.UndoLabel ? Json(*History.UndoLabel) : Json(nullptr)},
					{"RedoLabel", History.RedoLabel ? Json(*History.RedoLabel) : Json(nullptr)},
					{"RetainedCount", History.RetainedCount}, {"SemanticBytes", History.SemanticBytes}
				}},
			};
			if (TeamAuthor != 0) {
				const auto *Undo = world->Transactions.GetUndoTransaction();
				const auto *Redo = world->Transactions.GetRedoTransaction();
				auto &Actions = Result["History"];
				if (!Undo || Undo->Author != TeamAuthor) {
					Actions["CanUndo"] = false;
					Actions["UndoLabel"] = nullptr;
				}
				if (!Redo || Redo->Author != TeamAuthor) {
					Actions["CanRedo"] = false;
					Actions["RedoLabel"] = nullptr;
				}
				Result["CurrentDestination"] = nullptr;
				Result["ProjectId"] = project->Identity.ToString();
				Result["SchemaGeneration"] = GetRuntimeSchemaLifecycle().GetActiveGeneration();
			}
			return Result;
		}

		Json EncodeAssetRecord(const AssetRecord &Record, bool IncludeAdvancedMaterials = false) {
			Json Diagnostic = nullptr;
			if (Record.Diagnostic)
				Diagnostic = {{"Code", Record.Diagnostic->Code}, {"Message", Record.Diagnostic->Message}};
			Json Dependencies = Json::array();
			for (const auto &Dependency : Record.Dependencies) Dependencies.push_back(Dependency.ToString());
			Json Metadata = Json::object();
			// Report stored asset declarations; package geometry coverage is validated separately.
			const auto EncodeLocalBounds = [](const RenderBounds &Bounds, const char *Coverage) {
				return Json{
					{"Space", "AssetLocal"}, {"Coverage", Coverage},
					{"Minimum", {Bounds.Minimum.x, Bounds.Minimum.y, Bounds.Minimum.z}},
					{"Maximum", {Bounds.Maximum.x, Bounds.Maximum.y, Bounds.Maximum.z}},
				};
			};
			if (Record.Asset) std::visit([&](const auto &Asset) {
				using T = std::decay_t<decltype(Asset)>;
				if constexpr (std::is_same_v<T, ImportedImage>) {
					Metadata["Width"] = Asset.Width;
					Metadata["Height"] = Asset.Height;
				} else if constexpr (std::is_same_v<T, ImportedMesh>) {
					Metadata["GeometryInspectionVersion"] = 1;
					Metadata["LocalBounds"] = EncodeLocalBounds(Asset.Bounds,
						Asset.Skeleton || Asset.SkinInfluences ? "DeclaredBindPose" : "DeclaredMesh");
					Metadata["CollisionAssetId"] = Asset.CollisionAsset
						? Json(Asset.CollisionAsset->ToString()) : Json(nullptr);
					Metadata["VertexCount"] = Asset.Vertices ? Asset.Vertices->size() : 0;
					Metadata["IndexCount"] = Asset.Indices ? Asset.Indices->size() : 0;
					Metadata["SubmeshCount"] = Asset.SubmeshCount;
					Json Primitives = Json::array();
					if (Asset.Primitives) for (const auto &Primitive : *Asset.Primitives) Primitives.push_back({
							{"FirstIndex", Primitive.FirstIndex}, {"IndexCount", Primitive.IndexCount},
							{"Material", Primitive.Material ? Json(Primitive.Material->ToString()) : Json(nullptr)},
						});
					Metadata["Primitives"] = std::move(Primitives);
					if (Asset.Skeleton && Asset.Skeleton->Joints) {
						Metadata["JointCount"] = Asset.Skeleton->Joints->size();
						Metadata["SkeletonCompatibilityId"] = Asset.Skeleton->CompatibilityId.ToString();
					}
				} else if constexpr (std::is_same_v<T, ImportedCollision>) {
					Metadata["GeometryInspectionVersion"] = 1;
					Metadata["LocalBounds"] = EncodeLocalBounds(Asset.Bounds, "DeclaredCollision");
					Metadata["Representation"] = "StaticTriangles";
					Metadata["VertexCount"] = Asset.Positions ? Asset.Positions->size() : 0;
					Metadata["IndexCount"] = Asset.Indices ? Asset.Indices->size() : 0;
				} else if constexpr (std::is_same_v<T, ImportedAnimation>) {
					Metadata["Duration"] = Asset.Duration;
					Metadata["ChannelCount"] = Asset.Tracks ? Asset.Tracks->size() : 0;
					Metadata["SkeletonCompatibilityId"] = Asset.SkeletonCompatibilityId.ToString();
				} else if constexpr (std::is_same_v<T, ImportedFont>) {
					Metadata["FaceCount"] = Asset.FaceCount;
				} else if constexpr (std::is_same_v<T, ImportedMaterial>) {
					Metadata["BaseColorFactor"] = {
						Asset.BaseColorFactor[0], Asset.BaseColorFactor[1],
						Asset.BaseColorFactor[2], Asset.BaseColorFactor[3]
					};
					Metadata["BaseColorTexture"] = Asset.BaseColorTexture ?
						Json(Asset.BaseColorTexture->ToString()) : Json(nullptr);
					Metadata["MetallicFactor"] = Asset.MetallicFactor;
					Metadata["RoughnessFactor"] = Asset.RoughnessFactor;
					Metadata["NormalTexture"] = Asset.NormalTexture ?
						Json(Asset.NormalTexture->ToString()) : Json(nullptr);
					Metadata["MetallicRoughnessTexture"] = Asset.MetallicRoughnessTexture ?
						Json(Asset.MetallicRoughnessTexture->ToString()) : Json(nullptr);
					Metadata["AlphaMode"] = Asset.AlphaMode == AssetMaterialAlphaMode::Opaque ? "Opaque" :
						Asset.AlphaMode == AssetMaterialAlphaMode::Mask ? "Mask" : "Blend";
					Metadata["AlphaCutoff"] = Asset.AlphaCutoff;
					Metadata["DoubleSided"] = Asset.DoubleSided;
					if (IncludeAdvancedMaterials && Asset.HasAdvancedInputs()) {
						constexpr std::array Slots{"BaseColor", "Normal", "MetallicRoughness", "Emissive", "Occlusion"};
						constexpr std::array MinFilters{"Default", "Nearest", "Linear", "NearestMipmapNearest",
							"LinearMipmapNearest", "NearestMipmapLinear", "LinearMipmapLinear"};
						constexpr std::array MagFilters{"Default", "Nearest", "Linear"};
						constexpr std::array WrapModes{"Repeat", "MirroredRepeat", "ClampToEdge"};
						Json Samplers = Json::object();
						for (std::size_t Index = 0; Index < Slots.size(); ++Index) {
							const auto &Sampler = Asset.TextureSamplers[Index];
							if (Sampler == RenderSamplerState{}) continue;
							Samplers[Slots[Index]] = {
								{"MinFilter", MinFilters[static_cast<std::size_t>(Sampler.MinFilter)]},
								{"MagFilter", MagFilters[static_cast<std::size_t>(Sampler.MagFilter)]},
								{"WrapU", WrapModes[static_cast<std::size_t>(Sampler.WrapU)]},
								{"WrapV", WrapModes[static_cast<std::size_t>(Sampler.WrapV)]},
							};
						}
						Metadata["AdvancedMaterial"] = {
							{"EmissiveFactor", {Asset.EmissiveFactor.x, Asset.EmissiveFactor.y, Asset.EmissiveFactor.z}},
							{"EmissiveTexture", Asset.EmissiveTexture ? Json(Asset.EmissiveTexture->ToString()) : Json(nullptr)},
							{"OcclusionTexture", Asset.OcclusionTexture ? Json(Asset.OcclusionTexture->ToString()) : Json(nullptr)},
							{"OcclusionStrength", Asset.OcclusionStrength}, {"NormalScale", Asset.NormalScale},
							{"TextureSamplers", std::move(Samplers)},
						};
					}
				}
			}, *Record.Asset);
			return {
				{"AssetId", Record.Id.ToString()}, {"Reference", Record.Reference.Value},
				{"Kind", GetAssetKindName(Record.Kind)}, {"Name", Record.Name}, {"Source", Record.Source},
				{"SourceGroupId", Record.SourceGroupId.ToString()}, {"LogicalKey", Record.LogicalKey},
				{"PrimarySourceAsset", Record.PrimarySourceAsset},
				{"ContentId", Record.ContentId.IsValid() ? Json(Record.ContentId.ToString()) : Json(nullptr)},
				{"ContentRevision", Record.ContentRevision}, {"State", GetAssetStateName(Record.State)},
				{"Available", static_cast<bool>(Record.Asset) &&
					(Record.State == AssetState::Ready || Record.State == AssetState::Stale)},
				{"BuiltIn", Record.BuiltIn}, {"Diagnostic", std::move(Diagnostic)},
				{"Dependencies", std::move(Dependencies)}, {"Metadata", std::move(Metadata)},
			};
		}

		Json DescribeAnimationRig(const std::shared_ptr<AssetService> &Assets, std::string Reference) {
			if (!Assets || !AssetReference::Parse(Reference))
				throw std::invalid_argument("A strict asset reference is required");
			if (auto Clip = Assets->ResolveAnimation(Reference); Clip && Clip->Value.SkeletonAsset)
				Reference = "asset://" + Clip->Value.SkeletonAsset->ToString();
			const auto Mesh = Assets->ResolveMeshResource(Reference);
			if (!Mesh || !Mesh->Value.Skeleton || !Mesh->Value.Skeleton->Joints)
				throw std::invalid_argument("Select an available skinned Mesh or Animation asset");
			const auto Record = Assets->GetAsset(Reference);
			const auto &Skeleton = *Mesh->Value.Skeleton;
			Json Joints = Json::array(), Clips = Json::array();
			for (const auto &Joint : *Skeleton.Joints) {
				const auto Separator = Joint.Path.find_last_of('/');
				Joints.push_back({{"Name", Joint.Path.substr(Separator == std::string::npos ? 0 : Separator + 1)},
					{"Path", Joint.Path}, {"Parent", Joint.Parent},
					{"Translation", {Joint.BindTranslation.x, Joint.BindTranslation.y, Joint.BindTranslation.z}},
					{"Rotation", {Joint.BindRotation.x, Joint.BindRotation.y, Joint.BindRotation.z, Joint.BindRotation.w}},
					{"Scale", {Joint.BindScale.x, Joint.BindScale.y, Joint.BindScale.z}}});
			}
			std::size_t ChannelCount = 0;
			for (const auto &Entry : Assets->GetCatalog(false)) {
				if (Entry.Kind != AssetKind::Animation || !Entry.Asset) continue;
				const auto &Clip = std::get<ImportedAnimation>(*Entry.Asset);
				if (Clip.SkeletonCompatibilityId != Skeleton.CompatibilityId) continue;
				ChannelCount += Clip.Tracks ? Clip.Tracks->size() : 0;
				if (Clips.size() >= 256 || ChannelCount > 4096)
					throw std::length_error("Rig inspection exceeds the 256 clip / 4096 channel response bound");
				Json Channels = Json::array();
				if (Clip.Tracks) for (const auto &Track : *Clip.Tracks)
					Channels.push_back({{"JointPath", Track.JointPath},
						{"TranslationInterpolation", Track.TranslationInterpolation == AssetAnimationInterpolation::Step ? "STEP" : "LINEAR"},
						{"RotationInterpolation", Track.RotationInterpolation == AssetAnimationInterpolation::Step ? "STEP" : "LINEAR"},
						{"ScaleInterpolation", Track.ScaleInterpolation == AssetAnimationInterpolation::Step ? "STEP" : "LINEAR"},
						{"TranslationKeys", Track.TranslationKeys ? Track.TranslationKeys->size() : 0},
						{"RotationKeys", Track.RotationKeys ? Track.RotationKeys->size() : 0},
						{"ScaleKeys", Track.ScaleKeys ? Track.ScaleKeys->size() : 0}});
				Clips.push_back({{"Reference", Entry.Reference.Value}, {"Name", Entry.Name},
					{"ContentRevision", Entry.ContentRevision}, {"Duration", Clip.Duration}, {"Channels", std::move(Channels)}});
			}
			return {{"RigVersion", 1}, {"MeshReference", Reference}, {"ContentRevision", Record->ContentRevision},
				{"Diagnostic", Record->Diagnostic ? Json{{"Code", Record->Diagnostic->Code}, {"Message", Record->Diagnostic->Message}} : Json(nullptr)},
				{"SkeletonCompatibilityId", Skeleton.CompatibilityId.ToString()}, {"Joints", std::move(Joints)},
				{"Clips", std::move(Clips)},
				{"BoundsMinimum", {Mesh->Value.Bounds.Minimum.x, Mesh->Value.Bounds.Minimum.y, Mesh->Value.Bounds.Minimum.z}},
				{"BoundsMaximum", {Mesh->Value.Bounds.Maximum.x, Mesh->Value.Bounds.Maximum.y, Mesh->Value.Bounds.Maximum.z}}};
		}

		Json EncodeCharacterComposition(const CharacterCompositionResult &Inspection) {
			auto EncodeId = [](ObjectId Value) -> Json {
				return Value.IsValid() ? JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(Value)) : Json(nullptr);
			};
			auto EncodeAsset = [](const CharacterCompositionAsset &Value) {
				return Json{
					{"Reference", Value.Reference},
					{"AssetId", Value.Id ? Json(Value.Id->ToString()) : Json(nullptr)},
					{"ContentId", Value.ContentId.IsValid() ? Json(Value.ContentId.ToString()) : Json(nullptr)},
					{"ContentRevision", Value.ContentRevision}, {"State", GetAssetStateName(Value.State)},
					{"Available", Value.Available},
				};
			};
			Json Meshes = Json::array(), Attachments = Json::array(), Issues = Json::array();
			for (const auto &Value : Inspection.Meshes) {
				Meshes.push_back({
					{"Object", EncodeId(Value.Object)}, {"Mesh", EncodeAsset(Value.Mesh)},
					{"Material", Value.Material ? EncodeAsset(*Value.Material) : Json(nullptr)},
					{"SkeletonCompatibilityId", Value.SkeletonCompatibilityId.IsValid()
						? Json(Value.SkeletonCompatibilityId.ToString()) : Json(nullptr)},
					{"JointCount", Value.JointCount}, {"AnimatorCount", Value.AnimatorCount},
					{"IsRootPart", Value.IsRootPart}, {"Anchored", Value.Anchored},
					{"CanCollide", Value.CanCollide}, {"StaticMeshCollision", Value.StaticMeshCollision},
				});
			}
			for (const auto &Value : Inspection.Attachments) {
				Attachments.push_back({
					{"Object", EncodeId(Value.Object)}, {"OwnerMesh", EncodeId(Value.OwnerMesh)},
					{"JointPath", Value.JointPath}, {"JointPathExists", Value.JointPathExists},
				});
			}
			for (const auto &Value : Inspection.Issues) {
				Issues.push_back({
					{"Code", std::string(magic_enum::enum_name(Value.Code))},
					{"Severity", std::string(magic_enum::enum_name(Value.Severity))},
					{"Object", EncodeId(Value.Object)}, {"Reference", Value.Reference},
				});
			}
			return {
				{"Character", EncodeId(Inspection.Character)},
				{"RootPart", Inspection.RootPart ? EncodeId(*Inspection.RootPart) : Json(nullptr)},
				{"Complete", Inspection.Complete}, {"Valid", Inspection.Valid},
				{"ObjectsVisited", Inspection.ObjectsVisited}, {"Meshes", std::move(Meshes)},
				{"Attachments", std::move(Attachments)}, {"Issues", std::move(Issues)},
			};
		}

		void EncodePreviewBlendState(Json &Result, const AnimationPreview &Preview) {
			Result["BlendVersion"] = 1;
			Result["BlendWeight"] = Preview.GetBlendWeight();
			const auto Primary = Preview.GetTrack(AnimationPreviewClipSlot::Primary);
			const auto Secondary = Preview.GetTrack(AnimationPreviewClipSlot::Secondary);
			Result["PrimaryTimePosition"] = Primary ? Json(Primary->GetTimePosition()) : Json(nullptr);
			Result["PrimaryPlaybackState"] = Primary ? Json(magic_enum::enum_name(Primary->GetPlaybackState())) : Json(nullptr);
			Result["SecondaryTimePosition"] = Secondary ? Json(Secondary->GetTimePosition()) : Json(nullptr);
			Result["SecondaryPlaybackState"] = Secondary ? Json(magic_enum::enum_name(Secondary->GetPlaybackState())) : Json(nullptr);
		}

		std::optional<std::filesystem::path> ValidateProjectDestination(const Json &value) {
			if (!value.is_string()) return std::nullopt;
			const auto &encoded = value.get_ref<const std::string &>();
			if (encoded.empty() || encoded.size() > 32768 || encoded.find('\0') != std::string::npos ||
				encoded.find("://") != std::string::npos)
				return std::nullopt;
			const auto candidate = std::filesystem::u8path(encoded);
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
					".garg-creating-{}-{}",
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

		constexpr std::size_t MaximumTerrainBrushSamples = 4096;

		struct EditorTerrainBrush {
			terrain::EditRequest Edit;
			terrain::SampleBox Bounds;
		};

		std::optional<terrain::WorldPoint> DecodeTerrainWorldPoint(const Json &Value) {
			if (!Value.is_array() || Value.size() != 3) return std::nullopt;
			std::array<double, 3> Point;
			for (std::size_t Axis = 0; Axis < Point.size(); ++Axis) {
				if (!Value[Axis].is_number()) return std::nullopt;
				Point[Axis] = Value[Axis].get<double>();
				if (!std::isfinite(Point[Axis])) return std::nullopt;
			}
			return terrain::WorldPoint{Point[0], Point[1], Point[2]};
		}

		std::optional<terrain::SamplePoint> DecodeTerrainSamplePoint(const Json &Value) {
			if (!Value.is_array() || Value.size() != 3) return std::nullopt;
			std::array<std::int64_t, 3> Point;
			constexpr auto Bound = terrain::MaximumSampleCoordinate + 1;
			for (std::size_t Axis = 0; Axis < Point.size(); ++Axis) {
				if (!Value[Axis].is_number_integer()) return std::nullopt;
				const auto Component = Value[Axis].get<double>();
				if (Component < -Bound || Component > Bound) return std::nullopt;
				Point[Axis] = Value[Axis].get<std::int64_t>();
			}
			return terrain::SamplePoint{Point[0], Point[1], Point[2]};
		}

		std::expected<EditorTerrainBrush, std::string_view> DecodeTerrainBrush(const Json &Value, terrain::GridSpec Grid) {
			using namespace terrain;
			if (!Value.is_object() || !Value.contains("Shape") || !Value["Shape"].is_string())
				return std::unexpected("Terrain brush needs a Shape");
			const auto &Shape = Value["Shape"].get_ref<const std::string &>();
			if (Shape == "Smooth") {
				if (!HasOnlyFields(Value, {"Shape", "Min", "MaxExclusive", "Strength"}) || Value.size() != 4)
					return std::unexpected("Smooth requires Min, MaxExclusive and Strength");
				const auto Min = DecodeTerrainSamplePoint(Value["Min"]);
				const auto Max = DecodeTerrainSamplePoint(Value["MaxExclusive"]);
				const auto Strength = JsonCodec::DecodeUnsigned32(Value["Strength"]);
				if (!Min || !Max || !Strength || *Strength == 0 || *Strength > 255)
					return std::unexpected("Smooth requires integer lattice bounds and strength from 1 to 255");
				const SampleBox Bounds{*Min, *Max};
				if (auto Keys = GetRegionChunks(Bounds, 1); !Keys)
					return std::unexpected(GetFieldErrorText(Keys.error()));
				return EditorTerrainBrush{SmoothEdit{Bounds, static_cast<std::uint8_t>(*Strength)}, Bounds};
			}
			const bool Sphere = Shape == "Sphere";
			if ((!Sphere && Shape != "Box") ||
				!HasOnlyFields(Value, {"Shape", "Mode", "Center", Sphere ? "Radius" : "HalfExtent", "Material"}) ||
				!Value.contains("Mode") || !Value["Mode"].is_string() || !Value.contains("Center"))
				return std::unexpected("Sphere and Box require Mode, Center and their size");
			const auto &ModeValue = Value["Mode"].get_ref<const std::string &>();
			const auto Mode = ModeValue == "Add" ? std::optional(BrushMode::Add) :
				ModeValue == "Subtract" ? std::optional(BrushMode::Subtract) :
				ModeValue == "Paint" ? std::optional(BrushMode::Paint) : std::nullopt;
			if (!Mode) return std::unexpected("Terrain brush Mode must be Add, Subtract or Paint");
			MaterialId Material = 0;
			if (*Mode == BrushMode::Subtract) {
				if (Value.contains("Material")) return std::unexpected("Subtract clears material from excavated samples");
			} else {
				const auto Id = Value.contains("Material") ? JsonCodec::DecodeUnsigned32(Value["Material"]) : std::nullopt;
				if (!Id || *Id == 0) return std::unexpected("Add and Paint require a nonzero material ID");
				Material = *Id;
			}
			const auto Center = DecodeTerrainWorldPoint(Value["Center"]);
			if (!Center) return std::unexpected("Terrain brush Center must contain three finite world coordinates");
			if (Sphere) {
				if (!Value.contains("Radius") || !Value["Radius"].is_number())
					return std::unexpected("Sphere requires Radius in world units");
				const SphereBrush Brush{*Center, Value["Radius"].get<double>(), *Mode, Material};
				const auto Bounds = GetBrushBounds(Grid, Brush);
				if (!Bounds) return std::unexpected(GetFieldErrorText(Bounds.error()));
				return EditorTerrainBrush{Brush, *Bounds};
			}
			const auto HalfExtent = Value.contains("HalfExtent") ? DecodeTerrainWorldPoint(Value["HalfExtent"]) : std::nullopt;
			if (!HalfExtent) return std::unexpected("Box requires three finite HalfExtent components in world units");
			const BoxBrush Brush{*Center, *HalfExtent, *Mode, Material};
			const auto Bounds = GetBrushBounds(Grid, Brush);
			if (!Bounds) return std::unexpected(GetFieldErrorText(Bounds.error()));
			return EditorTerrainBrush{Brush, *Bounds};
		}

		std::optional<std::size_t> CountTerrainBrushSamples(terrain::SampleBox Bounds) {
			std::size_t Count = 1;
			for (const auto Size : {Bounds.MaxExclusive.X - Bounds.Min.X, Bounds.MaxExclusive.Y - Bounds.Min.Y,
				Bounds.MaxExclusive.Z - Bounds.Min.Z}) {
				if (Size <= 0 || static_cast<std::uint64_t>(Size) > MaximumTerrainBrushSamples / Count)
					return std::nullopt;
				Count *= static_cast<std::size_t>(Size);
			}
			return Count;
		}

		GuiTextureChanges CollectEditorViewportAssets(const std::shared_ptr<DataModel> &World, RenderPublisher &Publisher,
			std::span<const RenderTextureRemove> RetiredTextures) {
			auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
			if (!Assets) throw std::runtime_error("Editor viewport has no canonical AssetService");

			auto Textures = Assets->DrainTextureChanges();
			if (!RetiredTextures.empty())
				Textures.Removes.insert(Textures.Removes.end(), RetiredTextures.begin(), RetiredTextures.end());
			auto Meshes = Assets->DrainMeshChanges();
			if (!Meshes.Creates.empty() || !Meshes.Removes.empty())
				Publisher.SetAssetMeshChanges(std::move(Meshes.Creates), std::move(Meshes.Removes));
			return {std::move(Textures.Creates), std::move(Textures.Updates), std::move(Textures.Removes), Textures.UploadBytes};
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

		void RequirePlainPlayStorageDirectory(const std::filesystem::path &Path) {
			if (!Path.is_absolute() || !std::filesystem::is_directory(std::filesystem::symlink_status(Path)))
				throw std::runtime_error("Invalid Play storage directory");
#if defined(_WIN32)
			const auto Attributes = GetFileAttributesW(Path.c_str());
			if (Attributes == INVALID_FILE_ATTRIBUTES || (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
				throw std::runtime_error("Invalid Play storage directory");
#endif
		}

		std::filesystem::path GetPlayStorageRoot() {
			// This optional override is a trusted launch environment input, never a wire parameter.
			if (const auto *Override = SDL_getenv("GARGANTUAN_STUDIO_PLAY_DATA_ROOT"); Override && *Override)
				return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(Override)));
			const std::unique_ptr<char, decltype(&SDL_free)> Root(SDL_GetPrefPath("Gargantuan", "Studio"), SDL_free);
			if (!Root) throw std::runtime_error("No Studio user-data directory");
			return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(Root.get())));
		}

		std::shared_ptr<IPersistenceProvider> OpenPersistentPlayStorage(const Project &ProjectValue, bool Create) {
			const auto CanonicalProjectRoot = std::filesystem::canonical(ProjectValue.Root);
			const auto ProjectRoot = Paths::ToUtf8(CanonicalProjectRoot);
			const auto Identity = ProjectValue.Identity.ToString();
			const auto Digest = AssetContentId::Hash(
				std::span(reinterpret_cast<const std::uint8_t *>(ProjectRoot.data()), ProjectRoot.size())).ToString();
			auto Root = GetPlayStorageRoot();
			while (!Root.has_filename() && Root.has_relative_path()) Root = Root.parent_path();
			RequirePlainPlayStorageDirectory(Root);
			const auto Directory = std::filesystem::canonical(Root) / "PlayPersistence-v1";
			const auto Common = std::mismatch(CanonicalProjectRoot.begin(), CanonicalProjectRoot.end(),
				Directory.begin(), Directory.end());
			if (Common.first == CanonicalProjectRoot.end() || Common.second == Directory.end())
				throw std::runtime_error("Play storage and authored project roots overlap");
			if (Create) std::filesystem::create_directory(Directory);
			else if (!std::filesystem::exists(std::filesystem::symlink_status(Directory))) return {};
			RequirePlainPlayStorageDirectory(Directory);
			return MakeDurableLocalPersistenceProvider({
				.Path = Directory / (Identity + "-" + Digest + ".sqlite3"),
				.ProfileId = Identity + ":" + ProjectRoot,
				.Limits = {},
				.CreateIfMissing = Create,
			});
		}

		Json EncodePlayStorageDocument(const PersistenceDocumentValue &Value) {
			return std::visit([](const auto &Entry) -> Json {
				using EntryType = std::decay_t<decltype(Entry)>;
				if constexpr (std::is_same_v<EntryType, std::monostate>) {
					return nullptr;
				} else if constexpr (std::is_same_v<EntryType, PersistenceDocumentValue::Array>) {
					Json Array = Json::array();
					for (const auto &Item : Entry) Array.push_back(EncodePlayStorageDocument(Item));
					return Array;
				} else if constexpr (std::is_same_v<EntryType, PersistenceDocumentValue::Object>) {
					Json Object = Json::object();
					for (const auto &Field : Entry) Object[Field.Name] = EncodePlayStorageDocument(Field.Value);
					return Object;
				} else {
					return Entry;
				}
			}, Value.Data);
		}

		Json EncodePlayStorageRecord(const PersistenceRecord &Record) {
			Json Result = {{"Revision", std::to_string(Record.Revision)}};
			if (const auto *Bytes = std::get_if<PersistenceBytes>(&Record.Payload)) {
				std::vector<std::uint8_t> BytesForWire;
				BytesForWire.reserve(Bytes->size());
				for (const auto Byte : *Bytes) BytesForWire.push_back(std::to_integer<std::uint8_t>(Byte));
				Result["ByteCount"] = Bytes->size();
				Result["BytesBase64"] = EncodeBase64(BytesForWire);
			} else {
				Result["Document"] = EncodePlayStorageDocument(std::get<PersistenceDocumentValue>(Record.Payload));
			}
			return Result;
		}

		std::string PreRunRevision(const std::optional<std::string> &Source) {
			if (!Source) return "missing";
			return AssetContentId::Hash(std::span(reinterpret_cast<const std::uint8_t *>(Source->data()), Source->size())).ToString();
		}

		Json EncodeBase64Chunks(const std::vector<std::uint8_t> &Bytes) {
			const auto Encoded = EncodeBase64(Bytes);
			Json Chunks = Json::array();
			for (std::size_t Offset = 0; Offset < Encoded.size(); Offset += MaximumProtocolStringBytes)
				Chunks.push_back(Encoded.substr(Offset, MaximumProtocolStringBytes));
			if (Chunks.empty()) Chunks.push_back("");
			return Chunks;
		}

		std::vector<std::uint8_t> DecodeBase64Chunks(const Json &Chunks, std::size_t MaximumBytes) {
			const auto MaximumEncoded = ((MaximumBytes + 2) / 3) * 4;
			const auto MaximumChunks = (MaximumEncoded + MaximumProtocolStringBytes - 1) / MaximumProtocolStringBytes;
			if (!Chunks.is_array() || Chunks.empty() || Chunks.size() > MaximumChunks)
				throw std::invalid_argument("Base64 chunk count exceeds its limit");
			std::string Encoded;
			for (const auto &Chunk : Chunks) {
				if (!Chunk.is_string()) throw std::invalid_argument("Base64 chunk must be a string");
				const auto &Text = Chunk.get_ref<const std::string &>();
				if (Text.size() > MaximumProtocolStringBytes || Text.size() > MaximumEncoded - Encoded.size())
					throw std::invalid_argument("Base64 data exceeds its byte limit");
				Encoded += Text;
			}
			if (Encoded.size() % 4 != 0) throw std::invalid_argument("Base64 length is invalid");
			constexpr std::string_view Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::vector<std::uint8_t> Bytes;
			Bytes.reserve(Encoded.size() / 4 * 3);
			for (std::size_t Offset = 0; Offset < Encoded.size(); Offset += 4) {
				std::uint32_t Value = 0;
				for (std::size_t Index = 0; Index < 4; ++Index) {
					const char Character = Encoded[Offset + Index];
					const auto Digit = Alphabet.find(Character);
					if (Digit == std::string_view::npos && !(Character == '=' && Index >= 2 && Offset + 4 == Encoded.size()))
						throw std::invalid_argument("Base64 is invalid");
					Value = (Value << 6) | (Character == '=' ? 0 : static_cast<std::uint32_t>(Digit));
				}
				Bytes.push_back(static_cast<std::uint8_t>(Value >> 16));
				if (Encoded[Offset + 2] != '=') Bytes.push_back(static_cast<std::uint8_t>(Value >> 8));
				if (Encoded[Offset + 3] != '=') Bytes.push_back(static_cast<std::uint8_t>(Value));
			}
			if (Bytes.size() > MaximumBytes || EncodeBase64(Bytes) != Encoded)
				throw std::invalid_argument("Base64 data is oversized or noncanonical");
			return Bytes;
		}

		Json EncodePreRunSource(std::string_view Source) {
			ValidateProtocolString(Source, MaximumPreRunSourceBytes, "PreRun source");
			return EncodeBase64Chunks(std::vector<std::uint8_t>(Source.begin(), Source.end()));
		}

		std::string DecodePreRunSource(const Json &Chunks) {
			const auto Bytes = DecodeBase64Chunks(Chunks, MaximumPreRunSourceBytes);
			std::string Source(Bytes.begin(), Bytes.end());
			ValidateProtocolString(Source, MaximumPreRunSourceBytes, "PreRun source");
			return Source;
		}
	}

	struct EditorHost::AnimationPreviewJob final {
		std::uint64_t Id = 0;
		std::unique_ptr<AnimationPreview> Scene;
		std::unique_ptr<EditorViewportRenderer> Renderer;
		std::unique_ptr<SharedFrameRing> Ring;
		std::chrono::steady_clock::time_point LastStep = std::chrono::steady_clock::now();
		Json Rig;
	};

	struct EditorHost::GuiPreviewState final {
		struct DisplayedFrame {
			std::uint64_t Number = 0;
			std::uint64_t ProjectRevision = 0;
			GuiPreviewPlacement Placement;
			std::shared_ptr<const GuiLayoutSnapshot> Layout;
		};

		GuiRuntime Runtime;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		std::uint64_t LayoutGeneration = 0;
		std::uint64_t PlacementGeneration = 0;
		GuiPreviewPlacement Placement;
		std::shared_ptr<const RenderUiFrame> PlacedFrame;
		std::shared_ptr<const GuiLayoutSnapshot> DisplayedLayout;
		std::deque<DisplayedFrame> DisplayedFrames;

		explicit GuiPreviewState(const std::shared_ptr<DataModel> &World)
			: Runtime(World, Paths::GetExecutableDirectory() / "runtime" / "GargantuanSans.ttf",
				GuiRuntimeMode::AuthoringPreview) {}
	};

	struct EditorHost::TerrainAuthoringState final {
		enum class Action { Brush, Undo, Redo };

		struct BrushHistory {
			terrain::Revision AtRevision{};
			bool Undone{};
			std::vector<terrain::SampleWrite> Before;
			std::vector<terrain::SampleWrite> After;
		};

		struct Work {
			terrain::EditTicket Ticket{};
			terrain::Revision StartingRevision{};
			Action Operation{Action::Brush};
			std::optional<BrushHistory> Patch;
		};

		std::optional<Work> Pending;
		std::optional<BrushHistory> History;
	};

	struct EditorHost::PackageJob final {
		std::uint64_t Id = 0;
		std::uint64_t CapturedRevision = 0;
		bool CapturedUnsavedChanges = false;
		std::atomic<PackagePhase> Phase{PackagePhase::Snapshot};
		std::atomic_bool Complete{false};
		PackageCancellationToken Cancellation;
		std::mutex ResultMutex;
		std::optional<PackageBuildResult> Result;
		std::jthread Worker;
	};

	EditorHost::EditorHost(std::string sessionToken) :
		EditorHost(std::move(sessionToken), ScriptSecurityContext::StudioCoreUi()) {}

	EditorHost::EditorHost(std::string sessionToken, bool AudioEnabledValue,
		std::optional<EditorNodeDevelopmentPersistence> NodeDevelopmentValue) :
		EditorHost(std::move(sessionToken), ScriptSecurityContext::StudioCoreUi()) {
		AudioEnabled = AudioEnabledValue;
		NodeDevelopment = std::move(NodeDevelopmentValue);
	}

	EditorHost::EditorHost(std::string sessionToken, ScriptSecurityContext studioSecurity) :
		SessionToken(std::move(sessionToken)), StudioSecurity(std::move(studioSecurity)) {
		if (SessionToken.empty() || SessionToken.size() > 256)
			throw std::invalid_argument("EditorHost requires a nonempty bounded session token");
	}

	EditorHost::~EditorHost() {
		ActiveAnimationPreview.reset();
		GuiPreview.reset();
		ResetViewportSpatial();
		ViewportPresentationActive = false;
		ViewportLatestFrameMailbox.reset();
		if (ActivePackageJob) {
			ActivePackageJob->Cancellation.Cancel();
			ActivePackageJob.reset();
		}
		if (ActivePlaySession) {
			ActivePlaySession->Stop();
			ActivePlaySession.reset();
		}
		if (!World) return;
		try {
			(void)World->Transactions.TerminateOwner(*World, TransactionOwner);
		} catch (...) {
			World->Transactions.Reset();
		}
		try {
			// Retire authored subtrees explicitly so Instance reference cycles break.
			ScopedChangeJournalSuppression Suppression;
			World->Destroy();
		} catch (...) {
			// Host teardown must remain nonthrowing.
		}
	}

	void EditorHost::ResetViewportSpatial() {
		ViewportPublisher->SetSpatialResolver(nullptr);
		if (ViewportSpatial) ViewportSpatial->Shutdown();
		ViewportSpatial.reset();
	}

	void EditorHost::ResetEditorViewportDocument() {
		auto NewPublisher = std::make_unique<RenderPublisher>();
		TerrainAuthoring.reset();
		ResetViewportSpatial();
		GuiPreview.reset();
		RetiredGuiPreviewTextures.clear();
		GuiPreviewFramePublished = false;
		ViewportRenderer.reset();
		ViewportPublisher = std::move(NewPublisher);
	}

	void EditorHost::StepViewportSpatial() {
		if (!World) return;
		if (!ViewportSpatial) {
			const auto ExistingAssets = World->FindService("AssetService");
			const auto Assets = ExistingAssets ? std::dynamic_pointer_cast<AssetService>(*ExistingAssets) : nullptr;
			if (!Assets) return;
			ViewportSpatial = std::make_shared<SemanticSpatialResolver>(Assets);
			ViewportPublisher->SetSpatialResolver(ViewportSpatial);
		}
		// The bounded visual producer registers required attachments on extraction.
		// Existing observations then update without another whole-scene walk.
		ViewportSpatial->Step();
	}

	void EditorHost::ResetGuiPreview() {
		if (!GuiPreview) return;
		auto Retired = GuiPreview->Runtime.GetPreviewTextureRemovals();
		RetiredGuiPreviewTextures.insert(RetiredGuiPreviewTextures.end(), Retired.begin(), Retired.end());
		GuiPreview.reset();
	}

	void EditorHost::PublishEditViewport(Workspace &WorkspaceService, const RenderCameraInput &Camera) {
		if (GuiPreview) {
			auto &Preview = *GuiPreview;
			const auto Placement = GetGuiPreviewPlacement(Preview.Width, Preview.Height, ViewportWidth, ViewportHeight);
			const GuiViewportConfiguration Configuration{Placement.Width, Placement.Height, 1.0f, {}};
			Preview.Runtime.SetViewport(Configuration);
			(void)Preview.Runtime.Reconcile();
			const auto Presentation = Preview.Runtime.GetCommittedPresentation();
			if (!Preview.PlacedFrame || Preview.LayoutGeneration != Presentation->Generation || Preview.Placement != Placement) {
				const auto Layout = Preview.Runtime.GetPresentedLayout();
				if (Layout && Layout->Viewport == Configuration) {
					Preview.PlacedFrame = PlaceGuiPreview(*Presentation, Placement);
					Preview.DisplayedLayout = Layout;
				} else {
					Preview.PlacedFrame = std::make_shared<const RenderUiFrame>();
					Preview.DisplayedLayout.reset();
				}
				Preview.LayoutGeneration = Presentation->Generation;
				Preview.Placement = Placement;
				Preview.PlacementGeneration = NextGuiPreviewPlacementGeneration++;
			}
			Preview.Runtime.Publish(*ViewportPublisher, Preview.PlacedFrame, Preview.PlacementGeneration,
				CollectEditorViewportAssets(World, *ViewportPublisher, RetiredGuiPreviewTextures));
			GuiPreviewFramePublished = true;
		} else {
			if (GuiPreviewFramePublished) {
				ViewportPublisher->SetUiFrame(RenderUiFrame{});
				ViewportPublisher->SetViewportScenes(nullptr);
				GuiPreviewFramePublished = false;
			}
			auto Textures = CollectEditorViewportAssets(World, *ViewportPublisher, RetiredGuiPreviewTextures);
			if (!Textures.Creates.empty() || !Textures.Updates.empty() || !Textures.Removes.empty())
				ViewportPublisher->SetUiTextureChanges(std::move(Textures.Creates),
					std::move(Textures.Updates), std::move(Textures.Removes));
		}
		RetiredGuiPreviewTextures.clear();
		StepViewportSpatial();
		LastViewportPublication = ViewportPublisher->Publish(
			WorkspaceService, Camera, ViewportWidth, ViewportHeight
		);
	}

	void EditorHost::RecordGuiPreviewFrame(std::uint64_t FrameNumber) {
		if (!GuiPreview || ActivePlaySession || !GuiPreview->DisplayedLayout) return;
		auto &Frames = GuiPreview->DisplayedFrames;
		if (Frames.size() == MaximumGuiPreviewFrames) Frames.pop_front();
		const auto Revision = GuiPreview->Runtime.IsPresentedLayoutCurrent() ? World->GetAuthoritativeRevision() : 0;
		Frames.push_back({FrameNumber, Revision, GuiPreview->Placement, GuiPreview->DisplayedLayout});
	}

	void EditorHost::AdvanceViewportGeneration() {
		if (GuiPreview) GuiPreview->DisplayedFrames.clear();
		ViewportGeneration = ViewportGeneration == std::numeric_limits<std::uint64_t>::max()
			? 1 : ViewportGeneration + 1;
		ViewportFrameNumber = 0;
		ViewportLastPlaySessionId = 0;
		ViewportLastCameraRevision = 0;
		ViewportLastSourceRevision = 0;
		ViewportLastMode = LatestFrameMailboxLayout::Mode::Edit;
		LastViewportPublication.reset();
		ViewportProjection.Clear();
		ViewportPublisher->RequestFullResync();
		if (ActivePlaySession && ActivePlaySession->GetState() == PlaySessionState::Running)
			ActivePlaySession->RequestRenderFullResync();
		NextViewportFrame = std::chrono::steady_clock::now();
	}

	void EditorHost::EnsureViewportRenderer() {
		if (ViewportRenderer) return;
		ViewportRenderer = std::make_unique<EditorViewportRenderer>(ViewportWidth, ViewportHeight);
		if (!ActivePlaySession || ActivePlaySession->GetState() != PlaySessionState::Running) {
			ViewportPublisher->RequestFullResync();
			return;
		}
		// A CPU-only consumer may have advanced past the full publication before
		// native capture was enabled. A new native renderer cannot start mid-chain.
		if (ViewportProjection.GetLastPublicationId() != InvalidRenderPublicationId) {
			LastViewportPublication.reset();
			ViewportProjection.Clear();
			ActivePlaySession->RequestRenderFullResync();
		}
	}

	void EditorHost::ConsumePlayRenderPublications(EditorViewportRenderer *Renderer) {
		if (!ActivePlaySession || ActivePlaySession->GetState() != PlaySessionState::Running ||
			ViewportWidth == 0 || ViewportHeight == 0) return;
		auto Publications = ActivePlaySession->TakeRenderPublications();
		if (Publications.empty()) return;
		for (auto &Publication : Publications) {
			if (!Publication) continue;
			if (ViewportProjection.GetLastPublicationId() != Publication->Id)
				(void)ViewportProjection.Apply(*Publication);
			if (Renderer) Renderer->ApplyPublication(Publication);
			else if (ViewportApplyForTesting) ViewportApplyForTesting(Publication);
			LastViewportPublication = std::move(Publication);
		}
	}

	void EditorHost::PumpPlaySession() {
		if (!ActivePlaySession || ActivePlaySession->GetState() != PlaySessionState::Running) return;
		const auto Now = std::chrono::steady_clock::now();
		if (NextPlayStep != std::chrono::steady_clock::time_point{} && Now < NextPlayStep) return;
		NextPlayStep = Now + std::chrono::microseconds(8333);
		ActivePlaySession->Step();
		if (ActivePlaySession->GetState() != PlaySessionState::Running) {
			LastPlaySessionState = ActivePlaySession->GetState();
			AdvanceViewportGeneration();
			ViewportRenderer.reset();
		}
	}

	void EditorHost::PumpViewportPresentation() {
		if (!ViewportPresentationActive || !ViewportLatestFrameMailbox || !World || !ViewportCamera ||
			ViewportWidth == 0 || ViewportHeight == 0) return;
		const auto Now = std::chrono::steady_clock::now();
		if (NextViewportFrame != std::chrono::steady_clock::time_point{} && Now < NextViewportFrame) return;
		NextViewportFrame = Now + ViewportFrameInterval;
		try {
			if (ViewportPresentationCheckpointForTesting)
				ViewportPresentationCheckpointForTesting("BeforeCapture");
			if (!ViewportCaptureForTesting) EnsureViewportRenderer();
			auto WorkspaceService = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
			if (!WorkspaceService) throw std::runtime_error("Project has no Workspace for viewport presentation");

			LatestViewportFrameMetadata Metadata{.Generation = ViewportGeneration};
			if (ActivePlaySession && ActivePlaySession->GetState() == PlaySessionState::Running) {
				ConsumePlayRenderPublications(ViewportCaptureForTesting ? nullptr : ViewportRenderer.get());
				if (!LastViewportPublication) return;
				Metadata.Mode = LatestFrameMailboxLayout::Mode::Play;
				Metadata.PlaySessionId = ActivePlaySession->GetId().Value;
			} else {
				PublishEditViewport(*WorkspaceService, *ViewportCamera);
				Metadata.Mode = LatestFrameMailboxLayout::Mode::Edit;
			}
			if (ViewportProjection.GetLastPublicationId() != LastViewportPublication->Id)
				(void)ViewportProjection.Apply(*LastViewportPublication);
			const auto Captured = ViewportCaptureForTesting
				? ViewportCaptureForTesting(LastViewportPublication)
				: ViewportRenderer->CaptureBgra(LastViewportPublication);
			if (ViewportPresentationCheckpointForTesting)
				ViewportPresentationCheckpointForTesting("AfterCapture");
			Metadata.CameraRevision = ViewportCameraRevision;
			Metadata.SourceRevision = LastViewportPublication->Id;
			Metadata.RenderSubmission = Captured.RenderSubmission;
			Metadata.GpuReadbackWait = Captured.GpuReadbackWait;
			Metadata.CpuExtraction = Captured.CpuExtraction;
			if (ViewportPresentationCheckpointForTesting)
				ViewportPresentationCheckpointForTesting("BeforePublish");
			ViewportFrameNumber = ViewportLatestFrameMailbox->Publish(
				Captured.Width, Captured.Height, Captured.BgraPixels, Metadata
			);
			if (Metadata.Mode == LatestFrameMailboxLayout::Mode::Edit) RecordGuiPreviewFrame(ViewportFrameNumber);
			++ViewportProducedFrames;
			ViewportLastRenderSubmissionUs = static_cast<std::uint64_t>(Captured.RenderSubmission.count());
			ViewportLastGpuReadbackUs = static_cast<std::uint64_t>(Captured.GpuReadbackWait.count());
			ViewportLastCpuExtractionUs = static_cast<std::uint64_t>(Captured.CpuExtraction.count());
			ViewportLastMode = Metadata.Mode;
			ViewportLastPlaySessionId = Metadata.PlaySessionId;
			ViewportLastCameraRevision = Metadata.CameraRevision;
			ViewportLastSourceRevision = Metadata.SourceRevision;
			ViewportLastFailure.clear();
		} catch (const std::exception &Error) {
			++ViewportPresentationFailures;
			ViewportLastFailure = Error.what();
			ViewportMostRecentFailure = Error.what();
			ViewportRenderer.reset();
			LastViewportPublication.reset();
			ViewportProjection.Clear();
			ViewportPublisher->RequestFullResync();
			if (ActivePlaySession) ActivePlaySession->RequestRenderFullResync();
		}
	}

	void EditorHost::PumpViewportPresentationForTesting() {
		NextPlayStep = {};
		NextViewportFrame = {};
		PumpTerrainAuthoring();
		PumpPlaySession();
		PumpViewportPresentation();
	}

	void EditorHost::PumpPlaySessionForTesting() {
		NextPlayStep = {};
		PumpPlaySession();
	}

	void EditorHost::PumpTerrainAuthoring() {
		if (!TerrainAuthoring || !TerrainAuthoring->Pending || !World) return;
		const auto Space = std::dynamic_pointer_cast<Workspace>(World->FindFirstChildOfClass("Workspace", false));
		const auto Owner = Space ? Space->GetTerrain() : nullptr;
		if (!Owner) {
			TerrainAuthoring.reset();
			return;
		}
		Space->PollTerrainWork();
		auto &Pending = *TerrainAuthoring->Pending;
		const auto Receipt = Space->GetTerrainTicket(Pending.Ticket);
		if (Receipt && Receipt->Status == "Pending") return;
		const auto &Field = Owner->GetField();
		const auto Revision = Field.GetRevision();
		if (Revision != Pending.StartingRevision) {
			World->AdvanceAuthoritativeRevision();
			if (Receipt && Receipt->Revision) {
				if (Pending.Patch) {
					auto &Patch = *Pending.Patch;
					std::size_t Changed = 0;
					for (const auto Original : Patch.Before) {
						const auto Value = Field.Read(Original.Point);
						if (!Value || *Value == Original.Value) continue;
						Patch.Before[Changed] = Original;
						Patch.After[Changed++] = {Original.Point, *Value};
					}
					Patch.Before.resize(Changed);
					Patch.After.resize(Changed);
					Patch.AtRevision = Revision;
					if (Changed) TerrainAuthoring->History = std::move(Patch);
					else TerrainAuthoring->History.reset();
				} else if (TerrainAuthoring->History) {
					TerrainAuthoring->History->AtRevision = Revision;
					TerrainAuthoring->History->Undone = Pending.Operation == TerrainAuthoringState::Action::Undo;
				}
				ViewportPublisher->SetTerrainRenderState(Space->GetTerrainRenderState());
				NextViewportFrame = std::chrono::steady_clock::now();
			} else {
				// A provider violation after the field commit still makes the scene dirty.
				TerrainAuthoring->History.reset();
			}
		}
		TerrainAuthoring->Pending.reset();
	}

	bool EditorHost::HasNodeDevelopmentPersistence() const {
		return NodeDevelopment && NodeDevelopment->Admit &&
			StudioSecurity.HasCapability(ScriptCapability::EditorCommands) &&
			StudioSecurity.HasCapability(ScriptCapability::ReadDataModel);
	}

	bool EditorHost::MatchesNodeDevelopmentProject(const std::optional<Project> &Candidate) const {
		if (!HasNodeDevelopmentPersistence() || !Candidate || NodeDevelopment->Project != Candidate->Identity)
			return false;
		std::error_code Error;
		return std::filesystem::equivalent(Candidate->Root, NodeDevelopment->ProjectRoot, Error) && !Error;
	}

	void EditorHost::ResetAuthoringCursors() {
		Cursor.reset();
		TeamCursors.fill(std::nullopt);
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
			if (!HasOnlyFields(message, {"Version", "RequestId", "SessionToken", "Method", "Params", "Team"}) ||
				!message.contains("Version") || JsonCodec::DecodeUnsigned32(message["Version"]) != EditorHostProtocolVersion ||
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
			std::uint64_t TeamAuthor = 0;
			if (message.contains("Team")) {
				// Only the owning broker can add author identity over the private authenticated stdio channel.
				const auto &Team = message["Team"];
				if (!HasOnlyFields(Team, {"Author"}) || !Team.contains("Author") ||
					!Team["Author"].is_number_unsigned())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Invalid Team author context"));
				TeamAuthor = Team["Author"].get<std::uint64_t>();
				if (TeamAuthor == 0 || TeamAuthor > TeamCursors.size())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Team author is out of range"));
				if (method != "Handshake" && method != "GetProjectState" && method != "GetSchema" &&
					method != "GetSnapshot" && method != "PollChanges" && method != "GetTeamHistory" &&
					method != "GetScriptSource" && method != "GetScriptBinding" && method != "ResolveScriptBinding" &&
					method != "SetProperty" && method != "SetPropertyBatch" && method != "SetTransform" &&
					method != "SetAttribute" && method != "SetExtensionProperty" && method != "SetCustomProperty" &&
					method != "AddTag" && method != "RemoveTag" && method != "SetScriptSource" &&
					method != "CreateInstance" && method != "DestroyInstance" && method != "DuplicateInstance" &&
					method != "ReparentInstance" && method != "Undo" && method != "Redo" && method != "SaveProject")
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Method is unavailable in Team Create"));
				if (parameters.contains("TransactionId"))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Team requests cannot join open transactions"));
				if (World && World->Transactions.GetOpenCount() != 0)
					return SerializeBoundedResponse(ErrorResponse(requestId, "TransactionOpen", "A local authoring transaction is open"));
			}
			const auto EncodeActiveProjectState = [&](const std::shared_ptr<DataModel> &Document,
				std::uint64_t SavedRevision, const std::optional<Project> &Project) {
				auto State = EncodeProjectState(Document, SavedRevision, Project, TeamAuthor);
				if (TeamAuthor == 0 && HasNodeDevelopmentPersistence())
					State["PlayNodeDevelopmentAvailable"] = MatchesNodeDevelopmentProject(Project);
				return State;
			};
			auto &RequestCursor = TeamAuthor == 0 ? Cursor : TeamCursors[TeamAuthor - 1];
			// Batch failures and diagnostic operations cannot commit an unrelated
			// expired legacy group. These diagnostics do not edit document state.
			if (TeamAuthor == 0 && World && method != "SetPropertyBatch" && method != "GetStreamingDiagnostics" &&
				method != "ConfigureStreamingDiagnostics" && method != "GetPlayInputSnapshot" &&
				method != "InspectPlayPersistenceRecord" && method != "ResetPlayPersistenceRecord" &&
				method != "GetCharacterComposition" && method != "GetAnimationRig" &&
				method != "OpenAnimationPreview" && method != "ControlAnimationPreview" &&
				method != "CaptureAnimationPreview" && method != "CloseAnimationPreview")
				(void)World->Transactions.ExpireOwner(*World, TransactionOwner);
			if (ActivePlaySession && IsAuthoringMethodWhilePlaying(method))
				return SerializeBoundedResponse(ErrorResponse(
					requestId, "PlaySessionActive", "Stop Play before changing the authoritative project"
				));
			if (TerrainAuthoring && TerrainAuthoring->Pending && method != "CancelTerrainEdit" &&
				(IsAuthoringMethodWhilePlaying(method) || method == "StartPlaySession" || method == "ExportGasPlace"))
				return SerializeBoundedResponse(ErrorResponse(requestId, "TerrainEditPending",
					"Wait for the terrain edit to finish or cancel it first"));
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
			std::optional<std::uint64_t> ExpectedRevision;
			if (parameters.contains("ExpectedRevision")) {
				if (!parameters["ExpectedRevision"].is_number_unsigned())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "ExpectedRevision must be a positive unsigned integer"
					));
				const auto Value = parameters["ExpectedRevision"].get<std::uint64_t>();
				if (Value == 0)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "ExpectedRevision must be a positive unsigned integer"
					));
				ExpectedRevision = Value;
			}
			if (TeamAuthor != 0 && World && IsAuthoringMethodWhilePlaying(method)) {
				if (!RequestCursor)
					return SerializeBoundedResponse(ErrorResponse(requestId, "SnapshotRequired", "GetSnapshot must establish a journal cursor"));
				if (RequestCursor->NextSequence < ChangeJournal::Get().GetRetentionWindow(RequestCursor->Scope).OldestSequence) {
					RequestCursor.reset();
					return SerializeBoundedResponse(ErrorResponse(requestId, "ResnapshotRequired", "Journal cursor was evicted"));
				}
				if (!ExpectedRevision && (method == "SetTransform" ||
					method == "CreateInstance" || method == "DestroyInstance" || method == "DuplicateInstance" ||
					method == "ReparentInstance" || method == "Undo" || method == "Redo"))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Team operation requires ExpectedRevision"));
			}
			auto HasRevisionConflict = [&] {
				return ExpectedRevision && World && World->GetAuthoritativeRevision() != *ExpectedRevision;
			};
			const bool viewportMethod = method == "OpenViewportTransport" || method == "CloseViewportTransport" ||
				method == "ConfigureViewport" || method == "SetViewportCamera" ||
				method == "StartViewportPresentation" || method == "StopViewportPresentation" ||
				method == "GetViewportPresentationState" || method == "CaptureViewport" || method == "PickViewport" ||
				method == "SetGuiPreview" || method == "GetGuiPreview" || method == "PickGuiPreview";
			if (viewportMethod && !StudioSecurity.HasCapability(ScriptCapability::ViewportControl))
				return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Viewport access requires ViewportControl"));
			if (method == "Handshake") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Handshake takes no parameters"));
				constexpr std::uint32_t ScriptSecurityVersion = 1;
				const auto StudioExecutionDomain = GetScriptExecutionDomainName(StudioSecurity.Domain);
				if (TeamAuthor != 0)
					return SerializeBoundedResponse(SuccessResponse(requestId, {
						{"Engine", "Gargantuan"}, {"ProtocolVersion", EditorHostProtocolVersion},
						{"PropertyBatchVersion", EditorHostPropertyBatchVersion}, {"TeamAuthoringVersion", 1},
						{"ScriptSecurityVersion", ScriptSecurityVersion}, {"StudioExecutionDomain", StudioExecutionDomain},
						{"Capabilities", {"Schema", "Snapshot", "Journal", "AuthoritativeRevision", "SetProperty",
							"SetPropertyBatch", "SetTransform", "SetAttribute", "SetExtensionProperty", "SetCustomProperty",
							"AddTag", "RemoveTag", "CreateInstance", "DestroyInstance", "DuplicateInstance", "ReparentInstance",
							"ReadScriptSource", "WriteScriptSource", "ScriptDocumentBinding", "GetTeamHistory", "Undo", "Redo",
							"SaveProject", "CreateInstanceInitialProperties", "CreateScriptWithSource"}}
					}));
				Json capabilities = {
					"OpenProject", "CreateProject", "Schema", "Snapshot", "Journal", "SetProperty", "SetPropertyBatch", "SetTransform", "SetAttribute", "SetExtensionProperty", "SetCustomProperty",
					"AddTag", "RemoveTag", "SaveProject", "SaveProjectAs", "AuthoritativeRevision",
					"CreateInstance", "DestroyInstance", "DuplicateInstance", "ReparentInstance",
					"ExportGasModel", "ImportGasModel", "ExportGasPlace", "OpenGasPlace",
					"TerrainGasPlaceV2", "TerrainMaterialBindingsV1", "TerrainProjectGridV1",
					"TerrainAuthoringV1", "GetTerrainAuthoringState", "SubmitTerrainBrush",
					"GetTerrainEditStatus", "CancelTerrainEdit", "UndoTerrainEdit", "RedoTerrainEdit", "RaycastTerrainSurface",
					"BeginTransaction",
					"CommitTransaction",
					"AuthoritativeTransactions",
					"Undo", "Redo", "AuthoritativeHistoryStatus",
					"OptimisticProjectRevision", "CreateInstanceInitialProperties", "CreateScriptWithSource",
					"ReadScriptSource", "WriteScriptSource", "ScriptDocumentBinding", "PreRunSource", "PreRunCollectionV1", "ValidatePreRun",
					"CustomServiceDeclarations", "ReplicatedStorage",
					"PlaySession", "DiagnosticStream", "PlayScriptFailureLocation", "PlayRootDebug", "SendPlayInput", "StreamingDiagnostics", "PlayInputInspection", "PlayPersistence",
					"AssetCatalog", "ImportAsset", "ReimportAsset", "DeleteAsset", "StrictAssetReferences",
					"AdvancedMaterialMetadata",
					"AnimationRig", "AnimationPreview", "AnimationPreviewBlend", "CharacterComposition",
					"PackageBuild", "PackageBuildProgress", "PackageBuildCancellation", "RuntimeDistributionPolicy",
					"ConfigureViewport", "SetViewportCamera", "CaptureViewport", "PickViewport",
					"ViewportLatestFrame", "ViewportPresentationDiagnostics", "GuiPreview", "GuiPreviewLayout"
				};
				const bool PrivateNode = HasNodeDevelopmentPersistence();
				if (PrivateNode) capabilities.push_back("PlayNodeDevelopmentPersistence");
				const auto ExecutableDirectory = Paths::GetExecutableDirectory();
				if (std::filesystem::is_regular_file(ExecutableDirectory / "RuntimeDistribution" /
					"distributed-player-capability.json")) capabilities.push_back("DistributedPlayerPackages");
				if (std::filesystem::is_regular_file(ExecutableDirectory / "ServerRuntimeDistribution" /
					"runtime-distribution-capability.json")) capabilities.push_back("DedicatedServerPackages");
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
				if (LatestFrameMailbox::IsSupported()) {
					capabilities.push_back("SharedMemoryViewportV2");
					viewportTransports.push_back({
						{"Name", "LatestFrameMailbox"},
						{"Version", LatestFrameMailboxLayout::Version},
						{"PixelFormats", {"BGRA8"}},
						{"SlotCount", LatestFrameMailboxLayout::SlotCount},
					});
				}
				Json Handshake = {
					{"Engine", "Gargantuan"},
					{"ProtocolVersion", EditorHostProtocolVersion},
					{"PropertyBatchVersion", EditorHostPropertyBatchVersion},
					{"StreamingDiagnosticsVersion", 1},
					{"AnimationPreviewBlendVersion", 1},
					{"CharacterCompositionVersion", 1},
					{"PlayPersistenceVersion", 1},
					{"PlayScriptFailureLocationVersion", 1},
					{"PlayRootDebugVersion", 1},
					{"PlayPersistentStorage", SupportsDurableLocalPersistence()},
					{"Capabilities", std::move(capabilities)},
					{"ViewportWireVersion", 2},
					{"ViewportTransports", std::move(viewportTransports)},
					{"ScriptSecurityVersion", ScriptSecurityVersion},
					{"StudioExecutionDomain", StudioExecutionDomain},
					{"StudioCapabilities", EncodeCapabilities(StudioSecurity.Capabilities)},
				};
				if (PrivateNode) {
					Handshake["PlayNodeDevelopmentPersistenceVersion"] = 1;
					Handshake["PlayNodeDevelopmentProjectId"] = NodeDevelopment->Project.ToString();
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, std::move(Handshake)));
			}

			if (method == "GetPreRunSource" || method == "SetPreRunSource" || method == "ValidatePreRun") {
				const bool Write = method == "SetPreRunSource", Read = method == "GetPreRunSource";
				if (!StudioSecurity.HasCapability(ScriptCapability::EditorCommands) ||
					!StudioSecurity.HasCapability(Write ? ScriptCapability::MutateDataModel : ScriptCapability::ReadDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "PreRun authoring requires explicit EditorCommands and read/write authority"));
				if (!World || !CurrentProject) return SerializeBoundedResponse(ErrorResponse(requestId, "ProjectRequired", "Open a project first"));
				if (!HasOnlyFields(parameters, Read ? std::initializer_list<std::string_view>{"CollectionVersion"} :
					std::initializer_list<std::string_view>{"ProjectRoot", "ExpectedSourceRevision", "SourceBase64", "CollectionVersion"}) ||
					(parameters.contains("CollectionVersion") && (!parameters["CollectionVersion"].is_number_integer() || parameters["CollectionVersion"] != 1)) ||
					(!Read && (!parameters.contains("ProjectRoot") || !parameters.contains("ExpectedSourceRevision") ||
						!parameters["ExpectedSourceRevision"].is_string() || !parameters.contains("SourceBase64"))))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Invalid PreRun authoring fields"));
				if (ActivePreRunSchemaGeneration != GetRuntimeSchemaLifecycle().GetActiveGeneration())
					return SerializeBoundedResponse(ErrorResponse(requestId, "StaleSchema", "Reopen the project after schema replacement"));
				const auto Saved = ReadProjectPreRunSource(CurrentProject->Root);
				const bool CollectionClient = parameters.contains("CollectionVersion");
				if (Saved && Saved->starts_with(PreRunCollectionPrefix) && !CollectionClient)
					return SerializeBoundedResponse(ErrorResponse(requestId, "UnsupportedPreRunCollection", "This project requires a collection-aware PreRun editor"));
				const auto Location = EncodeProjectPath(CurrentProject->Root);
				const auto LocationRevision = AssetContentId::Hash(std::span(reinterpret_cast<const std::uint8_t *>(Location.data()), Location.size())).ToString();
				const auto Revision = PreRunRevision(Saved) + "@" + LocationRevision;
				auto State = [&] {
					return Json{{"ProjectRoot", JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(World->GetObjectId()))},
						{"Path", ".gargantuan/prerun.luau"}, {"Exists", Saved.has_value()}, {"SourceRevision", Revision},
						{"ActiveSourceRevision", PreRunRevision(ActivePreRunSource)}, {"SchemaGeneration", ActivePreRunSchemaGeneration},
						{"ReloadRequired", Saved != ActivePreRunSource}, {"MaximumSourceBytes", MaximumPreRunSourceBytes}};
				};
				if (Read) {
					auto Result = State(); Result["SourceBase64"] = EncodePreRunSource(Saved.value_or(""));
					return SerializeBoundedResponse(SuccessResponse(requestId, std::move(Result)));
				}
				const auto Root = JsonCodec::DecodeObjectId(parameters["ProjectRoot"]);
				if (!Root || Root->ToObjectId() != World->GetObjectId())
					return SerializeBoundedResponse(ErrorResponse(requestId, "StaleProject", "PreRun buffer belongs to another project generation"));
				if (parameters["ExpectedSourceRevision"] != Revision)
					return SerializeBoundedResponse(ErrorResponse(requestId, "Conflict", "PreRun source changed; reload or preserve the local buffer"));
				if (World->Transactions.GetOpenCount() != 0 || (ActivePackageJob && !ActivePackageJob->Complete.load()))
					return SerializeBoundedResponse(ErrorResponse(requestId, "LifecycleConflict", "Finish the active transaction or package job first"));
				std::string Source;
				try {
					Source = DecodePreRunSource(parameters["SourceBase64"]);
					if (Source.starts_with(PreRunCollectionPrefix)) {
						if (!CollectionClient) throw std::invalid_argument("PreRun collection support is required");
						(void)ReadPreRunChunks(Source);
					}
				}
				catch (const std::invalid_argument &Error) { return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidSource", Error.what())); }
				if (Write) {
					try {
						if (!CurrentProject->ReplacePreRunSource(Saved, Source, PersistenceCheckpointForTesting))
							throw PreRunSourceConflict("PreRun source changed before commit");
					} catch (const PreRunSourceConflict &Error) {
						return SerializeBoundedResponse(ErrorResponse(requestId, "Conflict", Error.what()));
					}
					auto Result = State(); Result["Exists"] = true; Result["SourceRevision"] = PreRunRevision(Source) + "@" + LocationRevision;
					Result["ReloadRequired"] = std::optional<std::string>(Source) != ActivePreRunSource;
					return SerializeBoundedResponse(SuccessResponse(requestId, std::move(Result)));
				}
				auto Result = State();
				const auto Diagnostic = ValidatePreRunSource(Source);
				Result["Validated"] = !Diagnostic; Result["ActiveSchemaChanged"] = false;
				Result["ValidatedSourceRevision"] = PreRunRevision(Source);
				Result["Diagnostics"] = Json::array();
				if (Diagnostic) {
					auto Message = Diagnostic->Message;
					if (Diagnostic->Source.starts_with("PreRun/")) Message = Diagnostic->Source + ": " + Message;
					Message = Message.substr(0, 4096);
					while (!Message.empty() && !IsValidProtocolUtf8(Message)) Message.pop_back();
					Result["Diagnostics"].push_back({{"Code", GetPreRunDiagnosticCodeName(Diagnostic->Code)},
						{"Source", ".gargantuan/prerun.luau"}, {"Definition", Diagnostic->Definition.substr(0, 256)}, {"Message", Message}});
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, std::move(Result)));
			}

			if (method == "InspectPackagePolicy") {
				if (!StudioSecurity.HasCapability(ScriptCapability::EditorCommands) ||
					!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Package policy preview requires project access."));
				if (!HasOnlyFields(parameters, {"ExpectedRevision", "Configuration", "RuntimeDistribution"}) ||
					!parameters.contains("ExpectedRevision") ||
					!parameters.contains("RuntimeDistribution") || !parameters["RuntimeDistribution"].is_object())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Package policy preview requires a distribution request."));
				if (!World || !CurrentProject)
					return SerializeBoundedResponse(ErrorResponse(requestId, "ProjectRequired", "OpenProject must succeed first."));
				if (World->Transactions.GetOpenCount() != 0 || HasRevisionConflict())
					return SerializeBoundedResponse(ErrorResponse(requestId, "Conflict", "Package policy requires the current committed authoring revision."));
				try {
					const auto Request = ParseRuntimeDistributionRequest(parameters["RuntimeDistribution"].dump());
					const auto Policy = PackageBuilder::CaptureRuntimeDistribution(*CurrentProject, World, Request, false);
					Json Rows = Json::array();
					std::size_t ReportBytes = 0;
					for (const auto &Script : Policy.Scripts) {
						std::string DisplayPath;
						for (const auto &Segment : Script.Path) {
							if (!DisplayPath.empty()) DisplayPath += '/';
							if (DisplayPath.size() + Segment.size() > 1023) {
								DisplayPath += "...";
								break;
							}
							DisplayPath += Segment;
						}
						if (DisplayPath.size() > 1024) DisplayPath = "<long script path>";
						ReportBytes += DisplayPath.size() + 192;
						if (Rows.size() == 128 || ReportBytes > 48 * 1024) break;
						Rows.push_back({
							{"SourceId", Script.SourceId}, {"Path", DisplayPath},
							{"Audience", Script.Eligible ? GetRuntimeDistributionName(Request.Target) : "Excluded"},
							{"Representation", GetRuntimeDistributionName(Script.Representation)},
							{"Diagnostics", GetRuntimeDistributionName(Script.Diagnostics)}, {"Origin", Script.Origin},
						});
					}
					return SerializeBoundedResponse(SuccessResponse(requestId, {
						{"CapturedRevision", World->GetAuthoritativeRevision()},
						{"CapturedUnsavedChanges", World->GetAuthoritativeRevision() != PersistedRevision},
						{"PolicySha256", Policy.PolicySha256}, {"TotalScripts", Policy.Scripts.size()},
						{"Truncated", Rows.size() != Policy.Scripts.size()}, {"Rows", std::move(Rows)},
						{"Diagnostics", Json::array()},
					}));
				} catch (const RuntimeDistributionError &Error) {
					return SerializeBoundedResponse(ErrorResponse(requestId, Error.Code, Error.what()));
				} catch (const std::exception &) {
					return SerializeBoundedResponse(ErrorResponse(requestId, "PackageCaptureFailed", "The package policy could not be previewed safely."));
				}
			}

			if (method == "StartPackageBuild") {
				if (!StudioSecurity.HasCapability(ScriptCapability::EditorCommands) ||
					!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Unauthorized", "Packaging requires EditorCommands and ReadDataModel"
					));
				if (!HasOnlyFields(parameters, {"OutputDirectory", "Configuration", "ExpectedRevision", "Audience",
					"RuntimeDistribution", "ExpectedPolicySha256"}) ||
					!parameters.contains("OutputDirectory") || !parameters["OutputDirectory"].is_string() ||
					parameters["OutputDirectory"].get_ref<const std::string &>().empty() ||
					parameters["OutputDirectory"].get_ref<const std::string &>().size() > 32768 ||
					!parameters.contains("Configuration") || !parameters["Configuration"].is_string() ||
					(parameters.contains("Audience") && !parameters["Audience"].is_string()) ||
					(parameters.contains("RuntimeDistribution") && (!parameters.contains("ExpectedRevision") ||
						!parameters["RuntimeDistribution"].is_object())) ||
					(parameters.contains("ExpectedPolicySha256") &&
						(!parameters.contains("RuntimeDistribution") || !parameters["ExpectedPolicySha256"].is_string() ||
						!AssetContentId::Parse(parameters["ExpectedPolicySha256"].get_ref<const std::string &>()))))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "StartPackageBuild requires a bounded output directory and configuration"
					));
				if (!World || !CurrentProject)
					return SerializeBoundedResponse(ErrorResponse(requestId, "ProjectRequired", "OpenProject must succeed first"));
				if (World->Transactions.GetOpenCount() != 0)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "TransactionOpen", "Commit the open authoring transaction before packaging"
					));
				if (ReadProjectPreRunSource(CurrentProject->Root) != ActivePreRunSource)
					return SerializeBoundedResponse(ErrorResponse(requestId, "PreRunReloadRequired", "Reopen the project to activate saved PreRun before packaging"));
				if (HasRevisionConflict())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Conflict", "The authoritative project revision changed before package capture"
					));
				auto Configuration = ParsePackageConfiguration(parameters["Configuration"].get_ref<const std::string &>());
				if (!Configuration)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "InvalidConfiguration", "Package configuration must be Development or Release"
					));
				auto Audience = ParsePackageAudience(parameters.value("Audience", std::string("Trusted")));
				if (!Audience)
					return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidAudience",
						"Package audience must be Trusted or DistributedPlayer"));
				if (ActivePackageJob && !ActivePackageJob->Complete.load(std::memory_order_acquire))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "PackageBuildActive", "One package build is already active"
					));
				if (NextPackageJobId == 0)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "PackageJobExhausted", "Package job identity is exhausted"
					));

				GamePayload Payload;
				std::optional<RuntimeDistributionRequest> DistributionRequest;
				try {
					if (parameters.contains("RuntimeDistribution")) {
						DistributionRequest = ParseRuntimeDistributionRequest(parameters["RuntimeDistribution"].dump());
						const auto Requested = DistributionRequest->Target == RuntimeDistributionTarget::DistributedPlayer ?
							PackageAudience::DistributedPlayer : PackageAudience::Trusted;
						if (parameters.contains("Audience") && *Audience != Requested)
							throw RuntimeDistributionError("AudienceViolation", "Package target and audience disagree.");
						Audience = Requested;
					}
					Payload = PackageBuilder::Capture(
						*CurrentProject, World, World->GetAuthoritativeRevision(), PersistedRevision, *Audience, DistributionRequest
					);
					if (parameters.contains("ExpectedPolicySha256") &&
						Payload.DistributionPlan->PolicySha256 != parameters["ExpectedPolicySha256"])
						return SerializeBoundedResponse(ErrorResponse(requestId, "PolicyChanged", "The project package policy changed after preview."));
					if (Payload.PreRunSource != ActivePreRunSource)
						return SerializeBoundedResponse(ErrorResponse(requestId, "PreRunReloadRequired", "PreRun source changed during package capture; reopen the project"));
				} catch (const RuntimeDistributionError &Error) {
					return SerializeBoundedResponse(ErrorResponse(requestId, Error.Code, Error.what()));
				} catch (const std::exception &) {
					if (*Audience == PackageAudience::DistributedPlayer)
						return SerializeBoundedResponse(ErrorResponse(requestId, "PackageCaptureFailed",
							"Could not capture a distributed Player package. Check the native schema, client code paths, "
							"and .gargantuan/package-audience.json."));
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "PackageCaptureFailed", "The authoritative project snapshot could not be captured for packaging"
					));
				}
				auto Job = std::make_unique<PackageJob>();
				Job->Id = NextPackageJobId++;
				Job->CapturedRevision = Payload.AuthoritativeRevision;
				Job->CapturedUnsavedChanges = Payload.UnsavedChanges;
				auto *JobPointer = Job.get();
				auto OutputDirectory = std::filesystem::path(parameters["OutputDirectory"].get<std::string>());
				auto RuntimeDistribution = Paths::GetExecutableDirectory() /
					(DistributionRequest && DistributionRequest->Target == RuntimeDistributionTarget::DedicatedServer ?
						"ServerRuntimeDistribution" : "RuntimeDistribution");
				Job->Worker = std::jthread([
					JobPointer,
					Payload = std::move(Payload),
					OutputDirectory = std::move(OutputDirectory),
					RuntimeDistribution = std::move(RuntimeDistribution),
					Configuration = *Configuration
				]() mutable {
					PackageBuildResult BuildResult;
					try {
						BuildResult = PackageBuilder::Build({
							.Payload = std::move(Payload),
							.RuntimeDistributionRoot = std::move(RuntimeDistribution),
							.OutputDirectory = std::move(OutputDirectory),
							.Configuration = Configuration,
							.Cancellation = JobPointer->Cancellation,
							.Progress = [JobPointer](PackagePhase Phase) {
								JobPointer->Phase.store(Phase, std::memory_order_release);
							},
						});
					} catch (...) {
						BuildResult.Diagnostics.push_back({
							PackageDiagnosticSeverity::Error, "Packaging", "InternalFailure",
							"The package build stopped because an internal bounded operation failed.", {}
						});
					}
					{
						std::scoped_lock Lock(JobPointer->ResultMutex);
						JobPointer->Result = std::move(BuildResult);
					}
					JobPointer->Phase.store(PackagePhase::Complete, std::memory_order_release);
					JobPointer->Complete.store(true, std::memory_order_release);
				});
				const auto JobId = Job->Id;
				const auto Revision = Job->CapturedRevision;
				const auto UnsavedChanges = Job->CapturedUnsavedChanges;
				ActivePackageJob = std::move(Job);
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"JobId", std::to_string(JobId)}, {"CapturedRevision", Revision},
					{"CapturedUnsavedChanges", UnsavedChanges}, {"Phase", "Snapshot"},
				}));
			}

			if (method == "GetPackageBuildState" || method == "CancelPackageBuild") {
				if (!StudioSecurity.HasCapability(ScriptCapability::EditorCommands) ||
					!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Unauthorized", "Package job access requires EditorCommands and ReadDataModel"
					));
				if (!HasOnlyFields(parameters, {"JobId"}) || !parameters.contains("JobId"))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "Package job requests require JobId"
					));
				auto JobId = DecodeCanonicalJobId(parameters["JobId"]);
				if (!JobId || !ActivePackageJob || ActivePackageJob->Id != *JobId)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "PackageJobNotFound", "Package job is missing or no longer retained"
					));
				if (method == "CancelPackageBuild") {
					ActivePackageJob->Cancellation.Cancel();
					return SerializeBoundedResponse(SuccessResponse(requestId, {
						{"JobId", std::to_string(*JobId)}, {"CancellationRequested", true},
					}));
				}
				Json Result{
					{"JobId", std::to_string(*JobId)},
					{"CapturedRevision", ActivePackageJob->CapturedRevision},
					{"CapturedUnsavedChanges", ActivePackageJob->CapturedUnsavedChanges},
					{"Phase", GetPackagePhaseName(ActivePackageJob->Phase.load(std::memory_order_acquire))},
					{"Complete", ActivePackageJob->Complete.load(std::memory_order_acquire)},
				};
				if (Result["Complete"].get<bool>()) {
					std::scoped_lock Lock(ActivePackageJob->ResultMutex);
					if (ActivePackageJob->Result) {
						const auto &BuildResult = *ActivePackageJob->Result;
						Json Timings = Json::array();
						for (const auto &Timing : BuildResult.Timings) Timings.push_back({
							{"Phase", GetPackagePhaseName(Timing.Phase)}, {"Microseconds", Timing.Duration.count()},
						});
						Result["Ok"] = BuildResult.Ok;
						Result["Cancelled"] = BuildResult.Cancelled;
						Result["OutputDirectory"] = BuildResult.OutputDirectory.generic_string();
						Result["PlayerExecutable"] = BuildResult.PlayerExecutable.empty()
							? Json(nullptr) : Json(BuildResult.PlayerExecutable.generic_string());
						if (BuildResult.Target) {
							Result["Target"] = GetRuntimeDistributionName(*BuildResult.Target);
							Result["ManifestSha256"] = BuildResult.ManifestSha256;
							Result["PlayerExecutableSha256"] = BuildResult.PlayerExecutableSha256;
							if (*BuildResult.Target == RuntimeDistributionTarget::DedicatedServer) {
								Result["ServerExecutable"] = std::move(Result["PlayerExecutable"]);
								Result["PlayerExecutable"] = nullptr;
							}
							if (!BuildResult.PrivateDiagnosticsPath.empty())
								Result["PrivateDiagnosticsPath"] = BuildResult.PrivateDiagnosticsPath.generic_string();
						}
						Result["Size"] = {
							{"TotalBytes", BuildResult.Size.TotalBytes}, {"RuntimeBytes", BuildResult.Size.RuntimeBytes},
							{"ProjectBytes", BuildResult.Size.ProjectBytes}, {"AssetBytes", BuildResult.Size.AssetBytes},
							{"ShaderBytes", BuildResult.Size.ShaderBytes}, {"OtherBytes", BuildResult.Size.OtherBytes},
						};
						Result["Timings"] = std::move(Timings);
						Result["Diagnostics"] = EncodePackageDiagnostics(BuildResult.Diagnostics);
					}
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, std::move(Result)));
			}

			if (method == "StartPlaySession") {
				if (!HasOnlyFields(parameters, {"PersistenceMode", "IncludeScriptFailureLocations", "DebugRoot"}) ||
					(parameters.contains("PersistenceMode") && !parameters["PersistenceMode"].is_string()) ||
					(parameters.contains("IncludeScriptFailureLocations") && !parameters["IncludeScriptFailureLocations"].is_boolean()))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Invalid Play launch parameters"));
				const auto PersistenceMode = parameters.value("PersistenceMode", std::string("Disabled"));
				const bool NodeMode = PersistenceMode == "NodeDevelopment";
				if (PersistenceMode != "Disabled" && PersistenceMode != "Disposable" && PersistenceMode != "Persistent" && !NodeMode)
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Unknown Play persistence mode"));
				if (!StudioSecurity.HasCapability(ScriptCapability::EditorCommands) ||
					!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Play requires EditorCommands and ReadDataModel"));
				if (!World || !CurrentProject)
					return SerializeBoundedResponse(ErrorResponse(requestId, "ProjectRequired", "OpenProject must succeed first"));
				if (World->Transactions.GetOpenCount() != 0)
					return SerializeBoundedResponse(ErrorResponse(requestId, "TransactionOpen", "Commit the open authoring transaction before Play"));
				if (ReadProjectPreRunSource(CurrentProject->Root) != ActivePreRunSource)
					return SerializeBoundedResponse(ErrorResponse(requestId, "PreRunReloadRequired", "Reopen the project to activate saved PreRun before Play"));
				if (ActivePlaySession)
					return SerializeBoundedResponse(ErrorResponse(requestId, "PlaySessionActive", "One local Play session is already active"));
				if (NextPlaySessionId == 0)
					return SerializeBoundedResponse(ErrorResponse(requestId, "PlaySessionExhausted", "Play session identity is exhausted"));
				std::optional<std::string> DebugSourceId;
				if (parameters.contains("DebugRoot")) {
					const auto &Root = parameters["DebugRoot"];
					if (!Root.is_object() || !HasOnlyFields(Root, {"Object", "SourceVersion"}) ||
						!Root.contains("Object") || !Root.contains("SourceVersion") || PersistenceMode != "Disabled")
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Root debugging requires a Script and disabled persistence"));
					const auto Object = JsonCodec::DecodeObjectId(Root["Object"]);
					const auto Version = JsonCodec::DecodeUnsigned32(Root["SourceVersion"]);
					if (!Object || !Version || *Version == 0)
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Debug source identity is invalid"));
					const auto Target = std::dynamic_pointer_cast<Script>(ObjectRegistry::Get().Lookup(Object->ToObjectId()));
					if (!Target || Target->GetReplicationScopeId() != World->GetObjectId() ||
						static_cast<std::uint32_t>(Target->GetSourceVersion()) != *Version || !Target->GetEnabled())
						return SerializeBoundedResponse(ErrorResponse(requestId, "DebugSourceUnavailable", "The enabled authored Script or its source version changed"));
					DebugSourceId = Target->GetSourceId();
				}
				std::shared_ptr<IPersistenceProvider> Persistence;
				if (NodeMode) {
					if (!MatchesNodeDevelopmentProject(CurrentProject))
						return SerializeBoundedResponse(ErrorResponse(requestId, "PlayNodeDevelopmentUnavailable",
							"The opened project has no approved private Node development binding"));
					try {
						Persistence = NodeDevelopment->Admit();
					} catch (const std::exception &) {
						// Suppress native profile details and credentials at the Studio boundary.
					}
					if (!Persistence)
						return SerializeBoundedResponse(ErrorResponse(requestId, "PlayNodeDevelopmentUnavailable",
							"Private Node development persistence admission failed"));
				} else if (PersistenceMode != "Disabled") {
					try {
						if (PersistenceMode == "Disposable")
							Persistence = MakeMemoryPersistenceProvider();
						else if (SupportsDurableLocalPersistence())
							Persistence = OpenPersistentPlayStorage(*CurrentProject, true);
					} catch (const std::exception &) {
						return SerializeBoundedResponse(ErrorResponse(requestId, "PlayPersistenceUnavailable", "Local Play data could not be opened"));
					}
					if (!Persistence)
						return SerializeBoundedResponse(ErrorResponse(requestId, "PlayPersistenceUnavailable", "Requested local Play storage is unavailable"));
				}
				const auto Id = PlaySessionId{NextPlaySessionId++};
				try {
					auto Snapshot = CurrentProject->CaptureGame(World, World->GetAuthoritativeRevision());
					const auto AdmitTerrain = terrain::GetSceneTerrainAdmission(CurrentProject->Identity,
						[&Snapshot](const terrain::TerrainArtifactId &Artifact, std::size_t Maximum) {
							const auto Artifacts = Snapshot.Terrain ? std::span<const terrain::TerrainArtifact>(Snapshot.Terrain->Artifacts)
								: std::span<const terrain::TerrainArtifact>{};
							return terrain::ReadCapturedTerrainArtifact(Artifacts, Artifact, Maximum);
						});
					auto ScriptSources = (DebugSourceId || parameters.value("IncludeScriptFailureLocations", false))
						? CapturePlayScriptSources(*CurrentProject, World) : PlayScriptSources{};
					ScriptSources.DebugSourceId = DebugSourceId;
					auto NewPlaySession = std::make_unique<PlaySession>(
						Id, std::move(Snapshot.Contents), CurrentProject->InstanceFileFormat,
						CurrentProject->Root, ViewportWidth == 0 ? 720 : ViewportWidth,
						ViewportHeight == 0 ? 540 : ViewportHeight, Snapshot.Revision, std::move(Snapshot.Assets),
						AudioEnabled && !DebugSourceId, std::move(Persistence), AdmitTerrain, std::move(ScriptSources)
					);
					AdvanceViewportGeneration();
					ActivePlaySession = std::move(NewPlaySession);
					LastPlaySessionId = Id;
					LastPlaySessionState = ActivePlaySession->GetState();
					NextPlayStep = std::chrono::steady_clock::now();
					ViewportRenderer.reset();
					Json Result = {
						{"PlaySessionId", std::to_string(Id.Value)},
						{"State", GetPlaySessionStateName(LastPlaySessionState)},
						{"PersistenceMode", PersistenceMode},
						{"LaunchAuthoritativeRevision", Snapshot.Revision},
						{"ViewportGeneration", ViewportGeneration},
						{"Diagnostics", EncodePlayDiagnostics(ActivePlaySession->DrainDiagnostics())},
					};
					if (DebugSourceId) Result["DebugPause"] = EncodePlayRootDebugPause(*ActivePlaySession);
					return SerializeBoundedResponse(SuccessResponse(requestId, std::move(Result)));
				} catch (const std::exception &Error) {
					ActivePlaySession.reset();
					LastPlaySessionId = Id;
					LastPlaySessionState = PlaySessionState::Failed;
					return SerializeBoundedResponse(ErrorResponse(requestId, "PlayStartFailed", Error.what()));
				}
			}

			if (method == "GetPlayRootDebugState" || method == "ResumePlayRootDebug") {
				if (!StudioSecurity.HasCapability(ScriptCapability::EditorCommands) ||
					!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Debugger requires local owner access"));
				const bool Resume = method == "ResumePlayRootDebug";
				if (!(Resume ? HasOnlyFields(parameters, {"PlaySessionId", "PauseSerial", "Action"}) :
					HasOnlyFields(parameters, {"PlaySessionId"})) || !parameters.contains("PlaySessionId"))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Invalid debugger request"));
				const auto Id = DecodePlaySessionId(parameters["PlaySessionId"]);
				if (!Id || !ActivePlaySession || ActivePlaySession->GetId().Value != Id->Value)
					return SerializeBoundedResponse(ErrorResponse(requestId, "StalePlaySession", "Debugger requires the owned Play session"));
				if (Resume) {
					const auto Serial = parameters.contains("PauseSerial") ? JsonCodec::DecodeUnsigned32(parameters["PauseSerial"]) : std::nullopt;
					const auto Action = parameters.contains("Action") && parameters["Action"].is_string()
						? parameters["Action"].get<std::string>() : std::string{};
					if (!Serial || *Serial == 0 || (Action != "NextLine" && Action != "Continue"))
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Debugger requires an exact pause and NextLine or Continue"));
					if (!ActivePlaySession->ResumeRootDebug(*Serial, Action == "NextLine"))
						return SerializeBoundedResponse(ErrorResponse(requestId, "DebugPauseUnavailable", "Debugger pause is stale or its execution budget is spent"));
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"PlaySessionId", std::to_string(Id->Value)}, {"DebugPause", EncodePlayRootDebugPause(*ActivePlaySession)},
				}));
			}

			if (method == "InspectPlayPersistenceRecord" || method == "ResetPlayPersistenceRecord") {
				const bool Reset = method == "ResetPlayPersistenceRecord";
				if (!StudioSecurity.HasCapability(ScriptCapability::EditorCommands) ||
					!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Play storage inspection requires owner access"));
				if (!World || !CurrentProject)
					return SerializeBoundedResponse(ErrorResponse(requestId, "ProjectRequired", "OpenProject must succeed first"));
				if (ActivePlaySession || ActiveAnimationPreview)
					return SerializeBoundedResponse(ErrorResponse(requestId, "PlayPersistenceBusy", "Stop Play and Preview before inspecting local data"));
				if (!HasOnlyFields(parameters, Reset ? std::initializer_list<std::string_view>{"Kind", "Store", "Key", "Revision"} :
					std::initializer_list<std::string_view>{"Kind", "Store", "Key"}) ||
					!parameters.contains("Kind") || !parameters["Kind"].is_string() ||
					!parameters.contains("Store") || !parameters["Store"].is_string() ||
					!parameters.contains("Key") || !parameters["Key"].is_string())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Play storage record fields are invalid"));
				const auto &Kind = parameters["Kind"].get_ref<const std::string &>();
				const auto &Store = parameters["Store"].get_ref<const std::string &>();
				const auto &Key = parameters["Key"].get_ref<const std::string &>();
				if ((Kind != "KeyValue" && Kind != "Document") || !IsPersistenceStoreName(Store) || !IsPersistenceKey(Key))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Play storage record identity is invalid"));
				std::optional<std::uint64_t> Revision;
				if (Reset) {
					Revision = parameters.contains("Revision") ? DecodeNonzeroDecimal(parameters["Revision"]) : std::nullopt;
					if (!Revision)
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Reset requires the inspected revision"));
				}
				try {
					auto Provider = SupportsDurableLocalPersistence() ? OpenPersistentPlayStorage(*CurrentProject, false) : nullptr;
					const PersistenceRequest Request{
						.Kind = Kind == "KeyValue" ? PersistenceStoreKind::KeyValue : PersistenceStoreKind::Document,
						.Operation = Reset ? PersistenceOperation::Delete : PersistenceOperation::Get,
						.Store = Store,
						.Key = Key,
						.ExpectedRevision = Revision,
						.Deadline = PersistenceClock::now() + std::chrono::milliseconds(150),
					};
					PersistenceResult Result;
					if (Provider) {
						try {
							Result = Provider->Execute(Request, {});
						} catch (...) {
							Result = {.Status = Reset ? PersistenceStatus::UnknownOutcome : PersistenceStatus::Unavailable};
						}
						if (!IsPersistenceResultValid(Request, Result))
							Result = {.Status = Reset ? PersistenceStatus::UnknownOutcome : PersistenceStatus::Unavailable};
					}
					Json Reply = {{"Status", GetPersistenceStatusName(Result.Status)}};
					if (Result.Record) Reply["Record"] = EncodePlayStorageRecord(*Result.Record);
					return SerializeBoundedResponse(SuccessResponse(requestId, std::move(Reply)));
				} catch (const std::exception &) {
					return SerializeBoundedResponse(ErrorResponse(requestId, "PlayPersistenceUnavailable", "Local Play data is unavailable"));
				}
			}

			if (method == "StopPlaySession") {
				if (!HasOnlyFields(parameters, {"PlaySessionId"}) || !parameters.contains("PlaySessionId"))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "StopPlaySession requires PlaySessionId"));
				auto Id = DecodePlaySessionId(parameters["PlaySessionId"]);
				if (!Id)
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "PlaySessionId is invalid"));
				if (!ActivePlaySession || ActivePlaySession->GetId().Value != Id->Value)
					return SerializeBoundedResponse(ErrorResponse(requestId, "StalePlaySession", "PlaySessionId is not the active owned session"));
				ActivePlaySession->Stop();
				auto Diagnostics = ActivePlaySession->DrainDiagnostics();
				LastPlaySessionId = *Id;
				LastPlaySessionState = PlaySessionState::Stopped;
				ActivePlaySession.reset();
				AdvanceViewportGeneration();
				ViewportRenderer.reset();
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"PlaySessionId", std::to_string(Id->Value)}, {"State", "Stopped"},
					{"ViewportGeneration", ViewportGeneration},
					{"Diagnostics", EncodePlayDiagnostics(std::move(Diagnostics))},
				}));
			}

			if (method == "GetPlaySessionState") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GetPlaySessionState takes no parameters"));
				auto State = ActivePlaySession ? ActivePlaySession->GetState() : LastPlaySessionState;
				auto Id = ActivePlaySession ? ActivePlaySession->GetId() : LastPlaySessionId;
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"PlaySessionId", Id.Value == 0 ? Json(nullptr) : Json(std::to_string(Id.Value))},
					{"State", GetPlaySessionStateName(State)}, {"Active", ActivePlaySession != nullptr},
				}));
			}

			if (method == "ConfigureStreamingDiagnostics" || method == "GetStreamingDiagnostics") {
				const bool Configure = method == "ConfigureStreamingDiagnostics";
				if (!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel) ||
					(Configure && !StudioSecurity.HasCapability(ScriptCapability::EditorCommands)))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Streaming diagnostics require owner read access"));
				if (!(Configure ? HasOnlyFields(parameters, {"PlaySessionId", "Enabled"}) :
					HasOnlyFields(parameters, {"PlaySessionId", "StartIndex", "MaximumUnits", "MaximumTimingSamples"})) ||
					!parameters.contains("PlaySessionId"))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Streaming diagnostic fields are malformed"));
				const auto Id = DecodePlaySessionId(parameters["PlaySessionId"]);
				if (!Id || !ActivePlaySession || ActivePlaySession->GetId().Value != Id->Value)
					return SerializeBoundedResponse(ErrorResponse(requestId, "StalePlaySession", "Streaming diagnostics require the active owned Play session"));
				if (Configure) {
					if (!parameters.contains("Enabled") || !parameters["Enabled"].is_boolean())
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Enabled must be boolean"));
					ActivePlaySession->SetStreamingDiagnosticsEnabled(parameters["Enabled"].get<bool>());
				}
				const auto Size = [&](const char *Name, std::size_t Default, std::size_t Maximum) -> std::optional<std::size_t> {
					if (!parameters.contains(Name)) return Default;
					if (!parameters[Name].is_number_unsigned()) return std::nullopt;
					const auto Value = parameters[Name].get<std::uint64_t>();
					return Value <= Maximum ? std::optional<std::size_t>(static_cast<std::size_t>(Value)) : std::nullopt;
				};
				const auto Start = Size("StartIndex", 0, MaximumPackageContentUnits);
				const auto Units = Size("MaximumUnits", 64, MaximumResidencyDiagnosticUnits);
				const auto Samples = Size("MaximumTimingSamples", 32, MaximumResidencyDiagnosticSamples);
				if (!Start || !Units || !Samples)
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Streaming diagnostic query exceeds bounds"));
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"PlaySessionId", std::to_string(Id->Value)},
					{"Diagnostics", EncodeStreamingDiagnosticsSnapshot(ActivePlaySession->GetStreamingDiagnostics({*Start, *Units, *Samples}))}}));
			}

			if (method == "GetPlayInputSnapshot") {
				if (!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Input inspection requires ReadDataModel"));
				if (!HasOnlyFields(parameters, {"PlaySessionId"}) || !parameters.contains("PlaySessionId"))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GetPlayInputSnapshot requires PlaySessionId"));
				const auto Id = DecodePlaySessionId(parameters["PlaySessionId"]);
				if (!Id)
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "PlaySessionId is invalid"));
				if (!ActivePlaySession || ActivePlaySession->GetId().Value != Id->Value)
					return SerializeBoundedResponse(ErrorResponse(requestId, "StalePlaySession", "PlaySessionId is not active"));
				if (ActivePlaySession->GetState() != PlaySessionState::Running)
					return SerializeBoundedResponse(ErrorResponse(requestId, "PlaySessionNotRunning", "Input inspection requires a running session"));
				const auto RuntimeWorld = ActivePlaySession->GetWorld();
				const auto Input = ActivePlaySession->GetInputForInspection();
				if (!Input)
					return SerializeBoundedResponse(ErrorResponse(requestId, "InputInspectionUnavailable", "Runtime input service is unavailable"));
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"PlaySessionId", std::to_string(Id->Value)},
					{"Snapshot", EncodePlayInputSnapshot(RuntimeWorld, *Input)},
				}));
			}

			if (method == "PollPlayDiagnostics") {
				if (!HasOnlyFields(parameters, {"PlaySessionId"}) || !parameters.contains("PlaySessionId"))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "PollPlayDiagnostics requires PlaySessionId"));
				auto Id = DecodePlaySessionId(parameters["PlaySessionId"]);
				if (!Id || !ActivePlaySession || ActivePlaySession->GetId().Value != Id->Value)
					return SerializeBoundedResponse(ErrorResponse(requestId, "StalePlaySession", "PlaySessionId is not active"));
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"PlaySessionId", std::to_string(Id->Value)},
					{"State", GetPlaySessionStateName(ActivePlaySession->GetState())},
					{"Diagnostics", EncodePlayDiagnostics(ActivePlaySession->DrainDiagnostics())},
				}));
			}

			if (method == "SendPlayInput") {
				if (!StudioSecurity.HasCapability(ScriptCapability::ViewportControl))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Play input requires ViewportControl"));
				if (!HasOnlyFields(parameters, {"PlaySessionId", "Type", "Focused", "Physical", "Logical", "State", "Repeat", "Modifiers", "Button", "X", "Y", "DeltaX", "DeltaY", "PointerId", "Action", "Text", "SelectionStart", "SelectionLength"}) ||
					!parameters.contains("PlaySessionId") || !parameters.contains("Type") || !parameters["Type"].is_string())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "SendPlayInput fields are malformed"));
				auto Id = DecodePlaySessionId(parameters["PlaySessionId"]);
				if (!Id || !ActivePlaySession || ActivePlaySession->GetId().Value != Id->Value)
					return SerializeBoundedResponse(ErrorResponse(requestId, "StalePlaySession", "PlaySessionId is not active"));
				const auto Type = parameters["Type"].get<std::string>();
				HostEventResult InputResult;
				if (Type == "Focus") {
					if (!parameters.contains("Focused") || !parameters["Focused"].is_boolean())
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Focus input requires Focused"));
					InputResult = ActivePlaySession->ProcessEvent(FocusEvent{parameters["Focused"].get<bool>()});
				} else if (Type == "Key") {
					if (!parameters.contains("Physical") || !parameters["Physical"].is_number_unsigned() ||
						!parameters.contains("Logical") || !parameters["Logical"].is_number_unsigned() ||
						!parameters.contains("State") || !parameters["State"].is_string())
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Key input fields are malformed"));
					auto PhysicalValue = JsonCodec::DecodeUnsigned32(parameters["Physical"]);
					auto LogicalValue = JsonCodec::DecodeUnsigned32(parameters["Logical"]);
					auto ModifierValue = parameters.contains("Modifiers") ? JsonCodec::DecodeUnsigned32(parameters["Modifiers"]) : std::optional<std::uint32_t>{0};
					const auto StateName = parameters["State"].get<std::string>();
					if (!PhysicalValue || !LogicalValue || !ModifierValue || *ModifierValue > 63 ||
						!IsPlayPhysicalKey(*PhysicalValue) || !IsPlayLogicalKey(*LogicalValue) ||
						(StateName != "Pressed" && StateName != "Released"))
						return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidPlayInput", "Key input is outside the closed HostEvent vocabulary"));
					InputResult = ActivePlaySession->ProcessEvent(KeyEvent{
						{1}, static_cast<PhysicalKey>(*PhysicalValue), static_cast<LogicalKey>(*LogicalValue),
						static_cast<KeyModifier>(*ModifierValue),
						StateName == "Pressed" ? ButtonState::Pressed : ButtonState::Released,
						parameters.value("Repeat", false),
					});
				} else if (Type == "PointerMove") {
					for (const auto Field : {"X", "Y", "DeltaX", "DeltaY"})
						if (!parameters.contains(Field) || !parameters[Field].is_number() || !std::isfinite(parameters[Field].get<float>()))
							return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidPlayInput", "Pointer input must be finite"));
					InputResult = ActivePlaySession->ProcessEvent(PointerMoveEvent{
						{1}, {parameters["X"].get<float>(), parameters["Y"].get<float>()},
						{parameters["DeltaX"].get<float>(), parameters["DeltaY"].get<float>()},
					});
				} else if (Type == "PointerButton") {
					if (!parameters.contains("Button") || !parameters["Button"].is_string() ||
						!parameters.contains("State") || !parameters["State"].is_string() ||
						!parameters.contains("X") || !parameters["X"].is_number() ||
						!parameters.contains("Y") || !parameters["Y"].is_number())
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Pointer-button input fields are malformed"));
					const auto Button = magic_enum::enum_cast<PointerButton>(parameters["Button"].get<std::string>());
					const auto StateName = parameters["State"].get<std::string>();
					const auto X = parameters["X"].get<float>();
					const auto Y = parameters["Y"].get<float>();
					if (!Button || (StateName != "Pressed" && StateName != "Released") || !std::isfinite(X) || !std::isfinite(Y))
						return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidPlayInput", "Pointer-button input is outside the closed HostEvent vocabulary"));
					InputResult = ActivePlaySession->ProcessEvent(PointerButtonEvent{
						{1}, *Button,
						StateName == "Pressed" ? ButtonState::Pressed : ButtonState::Released,
						{X, Y},
					});
				} else if (Type == "Wheel") {
					for (const auto Field : {"X", "Y", "DeltaX", "DeltaY"})
						if (!parameters.contains(Field) || !parameters[Field].is_number() || !std::isfinite(parameters[Field].get<float>()))
							return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidPlayInput", "Wheel input must be finite"));
					InputResult = ActivePlaySession->ProcessEvent(WheelEvent{
						{1}, {parameters["X"].get<float>(), parameters["Y"].get<float>()},
						{parameters["DeltaX"].get<float>(), parameters["DeltaY"].get<float>()},
					});
				} else if (Type == "Touch") {
					if (!parameters.contains("PointerId") || !parameters["PointerId"].is_number_unsigned() ||
						!parameters.contains("Action") || !parameters["Action"].is_string())
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Touch input fields are malformed"));
					for (const auto Field : {"X", "Y", "DeltaX", "DeltaY"})
						if (!parameters.contains(Field) || !parameters[Field].is_number() || !std::isfinite(parameters[Field].get<float>()))
							return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidPlayInput", "Touch input must be finite"));
					const auto Pointer = JsonCodec::DecodeUnsigned32(parameters["PointerId"]);
					const auto Action = magic_enum::enum_cast<TouchPointerAction>(parameters["Action"].get<std::string>());
					if (!Pointer || *Pointer == 0 || !Action)
						return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidPlayInput", "Touch identity or action is invalid"));
					InputResult = ActivePlaySession->ProcessEvent(TouchPointerEvent{
						{*Pointer}, {parameters["X"].get<float>(), parameters["Y"].get<float>()},
						{parameters["DeltaX"].get<float>(), parameters["DeltaY"].get<float>()}, *Action,
					});
				} else if (Type == "TextInput") {
					if (!parameters.contains("Text") || !parameters["Text"].is_string())
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "TextInput requires Text"));
					auto Text = BoundedUtf8::From(parameters["Text"].get<std::string>());
					if (!Text)
						return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidPlayInput", "TextInput is invalid UTF-8 or exceeds its bound"));
					InputResult = ActivePlaySession->ProcessEvent(TextInputEvent{{1}, *Text});
				} else if (Type == "TextEditing") {
					if (!parameters.contains("Text") || !parameters["Text"].is_string() ||
						!parameters.contains("SelectionStart") || !parameters["SelectionStart"].is_number_integer() ||
						!parameters.contains("SelectionLength") || !parameters["SelectionLength"].is_number_integer())
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "TextEditing fields are malformed"));
					auto Text = BoundedCompositionUtf8::From(parameters["Text"].get<std::string>());
					const auto SelectionStart = parameters["SelectionStart"].get<std::int64_t>();
					const auto SelectionLength = parameters["SelectionLength"].get<std::int64_t>();
					if (!Text || SelectionStart < 0 || SelectionStart > std::numeric_limits<std::int32_t>::max() ||
						SelectionLength < 0 || SelectionLength > std::numeric_limits<std::int32_t>::max())
						return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidPlayInput", "TextEditing is outside the bounded composition vocabulary"));
					InputResult = ActivePlaySession->ProcessEvent(TextEditingEvent{
						{1}, *Text, static_cast<std::int32_t>(SelectionStart), static_cast<std::int32_t>(SelectionLength),
					});
				} else {
					return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidPlayInput", "Input Type is unsupported"));
				}
				Json RelativePointerMode = nullptr;
				Json TextInputState = nullptr;
				if (InputResult.Command) {
					if (const auto *Relative = std::get_if<SetRelativePointerMode>(&*InputResult.Command))
						RelativePointerMode = Relative->Enabled;
					if (const auto *TextInput = std::get_if<SetTextInputState>(&*InputResult.Command))
						TextInputState = {
							{"Active", TextInput->Active}, {"Secure", TextInput->Secure},
							{"Multiline", TextInput->Multiline}, {"AutocorrectEnabled", TextInput->AutocorrectEnabled},
							{"X", TextInput->X}, {"Y", TextInput->Y},
							{"Width", TextInput->Width}, {"Height", TextInput->Height}, {"Cursor", TextInput->Cursor},
						};
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"PlaySessionId", std::to_string(Id->Value)}, {"Accepted", true},
					{"RelativePointerMode", std::move(RelativePointerMode)}, {"TextInputState", std::move(TextInputState)},
				}));
			}

			if (method == "OpenViewportTransport") {
				if (!HasOnlyFields(parameters, {"Transport", "Version", "PixelFormat"}))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "UnsupportedViewportTransport", "Requested viewport transport contract is unsupported"
					));
				const auto Transport = parameters.value("Transport", "");
				const auto Version = parameters.value("Version", 0u);
				const auto PixelFormat = parameters.value("PixelFormat", "");
				if (Transport == "LatestFrameMailbox" && Version == LatestFrameMailboxLayout::Version &&
					PixelFormat == "BGRA8") {
					if (!LatestFrameMailbox::IsSupported())
						return SerializeBoundedResponse(ErrorResponse(
							requestId, "UnsupportedViewportTransport", "Latest-frame viewport transport is unavailable"
						));
					try {
						const bool ReplacingCompatibilityTransport = ViewportFrameRing != nullptr;
						ViewportFrameRing.reset();
						if (!ViewportLatestFrameMailbox) ViewportLatestFrameMailbox = std::make_unique<LatestFrameMailbox>();
						if (ReplacingCompatibilityTransport) AdvanceViewportGeneration();
						return SerializeBoundedResponse(SuccessResponse(requestId, {
							{"Transport", "LatestFrameMailbox"},
							{"Version", LatestFrameMailboxLayout::Version},
							{"Name", ViewportLatestFrameMailbox->GetName()},
							{"EventName", ViewportLatestFrameMailbox->GetEventName()},
							{"MappingBytes", ViewportLatestFrameMailbox->GetMappingBytes()},
							{"HeaderBytes", LatestFrameMailboxLayout::HeaderBytes},
							{"SlotCount", LatestFrameMailboxLayout::SlotCount},
							{"SlotHeaderBytes", LatestFrameMailboxLayout::SlotHeaderBytes},
							{"SlotStride", LatestFrameMailboxLayout::SlotStride},
							{"MaximumPayloadBytes", LatestFrameMailboxLayout::MaximumPayloadBytes},
							{"MaximumWidth", LatestFrameMailboxLayout::MaximumWidth},
							{"MaximumHeight", LatestFrameMailboxLayout::MaximumHeight},
							{"PixelFormat", "BGRA8"},
						}));
					} catch (const std::exception &Error) {
						ViewportLatestFrameMailbox.reset();
						return SerializeBoundedResponse(ErrorResponse(requestId, "ViewportTransportUnavailable", Error.what()));
					}
				}
				if (Transport != "SharedMemoryRing" || Version != SharedFrameRingLayout::Version || PixelFormat != "RGB8")
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "UnsupportedViewportTransport", "Requested viewport transport contract is unsupported"
					));
				if (!SharedFrameRing::IsSupported())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "UnsupportedViewportTransport", "Shared-memory viewport transport is unavailable"
					));
				try {
					const bool ReplacingLatestTransport = ViewportLatestFrameMailbox != nullptr;
					ViewportPresentationActive = false;
					ViewportLatestFrameMailbox.reset();
					if (!ViewportFrameRing) ViewportFrameRing = std::make_unique<SharedFrameRing>();
					if (ReplacingLatestTransport) AdvanceViewportGeneration();
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
				const bool HadTransport = ViewportPresentationActive || ViewportLatestFrameMailbox || ViewportFrameRing;
				ViewportPresentationActive = false;
				ViewportLatestFrameMailbox.reset();
				ViewportFrameRing.reset();
				if (HadTransport) AdvanceViewportGeneration();
				return SerializeBoundedResponse(SuccessResponse(requestId, {{"Closed", true}}));
			}

			if (method == "StartViewportPresentation") {
				if (!HasOnlyFields(parameters, {"FrameRateMode"}) || !parameters.contains("FrameRateMode") ||
					!parameters["FrameRateMode"].is_string())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "StartViewportPresentation requires FrameRateMode"
					));
				if (!ViewportLatestFrameMailbox)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "ViewportTransportRequired", "Open the latest-frame viewport transport first"
					));
				if (!World || ViewportWidth == 0 || ViewportHeight == 0)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "ViewportRequired", "Open a project and configure the viewport first"
					));
				const auto &Mode = parameters["FrameRateMode"].get_ref<const std::string &>();
				if (Mode == "Adaptive" || Mode == "144") ViewportFrameInterval = std::chrono::microseconds(6944);
				else if (Mode == "120") ViewportFrameInterval = std::chrono::microseconds(8333);
				else if (Mode == "60") ViewportFrameInterval = std::chrono::microseconds(16667);
				else if (Mode == "30") ViewportFrameInterval = std::chrono::microseconds(33333);
				else if (Mode == "Uncapped") ViewportFrameInterval = std::chrono::microseconds(1000);
				else return SerializeBoundedResponse(ErrorResponse(
					requestId, "InvalidFrameRateMode", "FrameRateMode must be Adaptive, 30, 60, 120, 144, or Uncapped"
				));
				const bool WasActive = ViewportPresentationActive;
				ViewportPresentationActive = true;
				if (!WasActive && ActivePlaySession) {
					ActivePlaySession->RequestRenderFullResync();
					LastViewportPublication.reset();
					ViewportProjection.Clear();
					ViewportRenderer.reset();
				}
				NextViewportFrame = std::chrono::steady_clock::now();
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"Active", true}, {"Generation", ViewportGeneration}, {"FrameRateMode", Mode},
					{"FrameIntervalUs", ViewportFrameInterval.count()},
				}));
			}

			if (method == "StopViewportPresentation") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "StopViewportPresentation takes no parameters"
					));
				if (ViewportPresentationActive) {
					ViewportPresentationActive = false;
					AdvanceViewportGeneration();
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"Active", false}, {"Generation", ViewportGeneration},
				}));
			}

			if (method == "GetViewportPresentationState") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "GetViewportPresentationState takes no parameters"
					));
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"Active", ViewportPresentationActive},
					{"Generation", ViewportGeneration},
					{"LatestSequence", ViewportLatestFrameMailbox ? ViewportLatestFrameMailbox->GetLatestSequence() : 0},
					{"ProducedFrames", ViewportProducedFrames},
					{"PresentationFailures", ViewportPresentationFailures},
					{"ResizeCount", ViewportResizeCount},
					{"RenderSubmissionUs", ViewportLastRenderSubmissionUs},
					{"GpuReadbackUs", ViewportLastGpuReadbackUs},
					{"CpuExtractionUs", ViewportLastCpuExtractionUs},
					{"Mode", ViewportLastMode == LatestFrameMailboxLayout::Mode::Play ? "Play" : "Edit"},
					{"PlaySessionId", ViewportLastPlaySessionId == 0
						? Json(nullptr) : Json(std::to_string(ViewportLastPlaySessionId))},
					{"CameraRevision", ViewportLastCameraRevision},
					{"SourceRevision", ViewportLastSourceRevision},
					{"LastFailure", ViewportLastFailure.empty() ? Json(nullptr) : Json(ViewportLastFailure)},
					{"MostRecentFailure", ViewportMostRecentFailure.empty()
						? Json(nullptr) : Json(ViewportMostRecentFailure)},
				}));
			}

			if (method == "OpenProject") {
				if (!HasOnlyFields(parameters, {"Root"}) || !parameters.contains("Root") ||
					!parameters["Root"].is_string())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "OpenProject requires a bounded Root"));

				const auto &EncodedRoot = parameters["Root"].get_ref<const std::string &>();
				if (EncodedRoot.empty() || EncodedRoot.size() > 32768 || EncodedRoot.find('\0') != std::string::npos)
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "OpenProject requires a bounded Root"));
				const auto Root = std::filesystem::weakly_canonical(std::filesystem::u8path(EncodedRoot));
				if (!std::filesystem::is_directory(Root))
					return SerializeBoundedResponse(ErrorResponse(requestId, "ProjectNotFound", "Project root is not a directory"));

				auto CandidateFilesystem = std::make_unique<DiskFilesystem>(Root);
				std::optional<Project> CandidateProject = Project::fromExisting(CandidateFilesystem.get());
				auto CandidatePreRun = ReadProjectPreRunSource(Root);
				const bool MatchingSchema = World && CandidatePreRun == ActivePreRunSource &&
					ActivePreRunSchemaGeneration == GetRuntimeSchemaLifecycle().GetActiveGeneration();
				auto Prepared = CandidateProject->PrepareGameLoad();
				terrain::TerrainRenderPreview Preview;
				if (const auto *Field = Prepared.GetTerrain()) {
					auto Ready = terrain::TerrainRenderPreview::Prepare(*Field);
					if (!Ready && Ready.error() == terrain::EditStatus::ReopenCapacityExceeded)
						return SerializeBoundedResponse(ErrorResponse(requestId, "TerrainReopenCapacityExceeded",
							"Editable terrain supports up to 512 stored pages and 1024 contour candidates."));
					if (!Ready)
						return SerializeBoundedResponse(ErrorResponse(requestId, "TerrainPreviewUnavailable",
							"The project exceeds the edit viewport terrain budget or could not be prepared."));
					Preview = std::move(*Ready);
				}
				struct StagedWorld {
					std::shared_ptr<DataModel> Value;
					void Retire() {
						if (!Value) return;
						ScopedChangeJournalSuppression Suppression;
						Value->Destroy();
						Value.reset();
					}
					~StagedWorld() {
						try {
							Retire();
						} catch (...) {}
					}
				} CandidateWorld;
				std::string Response;
				ObjectId TerrainOwner;
				const auto LoadDocument = [&] {
					ScopedChangeJournalSuppression Suppression;
					CandidateWorld.Value = CandidateProject->DeserializeGame(std::move(Prepared));
					auto &Document = CandidateWorld.Value;
					Document->InitializeLoadedProjectRevision();
					const auto Space = std::dynamic_pointer_cast<Workspace>(Document->GetService("Workspace"));
					if (!Space || !Space->GetTerrain()) {
						Response = SerializeBoundedResponse(ErrorResponse(requestId, "InvalidProject", "Project has no valid Workspace"));
						return false;
					}
					TerrainOwner = Space->GetTerrain()->GetObjectId();
					auto Encoded = JsonCodec::Encode(SuccessResponse(requestId, {
						{"Root", JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(Document->GetObjectId()))},
						{"ProjectState", EncodeActiveProjectState(Document, Document->GetAuthoritativeRevision(), CandidateProject)},
					}), "EditorHost OpenProject response");
					if (!Encoded || Encoded->size() > EditorHostMaximumResponseBytes) {
						Response = SerializeBoundedResponse(ErrorResponse(requestId, "ResponseTooLarge",
							"EditorHost response exceeded its byte limit"));
						return false;
					}
					Response = std::move(*Encoded);
					return true;
				};

				if (MatchingSchema && !LoadDocument()) return Response;

				ResetEditorViewportDocument();
				if (World) {
					(void)World->Transactions.TerminateOwner(*World, TransactionOwner);
					World->Destroy();
				}
				ActiveAnimationPreview.reset();
				World.reset();
				Filesystem.reset();
				CurrentProject.reset();
				PersistedRevision = 0;
				ViewportCamera.reset();
				LastViewportPublication.reset();
				ViewportProjection.Clear();
				ViewportPublisher->RequestFullResync();
				ResetAuthoringCursors();
				ViewportWidth = 0;
				ViewportHeight = 0;
				ViewportPresentationActive = false;
				AdvanceViewportGeneration();
				if (!MatchingSchema) {
					ActivePreRunSource = BootstrapProjectRuntimeSchema(Root);
					ActivePreRunSchemaGeneration = GetRuntimeSchemaLifecycle().GetActiveGeneration();
					if (!LoadDocument()) return Response;
				}
				Filesystem = std::move(CandidateFilesystem);
				CurrentProject = std::move(CandidateProject);
				World = std::move(CandidateWorld.Value);
				World->Filesystem = Filesystem.get();
				ViewportPublisher->SetTerrainRenderState(std::move(Preview).Bind(TerrainOwner));
				PersistedRevision = World->GetAuthoritativeRevision();
				ViewportCamera = RenderCameraInput{};
				return Response;
			}

			if (method == "CreateProject") {
				if (!StudioSecurity.HasCapability(ScriptCapability::EditorCommands))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Unauthorized", "Project creation requires EditorCommands"
					));
				if (!HasOnlyFields(parameters, {"Destination", "Name", "TerrainSampleSpacing"}) ||
					!parameters.contains("Destination") || !parameters.contains("Name") ||
					!parameters["Name"].is_string())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "CreateProject requires Destination and Name"
					));
				terrain::GridSpec Grid;
				if (parameters.contains("TerrainSampleSpacing")) {
					const auto &Value = parameters["TerrainSampleSpacing"];
					const auto Spacing = Value.is_number() ? Value.get<double>() : 0.0;
					if (Spacing != 0.5 && Spacing != 1.0 && Spacing != 2.0)
						return SerializeBoundedResponse(ErrorResponse(
							requestId, "InvalidTerrainSpacing", "Terrain sample spacing must be 0.5, 1 or 2 world units"
						));
					Grid.SpacingShift = Spacing == 0.5 ? -1 : (Spacing == 2.0 ? 1 : 0);
				}
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
					const auto Space = std::dynamic_pointer_cast<Workspace>(NewWorld->GetService("Workspace"));
					if (!Space)
						throw std::runtime_error("The canonical new project could not create Workspace");
					if (Grid != terrain::GridSpec{})
						Space->RestoreTerrainField(std::make_unique<terrain::Field>(Grid));
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
					ResetEditorViewportDocument();
					if (World) {
						(void)World->Transactions.TerminateOwner(*World, TransactionOwner);
						World->Destroy();
					}
					ActiveAnimationPreview.reset();
					World.reset();
					Filesystem.reset();
					CurrentProject.reset();
					PersistedRevision = 0;
					ViewportCamera.reset();
					LastViewportPublication.reset();
					ViewportProjection.Clear();
					ViewportPublisher->RequestFullResync();
					ResetAuthoringCursors();
					ViewportWidth = 0;
					ViewportHeight = 0;
					ViewportPresentationActive = false;
					AdvanceViewportGeneration();
					ActivePreRunSource = BootstrapProjectRuntimeSchema(*Destination);
					ActivePreRunSchemaGeneration = GetRuntimeSchemaLifecycle().GetActiveGeneration();
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
					ResetViewportSpatial();
					if (World) {
						try { World->Destroy(); }
						catch (...) {}
					}
					World.reset();
					Filesystem.reset();
					CurrentProject.reset();
					PersistedRevision = 0;
					ViewportCamera.reset();
					ResetAuthoringCursors();
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "ProjectActivationFailure", "The persisted new project could not be activated"
					));
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"Root", JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(World->GetObjectId()))},
					{"ProjectState", EncodeActiveProjectState(World, PersistedRevision, CurrentProject)},
				}));
			}

			if (method == "GetProjectState") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GetProjectState takes no parameters"));
				if (!World || !CurrentProject)
					return SerializeBoundedResponse(ErrorResponse(requestId, "NoProjectLoaded", "No project is loaded"));
				return SerializeBoundedResponse(SuccessResponse(
					requestId, EncodeActiveProjectState(World, PersistedRevision, CurrentProject)
				));
			}

			if (TeamAuthor != 0 && method == "GetTeamHistory") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GetTeamHistory takes no parameters"));
				if (!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "History access requires ReadDataModel"));
				if (!World)
					return SerializeBoundedResponse(ErrorResponse(requestId, "NoProjectLoaded", "No project is loaded"));
				const auto &Transactions = World->Transactions;
				Json Entries = Json::array();
				for (const auto &Action : Transactions.GetCommitted()) {
					Entries.push_back({
						{"TransactionId", std::to_string(Action->Id.Value)}, {"Author", Action->Author},
						{"Label", Action->Label}, {"StartingRevision", Action->StartingRevision},
						{"ResultingRevision", Action->ResultingRevision}, {"ChangeCount", Action->Changes.size()}
					});
				}
				const auto *Undo = Transactions.GetUndoTransaction();
				const auto *Redo = Transactions.GetRedoTransaction();
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"Entries", std::move(Entries)}, {"CanUndo", Undo && Undo->Author == TeamAuthor},
					{"CanRedo", Redo && Redo->Author == TeamAuthor}
				}));
			}

			if (method == "GetCharacterComposition") {
				if (!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Character inspection requires ReadDataModel"));
				if (!World || !CurrentProject)
					return SerializeBoundedResponse(ErrorResponse(requestId, "NoProjectLoaded", "No project is loaded"));
				if (!Cursor)
					return SerializeBoundedResponse(ErrorResponse(requestId, "SnapshotRequired", "GetSnapshot must establish the project scope"));
				if (!HasOnlyFields(parameters, {"CharacterCompositionVersion", "Scope", "Object"}) ||
					!parameters.contains("CharacterCompositionVersion") || !parameters.contains("Scope") || !parameters.contains("Object"))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Character inspection requires version, Scope and Object"));
				const auto Version = JsonCodec::DecodeUnsigned32(parameters["CharacterCompositionVersion"]);
				const auto Scope = JsonCodec::DecodeObjectId(parameters["Scope"]);
				const auto Object = JsonCodec::DecodeObjectId(parameters["Object"]);
				if (!Version || !Scope || !Object)
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Character inspection identities/version are malformed"));
				if (*Version != 1)
					return SerializeBoundedResponse(ErrorResponse(requestId, "UnsupportedCapabilityVersion", "Unsupported CharacterComposition version"));
				if (Scope->ToObjectId() != World->GetObjectId())
					return SerializeBoundedResponse(ErrorResponse(requestId, "StaleProject", "Character inspection belongs to another project scope"));
				const auto Target = ObjectRegistry::Get().Lookup(Object->ToObjectId());
				if (!Target || Target->GetDestroyed() || Target->IsDestroying() ||
					Target->GetReplicationScopeId() != World->GetObjectId())
					return SerializeBoundedResponse(ErrorResponse(requestId, "StaleObject", "Character inspection target is not live in this project"));
				const auto CharacterValue = std::dynamic_pointer_cast<Character>(Target);
				if (!CharacterValue)
					return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidCharacter", "Select a Character or KinematicCharacter"));
				const auto ExistingAssets = World->FindService("AssetService");
				const auto Assets = ExistingAssets ? std::dynamic_pointer_cast<AssetService>(*ExistingAssets) : nullptr;
				auto Result = EncodeCharacterComposition(InspectCharacterComposition(CharacterValue, Assets));
				Result["CharacterCompositionVersion"] = 1;
				Result["Context"] = "Authoring";
				Result["Scope"] = JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(World->GetObjectId()));
				Result["AuthoritativeRevision"] = World->GetAuthoritativeRevision();
				return SerializeBoundedResponse(SuccessResponse(requestId, std::move(Result)));
			}

			if (method == "GetAnimationRig" || method == "OpenAnimationPreview" ||
				method == "ControlAnimationPreview" || method == "CaptureAnimationPreview" || method == "CloseAnimationPreview") {
				if (!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel) ||
					(method != "GetAnimationRig" && !StudioSecurity.HasCapability(ScriptCapability::ViewportControl)))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Animation access requires ReadDataModel and preview requires ViewportControl"));
				if (!World || !CurrentProject)
					return SerializeBoundedResponse(ErrorResponse(requestId, "NoProjectLoaded", "No project is loaded"));
				if (method == "GetAnimationRig" || method == "OpenAnimationPreview") {
					if (!HasOnlyFields(parameters, {"Reference"}) || !parameters.contains("Reference") || !parameters["Reference"].is_string())
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "A strict asset Reference is required"));
					auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
					auto Rig = DescribeAnimationRig(Assets, parameters["Reference"].get<std::string>());
					if (method == "GetAnimationRig") return SerializeBoundedResponse(SuccessResponse(requestId, Rig));
					if (ActiveAnimationPreview)
						return SerializeBoundedResponse(ErrorResponse(requestId, "PreviewActive", "Close the current Animation Preview first"));
					if (NextAnimationPreviewId == 0) throw std::overflow_error("Animation Preview identity exhausted");
					auto Candidate = std::make_unique<AnimationPreviewJob>();
					Candidate->Id = NextAnimationPreviewId++;
					Candidate->Scene = std::make_unique<AnimationPreview>(Assets->CaptureProjectAssets(), Rig["MeshReference"].get<std::string>());
					Candidate->Rig = Rig;
					Json Transport = {{"Transport", "Base64"}};
					if (SharedFrameRing::IsSupported()) {
						Candidate->Ring = std::make_unique<SharedFrameRing>();
						Transport = {{"Transport", "SharedMemoryRing"}, {"Version", SharedFrameRingLayout::Version},
							{"Name", Candidate->Ring->GetName()}, {"MappingBytes", Candidate->Ring->GetMappingBytes()},
							{"HeaderBytes", SharedFrameRingLayout::HeaderBytes}, {"SlotCount", SharedFrameRingLayout::SlotCount},
							{"SlotHeaderBytes", SharedFrameRingLayout::SlotHeaderBytes}, {"SlotStride", SharedFrameRingLayout::SlotStride},
							{"MaximumPayloadBytes", SharedFrameRingLayout::MaximumPayloadBytes}, {"PixelFormat", "RGB8"}};
					}
					Json Result = {{"PreviewId", std::to_string(Candidate->Id)}, {"Rig", std::move(Rig)}, {"FrameTransport", std::move(Transport)}};
					ActiveAnimationPreview = std::move(Candidate);
					return SerializeBoundedResponse(SuccessResponse(requestId, Result));
				}
				auto Id = parameters.contains("PreviewId") ? DecodeCanonicalJobId(parameters["PreviewId"]) : std::nullopt;
				if (!Id || !ActiveAnimationPreview || ActiveAnimationPreview->Id != *Id)
					return SerializeBoundedResponse(ErrorResponse(requestId, "StalePreview", "PreviewId is not the active owned preview"));
				auto &Preview = *ActiveAnimationPreview;
				if (method == "CloseAnimationPreview") {
					if (!HasOnlyFields(parameters, {"PreviewId"}))
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Close accepts only PreviewId"));
					ActiveAnimationPreview.reset();
					return SerializeBoundedResponse(SuccessResponse(requestId, {{"Closed", true}}));
				}
				if (method == "ControlAnimationPreview") {
					if (!HasOnlyFields(parameters, {"PreviewId", "Action", "Value", "Slot", "BlendVersion"}) ||
						!parameters.contains("Action") || !parameters["Action"].is_string())
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Control requires Action and optional Value"));
					const auto Action = parameters["Action"].get<std::string>();
					const bool BlendAction = parameters.contains("Slot") || Action == "BlendWeight" || Action == "ClearSecondaryClip";
					if (BlendAction || parameters.contains("BlendVersion")) {
						if (!parameters.contains("BlendVersion") || JsonCodec::DecodeUnsigned32(parameters["BlendVersion"]) != 1)
							return SerializeBoundedResponse(ErrorResponse(requestId, "UnsupportedCapabilityVersion", "Blend controls require BlendVersion 1"));
					}
					auto Slot = AnimationPreviewClipSlot::Primary;
					if (parameters.contains("Slot")) {
						if (!parameters["Slot"].is_string() ||
							(parameters["Slot"] != "Primary" && parameters["Slot"] != "Secondary"))
							return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Slot must be Primary or Secondary"));
						if (parameters["Slot"] == "Secondary") Slot = AnimationPreviewClipSlot::Secondary;
					}
					auto Track = Preview.Scene->GetTrack(Slot);
					if (Action == "Clip" && parameters.contains("Value") && parameters["Value"].is_string())
						Preview.Scene->SelectClip(Slot, parameters["Value"].get<std::string>());
					else if (Action == "ClearSecondaryClip" && !parameters.contains("Value") && !parameters.contains("Slot"))
						Preview.Scene->ClearSecondaryClip();
					else if (Action == "BlendWeight" && parameters.contains("Value") &&
						parameters["Value"].is_number() && !parameters.contains("Slot")) {
						const auto Value = parameters["Value"].get<float>();
						if (!std::isfinite(Value) || Value < 0.0f || Value > 1.0f)
							return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidPlayback", "Blend weight must be finite and within 0..1"));
						if (!Preview.Scene->GetTrack(AnimationPreviewClipSlot::Primary) ||
							!Preview.Scene->GetTrack(AnimationPreviewClipSlot::Secondary))
							return SerializeBoundedResponse(ErrorResponse(requestId, "ClipRequired", "Select both compatible clips before blending"));
						Preview.Scene->SetBlendWeight(Value);
					}
					else if (!Track) return SerializeBoundedResponse(ErrorResponse(requestId, "ClipRequired", "Select a clip first"));
					else if (Action == "Play" && !parameters.contains("Value")) {
						if (Track->GetPlaybackState() == Enums::AnimationPlaybackState::Paused) Track->Resume();
						else Track->Play();
					}
					else if (Action == "Pause" && !parameters.contains("Value")) Track->Pause();
					else if ((Action == "Stop" || Action == "Reset") && !parameters.contains("Value")) Track->Stop();
					else if (Action == "Loop" && parameters.contains("Value") && parameters["Value"].is_boolean()) Track->SetLooped(parameters["Value"].get<bool>());
					else if ((Action == "Seek" || Action == "Speed") && parameters.contains("Value") && parameters["Value"].is_number()) {
						const auto Value = parameters["Value"].get<float>();
						if (!std::isfinite(Value) || Value < 0 || (Action == "Speed" && Value > AnimationTrack::MaximumSpeed))
							return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidPlayback", "Playback value is out of bounds"));
						if (Action == "Seek") {
							if (Track->GetPlaybackState() == Enums::AnimationPlaybackState::Stopped) { Track->Play(); Track->Pause(); }
							Track->SetTimePosition(Value);
						} else Track->SetSpeed(Value);
					} else return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Invalid animation control action/value"));
					Preview.Scene->Step(0.0f);
					Preview.LastStep = std::chrono::steady_clock::now();
					Json Result = {{"PreviewId", std::to_string(*Id)}};
					if (parameters.contains("BlendVersion")) EncodePreviewBlendState(Result, *Preview.Scene);
					return SerializeBoundedResponse(SuccessResponse(requestId, std::move(Result)));
				}
				if (!HasOnlyFields(parameters, {"PreviewId", "Width", "Height", "Position", "Target"}) ||
					!parameters.contains("Width") || !parameters.contains("Height") ||
					!parameters["Width"].is_number_unsigned() || !parameters["Height"].is_number_unsigned() ||
					!parameters.contains("Position") || !parameters.contains("Target"))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Capture requires bounded dimensions and camera Position/Target"));
				const auto Width = JsonCodec::DecodeUnsigned32(parameters["Width"]), Height = JsonCodec::DecodeUnsigned32(parameters["Height"]);
				const auto Position = DecodeVector3(parameters["Position"]), Target = DecodeVector3(parameters["Target"]);
				if (!Width || !Height || *Width == 0 || *Height == 0 || *Width > 1024 || *Height > 1024 ||
					!Position || !Target || glm::length(*Position - *Target) < 0.001f)
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Invalid preview camera or dimensions"));
				if (glm::length(glm::cross(glm::normalize(*Target - *Position), glm::vec3(0, 1, 0))) < 0.001f)
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Preview camera direction is parallel to its up axis"));
				const auto Now = std::chrono::steady_clock::now();
				Preview.Scene->Step(std::min(60.0f, std::chrono::duration<float>(Now - Preview.LastStep).count()));
				Preview.LastStep = Now;
				if (!Preview.Renderer) Preview.Renderer = std::make_unique<EditorViewportRenderer>(*Width, *Height);
				else Preview.Renderer->Resize(*Width, *Height);
				auto Publication = Preview.Scene->Publish(MakeLookAtRenderCameraInput(*Position, *Target, {0, 1, 0}), *Width, *Height);
				auto Frame = Preview.Renderer->Capture(Publication);
				Json Result = {{"PreviewId", std::to_string(*Id)}, {"Width", *Width}, {"Height", *Height}, {"Format", "RGB8"}};
				if (Preview.Ring) {
					Result["FrameNumber"] = Preview.Ring->Publish(*Width, *Height, Frame.RgbPixels,
						static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Now.time_since_epoch()).count()));
				} else Result["Pixels"] = EncodeBase64(Frame.RgbPixels);
				if (auto Track = Preview.Scene->GetTrack()) {
					Result["TimePosition"] = Track->GetTimePosition();
					Result["PlaybackState"] = magic_enum::enum_name(Track->GetPlaybackState());
				}
				Json Positions = Json::array();
				if (auto Pose = Preview.Scene->GetPose(); Pose && Pose->JointModelTransforms)
					for (const auto &Transform : *Pose->JointModelTransforms) Positions.push_back({Transform[3].x, Transform[3].y, Transform[3].z});
				Result["JointPositions"] = std::move(Positions);
				EncodePreviewBlendState(Result, *Preview.Scene);
				return SerializeBoundedResponse(SuccessResponse(requestId, Result));
			}

			if (method == "GetAssetCatalog") {
				if (!HasOnlyFields(parameters, {"IncludeBuiltIns", "IncludeAdvancedMaterials"}) ||
					(parameters.contains("IncludeBuiltIns") && !parameters["IncludeBuiltIns"].is_boolean()) ||
					(parameters.contains("IncludeAdvancedMaterials") && !parameters["IncludeAdvancedMaterials"].is_boolean()))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "GetAssetCatalog accepts optional IncludeBuiltIns and IncludeAdvancedMaterials booleans"
					));
				if (!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Unauthorized", "Asset catalog access requires ReadDataModel"
					));
				if (!World || !CurrentProject)
					return SerializeBoundedResponse(ErrorResponse(requestId, "NoProjectLoaded", "No project is loaded"));
				auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
				if (!Assets)
					return SerializeBoundedResponse(ErrorResponse(requestId, "AssetServiceFailure", "AssetService is unavailable"));
				Json Records = Json::array();
				const bool IncludeAdvancedMaterials = parameters.value("IncludeAdvancedMaterials", false);
				for (const auto &Record : Assets->GetCatalog(parameters.value("IncludeBuiltIns", true)))
					Records.push_back(EncodeAssetRecord(Record, IncludeAdvancedMaterials));
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"CatalogVersion", 2}, {"Assets", std::move(Records)},
					{"ProjectState", EncodeProjectState(World, PersistedRevision, CurrentProject)},
				}));
			}

			if (method == "ImportAsset" || method == "ReimportAsset" || method == "DeleteAsset") {
				if (!StudioSecurity.HasCapability(ScriptCapability::EditorCommands))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Unauthorized", "Asset authoring requires EditorCommands"
					));
				if (!World || !CurrentProject || !Filesystem)
					return SerializeBoundedResponse(ErrorResponse(requestId, "NoProjectLoaded", "No project is loaded"));
				if (World->Transactions.GetOpenCount() != 0)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "TransactionOpen", "Commit the open transaction before changing the asset catalog"
					));
				if (HasRevisionConflict())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Conflict", "The authoritative project revision changed before asset authoring"
					));

				auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
				if (!Assets)
					return SerializeBoundedResponse(ErrorResponse(requestId, "AssetServiceFailure", "AssetService is unavailable"));
				AssetOperationResult Result;
				if (method == "ImportAsset") {
					if (!HasOnlyFields(parameters, {"Source", "Kind", "Name", "ExpectedRevision"}) ||
						!parameters.contains("Source") || !parameters["Source"].is_string() ||
						(parameters.contains("Kind") && !parameters["Kind"].is_string()) ||
						(parameters.contains("Name") && !parameters["Name"].is_string()))
						return SerializeBoundedResponse(ErrorResponse(
							requestId, "MalformedRequest", "ImportAsset requires Source and accepts Kind, Name, and ExpectedRevision"
						));
					std::optional<AssetKind> Kind;
					if (parameters.contains("Kind")) {
						Kind = ParseAssetKind(parameters["Kind"].get_ref<const std::string &>());
						if (!Kind)
							return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidAssetKind", "Asset kind is unsupported"));
					}
					SourceMount Mount(*Filesystem);
					Result = Assets->ImportProjectAsset(
						Mount, parameters["Source"].get<std::string>(), Kind, parameters.value("Name", std::string{})
					);
				} else {
					if (!HasOnlyFields(parameters, {"Reference", "ExpectedRevision"}) ||
						!parameters.contains("Reference") || !parameters["Reference"].is_string())
						return SerializeBoundedResponse(ErrorResponse(
							requestId, "MalformedRequest", "Asset mutation requires Reference and accepts ExpectedRevision"
						));
					const auto &Reference = parameters["Reference"].get_ref<const std::string &>();
					if (!AssetReference::Parse(Reference))
						return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidAssetReference", "Asset reference is not canonical"));
					if (method == "ReimportAsset") {
						SourceMount Mount(*Filesystem);
						Result = Assets->ReimportProjectAsset(Mount, Reference);
					} else Result = Assets->DeleteProjectAsset(Reference);
				}
				Json Diagnostic = nullptr;
				if (!Result.Ok) Diagnostic = {
					{"Code", Result.Diagnostic.Code.empty() ? "AssetOperationFailed" : Result.Diagnostic.Code},
					{"Message", Result.Diagnostic.Message.empty() ? "Asset operation failed" : Result.Diagnostic.Message},
				};
				Json Records = Json::array();
				for (const auto &Record : Result.Records) Records.push_back(EncodeAssetRecord(Record));
				if (Records.empty() && Result.Record) Records.push_back(EncodeAssetRecord(*Result.Record));
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"OperationSucceeded", Result.Ok},
					{"Asset", Result.Record ? Json(EncodeAssetRecord(*Result.Record)) : Json(nullptr)},
					{"Assets", std::move(Records)},
					{"Diagnostic", std::move(Diagnostic)},
					{"ProjectState", EncodeProjectState(World, PersistedRevision, CurrentProject)},
				}));
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
				if (!HasOnlyFields(parameters, {"ExpectedRevision"}))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "MalformedRequest", "Undo and Redo accept only ExpectedRevision"
					));
				if (!StudioSecurity.HasCapability(ScriptCapability::MutateDataModel))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Unauthorized", "History execution requires MutateDataModel"
					));
				if (!World || !CurrentProject)
					return SerializeBoundedResponse(ErrorResponse(requestId, "NoProjectLoaded", "No project is loaded"));
				if (!RequestCursor)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "SnapshotRequired", "GetSnapshot must establish a journal cursor"
					));
				if (HasRevisionConflict())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Conflict", "The authoritative project revision changed before history execution"
					));
				if (TeamAuthor != 0) {
					const auto *Action = method == "Undo" ? World->Transactions.GetUndoTransaction()
						: World->Transactions.GetRedoTransaction();
					if (Action && Action->Author != TeamAuthor)
						return SerializeBoundedResponse(ErrorResponse(
							requestId, "HistoryConflict", "The next history action belongs to another author"
						));
				}
				auto Result = method == "Undo" ? Mutations.Undo(*World, StudioSecurity) : Mutations.Redo(*World, StudioSecurity);
				if (!Result.Succeeded()) {
					const char *Code = Result.Status == TransactionStatus::NothingToUndo ? "NothingToUndo" :
						Result.Status == TransactionStatus::NothingToRedo ? "NothingToRedo" :
						Result.Status == TransactionStatus::RevisionExhausted ? "RevisionExhausted" :
						Result.Status == TransactionStatus::InvalidState ? "TransactionOpen" : "HistoryExecutionFailed";
					return SerializeBoundedResponse(ErrorResponse(requestId, Code, Result.Message));
				}
				auto ProjectState = EncodeProjectState(World, PersistedRevision, CurrentProject, TeamAuthor);
				const auto &History = ProjectState["History"];
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"TransactionId", std::to_string(Result.Id.Value)},
					{"Action", method}, {"ResultingRevision", Result.ResultingRevision},
					{"CanUndo", History["CanUndo"]}, {"CanRedo", History["CanRedo"]},
					{"UndoLabel", History["UndoLabel"]}, {"RedoLabel", History["RedoLabel"]},
					{"ProjectState", ProjectState},
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
				if ((!saveAs && !HasOnlyFields(parameters, {"ExpectedRevision"})) ||
					(saveAs && (!HasOnlyFields(parameters, {"Destination", "ExpectedRevision"}) ||
						!parameters.contains("Destination"))))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Project persistence parameters are invalid"));
				if (HasRevisionConflict())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Conflict", "The authoritative project revision changed before persistence"
					));

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
					destinationProject->Identity = CurrentProject->Identity;
				}
				auto &project = saveAs ? *destinationProject : *CurrentProject;
				Project::PersistenceSnapshot snapshot;
				try {
					snapshot = project.CaptureGame(World, World->GetAuthoritativeRevision(), !saveAs);
				} catch (const std::exception &) {
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "SerializationFailure", "The authoritative project could not be serialized"
					));
				}
				try {
					const bool CopyProject = saveAs && project.Root != CurrentProject->Root;
					const auto PreRun = CopyProject ? ReadProjectPreRunSource(CurrentProject->Root) : std::nullopt;
					const auto Persist = [&](const Project &Candidate) {
						Candidate.ValidatePersistenceDestination();
						if (CopyProject) {
							const auto ExistingPreRun = ReadProjectPreRunSource(Candidate.Root);
							if (ExistingPreRun != PreRun) {
								// An already saved scene must keep the schema with which it was written.
								if (!PreRun || std::filesystem::exists(Candidate.InstanceFilePath) ||
									!Candidate.ReplacePreRunSource(ExistingPreRun, *PreRun))
									throw PreRunSourceConflict("PreRun destination conflicts with the project being saved");
							}
						}
						Candidate.PersistGameAtomically(snapshot, PersistenceCheckpointForTesting);
					};
					if (CopyProject && !std::filesystem::exists(project.Root)) {
						// Publish a new destination only after the complete project closure is present.
						std::filesystem::create_directories(project.Root.parent_path());
						const auto Staging = CreateOwnedStagingDirectory(project.Root);
						try {
							DiskFilesystem StagingFilesystem(Staging);
							auto Candidate = Project::forDestination(&StagingFilesystem, project.InstanceFileFormat);
							Candidate.Identity = project.Identity;
							Persist(Candidate);
							if (std::filesystem::exists(project.Root))
								throw std::runtime_error("Save As destination appeared during persistence");
							std::filesystem::rename(Staging, project.Root);
						} catch (...) {
							std::error_code Ignored;
							std::filesystem::remove_all(Staging, Ignored);
							throw;
						}
					} else {
						Persist(project);
					}
					PersistedRevision = snapshot.Revision;
					if (saveAs) {
						Filesystem = std::move(destinationFilesystem);
						CurrentProject = std::move(destinationProject);
						World->Root = CurrentProject->Root;
						World->Filesystem = Filesystem.get();
					}
					Json result = EncodeActiveProjectState(World, PersistedRevision, CurrentProject);
					result["PersistedRevision"] = snapshot.Revision;
					return SerializeBoundedResponse(SuccessResponse(requestId, std::move(result)));
				} catch (const std::filesystem::filesystem_error &) {
					return SerializeBoundedResponse(ErrorResponse(requestId, "FilesystemFailure", "The project destination could not be written atomically"));
				} catch (const std::exception &) {
					return SerializeBoundedResponse(ErrorResponse(requestId, "PersistenceFailure", "The project could not be persisted atomically"));
				}
			}

			if (method == "GetSchema") {
				if (!HasOnlyFields(parameters, {"SchemaDiscoveryVersion"}))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Invalid GetSchema fields"));
				const auto DiscoveryVersion = parameters.contains("SchemaDiscoveryVersion")
					? JsonCodec::DecodeUnsigned32(parameters["SchemaDiscoveryVersion"]) : std::optional<std::uint32_t>(6);
				if (!DiscoveryVersion || (*DiscoveryVersion != 6 && *DiscoveryVersion != 7))
					return SerializeBoundedResponse(ErrorResponse(requestId, "UnsupportedCapabilityVersion", "Supported schema discovery versions are 6 and 7"));
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
							{"AtomicBatchWritable", AtomicBatchWritable(*property, StudioSecurity)},
							{"Persistence", property->PersistencePolicy == InstanceProperty::Persistence::Saved ? "Saved" : "Transient"},
							{"Replication", property->ReplicationPolicy == InstanceProperty::Replication::FutureReplicated ? "Replicated" : "None"},
							{"Authority", property->WriteAuthority == InstanceProperty::Authority::Main ? "Main" : "Any"},
							{"HasValidator", static_cast<bool>(property->Validate) || property->Range.has_value()},
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
						if (classDefinition->ConstructionKind == SchemaClassConstructionKind::Native) {
							std::vector<std::string> Names;
							Names.reserve(classDefinition->Properties.size());
							for (const auto &[Name, Property] : classDefinition->Properties) {
								(void)Property;
								Names.push_back(Name);
							}
							std::ranges::sort(Names);
							for (const auto &Name : Names)
								properties.push_back(EncodeNativePropertyMetadata(
									*classDefinition, classDefinition->Properties.at(Name), StudioSecurity
								));
						} else {
							for (const auto &property : classDefinition->DeclaredCustomProperties)
								properties.push_back(EncodeSchemaScalarPropertyMetadata(
									property.Name, property.CanonicalName, property.Type,
									property.DefaultValue, property.Editable
								));
						}
						encoded["Properties"] = std::move(properties);
						if (*DiscoveryVersion >= 7 && !classDefinition->DefaultOverrides.empty()) {
							Json Overrides = Json::array();
							for (const auto &Override : classDefinition->DefaultOverrides) {
								const auto &Property = *classDefinition->AllProperties.at(Override.Property);
								const auto *Declaring = GetActiveRuntimeSchemaRegistry().FindClassById(Override.DeclaringClassSchemaId);
								Overrides.push_back({
									{"DeclaringClassSchemaId", Override.DeclaringClassSchemaId.ToString()},
									{"DeclaringDefinitionVersion", Override.DeclaringDefinitionVersion},
									{"Property", Override.Property},
									{"CanonicalName", Declaring->CanonicalName + "." + Override.Property},
									{"Default", JsonCodec::EncodeWireValue(*EncodePropertyDefault(Property, Override.Value))}});
							}
							encoded["DefaultOverrides"] = std::move(Overrides);
						}
					} else if (const auto *enumDefinition = std::get_if<SchemaEnumDefinition>(entry)) {
						Json items = Json::array();
						for (const auto &item : enumDefinition->Items)
							items.push_back({{"Name", item.Name}, {"Value", item.Value}});
						encoded["Items"] = std::move(items);
					} else if (const auto *extension = std::get_if<SchemaExtensionDefinition>(entry)) {
						encoded["TargetClassSchemaId"] = extension->TargetClassId.ToString();
						Json properties = Json::array();
						for (const auto &property : extension->Properties)
							properties.push_back(EncodeSchemaScalarPropertyMetadata(
								property.Name, property.CanonicalName, property.Type,
								property.DefaultValue, property.Editable
							));
						encoded["Properties"] = std::move(properties);
					}
					definitions.push_back(std::move(encoded));
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"SchemaDiscoveryVersion", *DiscoveryVersion},
					{"RegistryGeneration", GetRuntimeSchemaLifecycle().GetActiveGeneration()},
					{"Definitions", std::move(definitions)},
					{"Classes", std::move(classes)},
				}));
			}

			if ((method == "GetScriptSource" || method == "GetScriptBinding" || method == "ResolveScriptBinding") &&
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
			if (method == "SetPropertyBatch") {
				auto Fail = [&](const char *Code, const char *Message) {
					return SerializeBoundedResponse(ErrorResponse(requestId, Code, Message));
				};
				if (!StudioSecurity.HasCapability(ScriptCapability::MutateDataModel))
					return Fail("Unauthorized", "SetPropertyBatch requires MutateDataModel");
				if (!World || !CurrentProject) return Fail("ProjectRequired", "OpenProject must succeed first");
				if (!HasOnlyFields(parameters, {"PropertyBatchVersion", "Scope", "ExpectedRevision", "Writes"}) ||
					!parameters.contains("PropertyBatchVersion") || !parameters.contains("Scope") ||
					!ExpectedRevision || !parameters.contains("Writes") || !parameters["Writes"].is_array())
					return Fail("MalformedRequest", "SetPropertyBatch requires version, scope, revision, and writes");
				const auto Version = JsonCodec::DecodeUnsigned32(parameters["PropertyBatchVersion"]);
				if (!Version || *Version != EditorHostPropertyBatchVersion)
					return Fail("UnsupportedCapabilityVersion", "PropertyBatchVersion is not supported");
				const auto Scope = JsonCodec::DecodeObjectId(parameters["Scope"]);
				if (!Scope) return Fail("MalformedRequest", "Scope must be a canonical ObjectId");
				if (Scope->ToObjectId() != World->GetObjectId())
					return Fail("StaleProject", "Scope does not identify the current project instance");
				if (!RequestCursor) return Fail("SnapshotRequired", "GetSnapshot must establish a cursor");
				if (HasRevisionConflict()) return Fail("Conflict", "The authoritative project revision changed");
				if (World->Transactions.GetOpenCount() != 0)
					return Fail("TransactionOpen", "Commit the open authoring transaction before a property batch");
				const auto &EncodedWrites = parameters["Writes"];
				if (EncodedWrites.empty()) return Fail("MalformedRequest", "Writes must not be empty");
				if (EncodedWrites.size() > MaximumPreparedPropertyWrites)
					return Fail("ResourceLimit", "Writes exceeds the 256-write limit");
				std::vector<PreparedPropertyWrite> Writes;
				Writes.reserve(EncodedWrites.size());
				for (const auto &Encoded : EncodedWrites) {
					if (!HasOnlyFields(Encoded, {"Object", "DeclaringClassSchemaId", "DeclaringDefinitionVersion", "Property", "Value"}) ||
						!Encoded.contains("Object") || !Encoded.contains("DeclaringClassSchemaId") ||
						!Encoded["DeclaringClassSchemaId"].is_string() || !Encoded.contains("DeclaringDefinitionVersion") ||
						!Encoded.contains("Property") || !Encoded["Property"].is_string() || !Encoded.contains("Value"))
						return Fail("MalformedRequest", "A batch write has invalid fields");
					const auto &Name = Encoded["Property"].get_ref<const std::string &>();
					const auto &SchemaText = Encoded["DeclaringClassSchemaId"].get_ref<const std::string &>();
					if (Name.empty() || Name.size() > MaximumProtocolIdentifierBytes || SchemaText.size() != 32)
						return Fail("MalformedRequest", "A property or schema identifier has invalid length");
					const auto Object = JsonCodec::DecodeObjectId(Encoded["Object"]);
					const auto Schema = SchemaId::Parse(SchemaText);
					const auto DefinitionVersion = JsonCodec::DecodeUnsigned32(Encoded["DeclaringDefinitionVersion"]);
					if (!Object || !Schema || !DefinitionVersion || *DefinitionVersion == 0)
						return Fail("MalformedRequest", "A batch write has invalid identity or version");
					for (const auto &Previous : Writes)
						if (Previous.Object == Object->ToObjectId() && Previous.PropertyName == Name)
							return Fail("DuplicateWrite", "An object/property pair occurs more than once");
					auto Target = ObjectRegistry::Get().Lookup(Object->ToObjectId());
					if (!Target || Target->GetDestroyed() || Target->IsDestroying() ||
						Target->GetReplicationScopeId() != World->GetObjectId())
						return Fail("StaleObject", "Object is not live in the current project instance");
					const auto *Property = Target->FindProperty(Name);
					if (!Property || Property->DeclaringSchemaId != *Schema ||
						Property->DeclaringDefinitionVersion != *DefinitionVersion)
						return Fail("StaleSchema", "Property identity or declaring schema version is incompatible");
					if (!Property->Write || Property->WritePermission == Enums::Permission::Never)
						return Fail("ReadOnly", "Property is read-only");
					if (!PreparedPropertyCommit::SupportsProperty(*Property))
						return Fail("UnsupportedProperty", "Property does not support prepared atomic writes");
					if (!AtomicBatchWritable(*Property, StudioSecurity))
						return Fail("Unauthorized", "Property access is not permitted");
					auto Value = JsonCodec::DecodeWireValue(Encoded["Value"]);
					if (!Value) return Fail("ValidationFailed", "WireValue is invalid");
					Writes.push_back({Object->ToObjectId(), *Schema, *DefinitionVersion, Name, std::move(*Value)});
				}
				PropertyBatchResponse Response(requestId);
				// Release the parsed tree while failures still precede mutation. No
				// references into parameters or EncodedWrites are used after this point.
				message = nullptr;
				const auto Result = PreparedPropertyCommit::Apply(*World, *ExpectedRevision, Writes, StudioSecurity, TeamAuthor);
				if (Result.Status != MutationStatus::Success) {
					const auto *Code = Result.Status == MutationStatus::Conflict ? "Conflict" :
						Result.Status == MutationStatus::InvalidProperty ? "StaleSchema" : MutationStatusName(Result.Status);
					return Fail(Code, "Prepared property batch was rejected without mutation");
				}
				Response.Complete(Result);
				return std::move(Response.Encoded);
			}

			if (!World)
				return SerializeBoundedResponse(ErrorResponse(requestId, "ProjectRequired", "OpenProject must succeed first"));
			auto StudioMutationAuthority = [&] {
				return MutationAuthorityContext::Studio(
					StudioSecurity, World->GetObjectId(), RequestTransaction, TransactionOwner, TeamAuthor
				);
			};

			if (method == "GetTerrainAuthoringState" || method == "SubmitTerrainBrush" ||
				method == "GetTerrainEditStatus" || method == "CancelTerrainEdit" ||
				method == "UndoTerrainEdit" || method == "RedoTerrainEdit" || method == "RaycastTerrainSurface") {
				using Action = TerrainAuthoringState::Action;
				using namespace terrain;
				const auto Fail = [&](std::string Code, std::string Message) {
					return SerializeBoundedResponse(ErrorResponse(requestId, std::move(Code), std::move(Message)));
				};
				const auto Reply = [&](Json Value) {
					return SerializeBoundedResponse(SuccessResponse(requestId, std::move(Value)));
				};
				const bool GetState = method == "GetTerrainAuthoringState";
				const bool GetStatus = method == "GetTerrainEditStatus";
				const bool Cancel = method == "CancelTerrainEdit";
				const bool Query = method == "RaycastTerrainSurface";
				if (StudioSecurity.Domain != ScriptExecutionDomain::Studio ||
					!StudioSecurity.HasCapability(ScriptCapability::EditorCommands) ||
					!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel) ||
					(Query && !StudioSecurity.HasCapability(ScriptCapability::ViewportControl)) ||
					(!GetState && !GetStatus && !Query && !StudioSecurity.HasCapability(ScriptCapability::MutateDataModel)))
					return Fail("Unauthorized", "Terrain authoring requires trusted Studio capabilities");
				const auto Space = std::dynamic_pointer_cast<Workspace>(World->FindFirstChildOfClass("Workspace", false));
				const auto Owner = Space ? Space->GetTerrain() : nullptr;
				if (!Owner || !CurrentProject) return Fail("TerrainUnavailable", "The project has no canonical Workspace terrain");
				const auto Scope = World->GetObjectId();
				if (!GetState) {
					const auto Requested = parameters.contains("Scope") ?
						JsonCodec::DecodeObjectId(parameters["Scope"]) : std::nullopt;
					if (!Requested) return Fail("MalformedRequest", "Terrain requests need a valid Scope");
					if (Requested->ToObjectId() != Scope) return Fail("StaleTerrainScope", "The project scope changed");
				}
				if (GetState || GetStatus) PumpTerrainAuthoring();
				const auto &Field = Owner->GetField();
				const auto Revision = Field.GetRevision();
				const auto IsBusy = [&] { return TerrainAuthoring && TerrainAuthoring->Pending.has_value(); };
				const auto HasHistory = [&] {
					return TerrainAuthoring && TerrainAuthoring->History && TerrainAuthoring->History->AtRevision == Revision;
				};
				const auto State = [&] {
					const bool HasUndo = HasHistory() && !TerrainAuthoring->History->Undone;
					const bool HasRedo = HasHistory() && TerrainAuthoring->History->Undone;
					return Json{
						{"Scope", JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(Scope))},
						{"TerrainRevision", std::to_string(Revision)},
						{"SampleSpacing", Field.GetGrid().GetSpacing()},
						{"MaximumBrushSamples", MaximumTerrainBrushSamples},
						{"EditingAvailable", !ActivePlaySession && !ActiveAnimationPreview && !GuiPreview &&
							StudioSecurity.HasCapability(ScriptCapability::MutateDataModel)},
						{"Busy", IsBusy()},
						{"CanUndo", !IsBusy() && !ActivePlaySession && HasUndo},
						{"CanRedo", !IsBusy() && !ActivePlaySession && HasRedo},
						{"ProjectState", EncodeProjectState(World, PersistedRevision, CurrentProject)},
					};
				};
				if (GetState) {
					if (!parameters.empty()) return Fail("MalformedRequest", "GetTerrainAuthoringState takes no parameters");
					return Reply(State());
				}
				if (GetStatus || Cancel) {
					const auto Ticket = parameters.contains("Ticket") ? DecodeCanonicalJobId(parameters["Ticket"]) : std::nullopt;
					if (!Ticket || !HasOnlyFields(parameters, {"Scope", "Ticket"}))
						return Fail("MalformedRequest", "A canonical nonzero terrain ticket is required");
					if (Cancel) {
						if (!IsBusy() || TerrainAuthoring->Pending->Ticket != *Ticket)
							return Fail("TerrainEditUnavailable", "The terrain edit is no longer pending");
						const auto Result = Mutations.Apply(CancelTerrainEditCommand{Owner->GetObjectId(), Scope, *Ticket},
							StudioMutationAuthority());
						if (!Result.Succeeded()) return Fail(MutationStatusName(Result.Status), Result.Message);
					}
					const auto Receipt = Space->GetTerrainTicket(*Ticket);
					if (!Receipt) return Fail("TerrainEditUnavailable", "The terrain ticket is unavailable in this scope");
					auto Result = State();
					Result["Ticket"] = std::to_string(*Ticket);
					Result["Status"] = Cancel ? std::string_view("CancelRequested") : Receipt->Status;
					Result["Diagnostic"] = Receipt->Diagnostic;
					Result["CommittedTerrainRevision"] = Receipt->Revision ?
						Json(std::to_string(*Receipt->Revision)) : Json(nullptr);
					return Reply(std::move(Result));
				}
				const auto RequestedRevision = parameters.contains("ExpectedTerrainRevision") ?
					DecodeCanonicalJobId(parameters["ExpectedTerrainRevision"], true) : std::nullopt;
				if (!RequestedRevision) return Fail("MalformedRequest", "A canonical expected terrain revision is required");
				if (*RequestedRevision != Revision || HasRevisionConflict())
					return Fail("Conflict", "The project or terrain revision changed");
				const auto Initialize = [&]() -> std::optional<std::string> {
					try {
						Space->InitializeTerrainRuntime(RuntimeMode::Offline);
						if (Space->HasTerrainRuntime()) return std::nullopt;
						return "Terrain collision is unavailable";
					} catch (const std::exception &Error) {
						return Error.what();
					}
				};
				if (Query) {
					if (!HasOnlyFields(parameters, {"Scope", "ExpectedTerrainRevision", "Origin", "Direction"}) ||
						!parameters.contains("Origin") || !parameters.contains("Direction"))
						return Fail("MalformedRequest", "Terrain raycast needs Origin and Direction");
					if (ActivePlaySession || ActiveAnimationPreview || GuiPreview)
						return Fail("ViewportUnavailable", "Terrain surface targeting requires the edit viewport");
					const auto Origin = DecodeVector3(parameters["Origin"]);
					const auto Direction = DecodeVector3(parameters["Direction"]);
					const auto Distance = Direction ? glm::length(*Direction) : 0.f;
					if (!Origin || !Direction || Distance <= 0 || Distance > 8192.f)
						return Fail("MalformedRequest", "Terrain raycast requires a finite displacement of at most 8192 world units");
					if (Field.GetStats().MaterializedPages == 0) return Reply({{"Hit", nullptr}});
					if (auto Denial = Initialize()) return Fail("TerrainUnavailable", std::move(*Denial));
					RaycastParams Filter;
					Filter.FilterType = Enums::RaycastFilterType::Include;
					Filter.FilterDescendantsInstances.Values.emplace_back(Owner);
					Filter.IncludeTerrain = true;
					const auto Hit = Space->ResolveRaycast(*Origin, *Direction, Filter);
					if (!Hit.Succeeded()) return Fail("TerrainQueryFailed", Hit.Message);
					if (!Hit.TerrainInstance) return Reply({{"Hit", nullptr}});
					return Reply({{"Hit", {
						{"Position", {Hit.Position.x, Hit.Position.y, Hit.Position.z}},
						{"Normal", {Hit.Normal.x, Hit.Normal.y, Hit.Normal.z}},
						{"Distance", Hit.Distance},
					}}});
				}
				if (!Cursor) return Fail("SnapshotRequired", "GetSnapshot must establish a cursor");
				if (RequestTransaction || World->Transactions.GetOpenCount() != 0)
					return Fail("TransactionOpen", "Close the object transaction before editing terrain");
				if (ActiveAnimationPreview || GuiPreview)
					return Fail("ViewportUnavailable", "Close the active preview before editing terrain");
				World->EnsureAuthoritativeRevisionAvailable();
				TerrainAuthoringState::Work Pending{.StartingRevision = Revision};
				EditRequest Edit;
				if (method == "SubmitTerrainBrush") {
					if (!HasOnlyFields(parameters, {"Scope", "ExpectedTerrainRevision", "ExpectedRevision", "Brush"}) ||
						!parameters.contains("Brush"))
						return Fail("MalformedRequest", "SubmitTerrainBrush needs a Brush");
					auto Input = DecodeTerrainBrush(parameters["Brush"], Field.GetGrid());
					if (!Input) return Fail("InvalidTerrainBrush", std::string(Input.error()));
					const auto Count = CountTerrainBrushSamples(Input->Bounds);
					if (!Count) return Fail("TerrainBrushLimit", "Use a smaller brush or region covering at most 4096 lattice samples");
					Pending.Patch.emplace();
					Pending.Patch->Before.reserve(*Count);
					Pending.Patch->After.resize(*Count);
					const auto [Min, Max] = Input->Bounds;
					for (auto Z = Min.Z; Z < Max.Z; ++Z)
						for (auto Y = Min.Y; Y < Max.Y; ++Y)
							for (auto X = Min.X; X < Max.X; ++X) {
								const SamplePoint Point{X, Y, Z};
								Pending.Patch->Before.push_back({Point, Field.Read(Point).value()});
							}
					Edit = std::move(Input->Edit);
				} else {
					if (!HasOnlyFields(parameters, {"Scope", "ExpectedTerrainRevision", "ExpectedRevision"}))
						return Fail("MalformedRequest", "Terrain history accepts the scope and expected revisions");
					const bool Undo = method == "UndoTerrainEdit";
					if (!HasHistory() || TerrainAuthoring->History->Undone == Undo)
						return Fail(Undo ? "NothingToUndo" : "NothingToRedo", "No terrain brush is available at this revision");
					const auto &History = *TerrainAuthoring->History;
					Pending.Operation = Undo ? Action::Undo : Action::Redo;
					Edit = Undo ? History.Before : History.After;
				}
				if (auto Denial = Initialize()) return Fail("TerrainUnavailable", std::move(*Denial));
				if (!TerrainAuthoring) TerrainAuthoring = std::make_unique<TerrainAuthoringState>();
				const auto Result = Mutations.Apply(SubmitTerrainEditCommand{Owner->GetObjectId(), Scope,
					std::make_shared<const TerrainMutation>(TerrainMutation{std::move(Edit), RequestedRevision})},
					StudioMutationAuthority());
				if (!Result.Succeeded() || !Result.TerrainTicket)
					return Fail(MutationStatusName(Result.Status), Result.Message);
				Pending.Ticket = *Result.TerrainTicket;
				TerrainAuthoring->Pending.emplace(std::move(Pending));
				return Reply({{"Ticket", std::to_string(*Result.TerrainTicket)}, {"Status", "Pending"}});
			}

			if (method == "ExportGasModel" || method == "ExportGasPlace") {
				if (!CurrentProject || !StudioSecurity.HasCapability(ScriptCapability::ReadDataModel) ||
					!StudioSecurity.HasCapability(ScriptCapability::EditorCommands))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "GAS export requires project authoring access"));
				const bool IsModel = method == "ExportGasModel";
				SerializationResult<Gas::Document> Document;
				std::optional<terrain::TerrainCapture> CapturedTerrain;
				if (IsModel) {
					if (!HasOnlyFields(parameters, {"Roots", "AssetIds"}) || !parameters.contains("Roots") ||
						!parameters["Roots"].is_array() || parameters["Roots"].empty() || parameters["Roots"].size() > 256)
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GASM export requires up to 256 root identities"));
					std::vector<Gas::Dependency> RequestedDependencies;
					if (parameters.contains("AssetIds")) {
						const auto &Ids = parameters["AssetIds"];
						if (!Ids.is_array() || Ids.size() > Gas::MaximumDependencies)
							return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GASM asset identity list is invalid"));
						for (const auto &EncodedId : Ids) {
							const auto Id = EncodedId.is_string() ? AssetId::Parse(EncodedId.get_ref<const std::string &>()) : std::nullopt;
							if (!Id)
								return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GASM asset identity is invalid"));
							RequestedDependencies.push_back({*Id, {}});
						}
					}
					std::vector<std::shared_ptr<Instance>> Roots;
					for (const auto &EncodedRoot : parameters["Roots"]) {
						const auto Id = JsonCodec::DecodeObjectId(EncodedRoot);
						const auto Root = Id ? ObjectRegistry::Get().Lookup(Id->ToObjectId()) : nullptr;
						if (!Root || Root->GetDataModel() != World)
							return SerializeBoundedResponse(ErrorResponse(requestId, "StaleObject", "GASM root is outside the current project"));
						Roots.push_back(Root);
					}
					Document = InstanceSerialization::CaptureGasDocument(Roots, Gas::DocumentKind::Model);
					if (Document) {
						Document->Dependencies = std::move(RequestedDependencies);
						for (const auto &Node : Document->Nodes) {
							const auto *Class = InstanceClassRegistry::GetDefinitionBySchemaId(Node.Class);
							if (!Class || !Class->EditorVisible || World->IsProtectedServiceClass(Node.Class) ||
								Node.Class == SchemaId::FromNativeName("Engine", "FileLink"))
								return SerializeBoundedResponse(ErrorResponse(requestId, "GasExportFailed",
									"GASM selection includes a class that cannot be inserted"));
						}
						const auto Assets = std::dynamic_pointer_cast<AssetService>(
							World->FindFirstChildOfClass("AssetService", false));
						auto Pins = Gas::ResolveDependencies(*Document, Assets.get(), false);
						if (!Pins)
							return SerializeBoundedResponse(ErrorResponse(requestId, "GasExportFailed", Pins.error().Message));
						Document->Dependencies = std::move(*Pins);
					}
				} else {
					if (!parameters.empty())
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GASP export takes no parameters"));
					Document = CurrentProject->CaptureGasPlace(World, &CapturedTerrain);
					if (Document && (Document->Place->PreRunSource != ActivePreRunSource ||
						GetRuntimeSchemaLifecycle().GetActiveGeneration() != ActivePreRunSchemaGeneration))
						return SerializeBoundedResponse(ErrorResponse(requestId, "GasExportFailed",
							"Reopen the project to apply PreRun changes before exporting"));
				}
				if (!Document)
					return SerializeBoundedResponse(ErrorResponse(requestId, "GasExportFailed", Document.error().Message));
				const auto Encoded = Gas::Encode(*Document);
				if (!Encoded)
					return SerializeBoundedResponse(ErrorResponse(requestId, "GasExportFailed", Encoded.error().Message));
				if (Encoded->size() > MaximumGasTransferBytes)
					return SerializeBoundedResponse(ErrorResponse(requestId, "ResourceLimit", "GAS exceeds the EditorHost transfer limit"));
				if (CapturedTerrain) {
					try {
						CurrentProject->PersistTerrainArtifacts(*CapturedTerrain);
					} catch (const std::exception &Error) {
						return SerializeBoundedResponse(ErrorResponse(requestId, "GasExportFailed", Error.what()));
					}
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"DataBase64", EncodeBase64Chunks(*Encoded)},
					{"DocumentKind", IsModel ? "Model" : "Place"}, {"FormatVersion", Gas::GetFormatVersion(*Document)},
				}));
			}

			if (method == "ImportGasModel" || method == "OpenGasPlace") {
				const bool IsModel = method == "ImportGasModel";
				if (!CurrentProject || !StudioSecurity.HasCapability(ScriptCapability::MutateDataModel) ||
					!StudioSecurity.HasCapability(ScriptCapability::EditorCommands))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "GAS import requires project authoring access"));
				if (!Cursor)
					return SerializeBoundedResponse(ErrorResponse(requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"));
				if (!HasOnlyFields(parameters, IsModel
					? std::initializer_list<std::string_view>{"DataBase64", "ExpectedRevision", "Parent"}
					: std::initializer_list<std::string_view>{"DataBase64", "ExpectedRevision"}) ||
					!parameters.contains("DataBase64") || !ExpectedRevision || (IsModel && !parameters.contains("Parent")))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GAS import fields are invalid"));
				if (HasRevisionConflict() || World->Transactions.GetOpenCount() != 0)
					return SerializeBoundedResponse(ErrorResponse(requestId, "Conflict", "GAS import requires a current revision and closed transaction"));
				Gas::Binary Data;
				try {
					Data = DecodeBase64Chunks(parameters["DataBase64"], MaximumGasTransferBytes);
				} catch (const std::invalid_argument &) {
					return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidGasDocument", "GAS transfer data is invalid"));
				}
				auto Document = Gas::Decode(Data);
				if (!Document || Document->Kind != (IsModel ? Gas::DocumentKind::Model : Gas::DocumentKind::Place))
					return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidGasDocument", "GAS document is invalid or has the wrong kind"));
				if (IsModel) {
					const auto Parent = JsonCodec::DecodeObjectId(parameters["Parent"]);
					if (!Parent)
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GASM import requires a parent identity"));
					const auto Result = Mutations.Apply(ImportGasModelCommand{
						std::make_shared<const Gas::Document>(std::move(*Document)), Parent->ToObjectId(), *ExpectedRevision,
					}, StudioMutationAuthority());
					if (!Result.Succeeded() || !Result.Object)
						return SerializeBoundedResponse(ErrorResponse(requestId, MutationStatusName(Result.Status), Result.Message));
					return SerializeBoundedResponse(SuccessResponse(requestId, {
						{"Root", JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(*Result.Object))},
						{"ProjectState", EncodeProjectState(World, PersistedRevision, CurrentProject)},
					}));
				}
				if (Document->Place->PreRunSource != ActivePreRunSource ||
					GetRuntimeSchemaLifecycle().GetActiveGeneration() != ActivePreRunSchemaGeneration)
					return SerializeBoundedResponse(ErrorResponse(requestId, "GasImportFailed",
						"GASP PreRun requires the schema already active in the current project"));
				struct StagedPlace {
					std::shared_ptr<DataModel> World;
					~StagedPlace() {
						if (!World) return;
						try {
							ScopedChangeJournalSuppression Suppression;
							World->Destroy();
						} catch (...) {}
					}
				} Staged;
				auto &Candidate = Staged.World;
				std::string Response;
				std::shared_ptr<const terrain::RenderState> TerrainRender;
				try {
					ScopedChangeJournalSuppression Suppression;
					Candidate = CurrentProject->LoadGasPlace(*Document);
					const auto Space = std::dynamic_pointer_cast<Workspace>(Candidate->GetService("Workspace"));
					if (!Space || !Space->GetTerrain())
						return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidProject", "GASP place has no valid Workspace"));
					const auto Terrain = Space->GetTerrain();
					auto Preview = terrain::TerrainRenderPreview::Prepare(Terrain->GetField());
					if (!Preview && Preview.error() == terrain::EditStatus::ReopenCapacityExceeded)
						return SerializeBoundedResponse(ErrorResponse(requestId, "TerrainReopenCapacityExceeded",
							"Editable terrain supports up to 512 stored pages and 1024 contour candidates."));
					if (!Preview)
						return SerializeBoundedResponse(ErrorResponse(requestId, "TerrainPreviewUnavailable",
							"The GASP place exceeds the edit viewport terrain budget or could not be prepared."));
					TerrainRender = std::move(*Preview).Bind(Terrain->GetObjectId());
					Candidate->InitializeLoadedProjectRevision();
					Response = SerializeBoundedResponse(SuccessResponse(requestId, {
						{"Root", JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(Candidate->GetObjectId()))},
						{"ProjectState", EncodeActiveProjectState(Candidate, 0, CurrentProject)},
					}));
				} catch (const std::exception &) {
					return SerializeBoundedResponse(ErrorResponse(requestId, "GasImportFailed", "GASP place could not be opened with the current project inputs"));
				}
				ResetEditorViewportDocument();
				World->Destroy();
				World = std::move(Candidate);
				ViewportPublisher->SetTerrainRenderState(std::move(TerrainRender));
				ActiveAnimationPreview.reset();
				PersistedRevision = 0;
				ResetAuthoringCursors();
				ViewportCamera = RenderCameraInput{};
				AdvanceViewportGeneration();
				return Response;
			}

			auto workspace = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
			if (!workspace || !ViewportCamera)
				return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidProject", "Project has no valid editor viewport state"));
			auto &camera = *ViewportCamera;

			if (method == "SetGuiPreview" || method == "GetGuiPreview") {
				if (method == "SetGuiPreview") {
					if (ActivePlaySession)
						return SerializeBoundedResponse(ErrorResponse(requestId, "PlaySessionActive", "Stop Play before changing GUI preview"));
					if (!HasOnlyFields(parameters, {"Enabled", "Width", "Height"}) || !parameters.contains("Enabled") ||
						!parameters["Enabled"].is_boolean() || parameters.contains("Width") != parameters.contains("Height"))
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "SetGuiPreview requires Enabled and an optional dimension pair"));
					std::uint32_t Width = 0, Height = 0;
					if (parameters.contains("Width")) {
						const auto ParsedWidth = JsonCodec::DecodeUnsigned32(parameters["Width"]);
						const auto ParsedHeight = JsonCodec::DecodeUnsigned32(parameters["Height"]);
						if (!ParsedWidth || !ParsedHeight || *ParsedWidth == 0 || *ParsedHeight == 0 ||
							*ParsedWidth > EditorHostMaximumViewportWidth || *ParsedHeight > EditorHostMaximumViewportHeight ||
							static_cast<std::uint64_t>(*ParsedWidth) * *ParsedHeight > EditorHostMaximumViewportPixels)
							return SerializeBoundedResponse(ErrorResponse(requestId, "ViewportTooLarge", "GUI preview dimensions exceed protocol limits"));
						Width = *ParsedWidth;
						Height = *ParsedHeight;
					}
					const bool Enabled = parameters["Enabled"].get<bool>();
					if (Enabled && (!GuiPreview || GuiPreview->Width != Width || GuiPreview->Height != Height)) {
						if (!GuiPreview) GuiPreview = std::make_unique<GuiPreviewState>(World);
						GuiPreview->Width = Width;
						GuiPreview->Height = Height;
						GuiPreview->DisplayedFrames.clear();
						NextViewportFrame = std::chrono::steady_clock::now();
					} else if (!Enabled && GuiPreview) {
						ResetGuiPreview();
						NextViewportFrame = std::chrono::steady_clock::now();
					}
				} else if (!parameters.empty()) {
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GetGuiPreview takes no parameters"));
				}
				const auto Placement = GetGuiPreviewPlacement(GuiPreview ? GuiPreview->Width : 0,
					GuiPreview ? GuiPreview->Height : 0, ViewportWidth, ViewportHeight);
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"Enabled", GuiPreview != nullptr && !ActivePlaySession}, {"Generation", ViewportGeneration},
					{"Width", Placement.Width}, {"Height", Placement.Height},
					{"X", Placement.Bounds.X}, {"Y", Placement.Bounds.Y},
					{"DisplayWidth", Placement.Bounds.Width}, {"DisplayHeight", Placement.Bounds.Height},
				}));
			}

			if (method == "PickGuiPreview") {
				if (ActivePlaySession)
					return SerializeBoundedResponse(ErrorResponse(requestId, "PlaySessionActive", "Stop Play before picking authored GUI"));
				if (!GuiPreview)
					return SerializeBoundedResponse(ErrorResponse(requestId, "GuiPreviewInactive", "Enable GUI preview before picking"));
				if (!HasOnlyFields(parameters, {"X", "Y", "Object", "FrameNumber", "Generation", "IncludeLayout"}) ||
					!parameters.contains("FrameNumber") || !parameters["FrameNumber"].is_number_unsigned() ||
					!parameters.contains("Generation") || !parameters["Generation"].is_number_unsigned() ||
					(parameters.contains("IncludeLayout") && !parameters["IncludeLayout"].is_boolean()))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "PickGuiPreview requires the displayed frame identity"));
				const bool IncludeLayout = parameters.value("IncludeLayout", false);
				std::optional<WireObjectId> LayoutObject;
				float X = 0.0f;
				float Y = 0.0f;
				if (parameters.contains("Object")) {
					LayoutObject = JsonCodec::DecodeObjectId(parameters["Object"]);
					if (!IncludeLayout || !LayoutObject || parameters.contains("X") || parameters.contains("Y"))
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GUI layout lookup requires only an authored object and frame identity"));
				} else {
					if (!parameters.contains("X") || !parameters["X"].is_number() ||
						!parameters.contains("Y") || !parameters["Y"].is_number())
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GUI preview pick requires pixels"));
					X = parameters["X"].get<float>();
					Y = parameters["Y"].get<float>();
				}
				const auto FrameNumber = parameters["FrameNumber"].get<std::uint64_t>();
				const auto Generation = parameters["Generation"].get<std::uint64_t>();
				if (FrameNumber == 0 || Generation == 0 || !std::isfinite(X) || !std::isfinite(Y))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GUI preview pick values are invalid"));
				const auto &Frames = GuiPreview->DisplayedFrames;
				const auto Frame = std::ranges::find(Frames, FrameNumber, &GuiPreviewState::DisplayedFrame::Number);
				if (Generation != ViewportGeneration || Frame == Frames.end())
					return SerializeBoundedResponse(ErrorResponse(requestId, "StaleFrame", "The displayed GUI preview frame is no longer available"));
				const auto &Placement = Frame->Placement;
				const auto Starter = World->FindFirstChildOfClass("StarterGui", false);
				const auto IsAuthored = [&](const GuiLayoutNode &Node) {
					if (!Node.Parent.IsValid() || !Node.EffectiveVisible || Node.EffectiveOpacity <= 0.0f) return false;
					const auto Object = ObjectRegistry::Get().Lookup(Node.Object);
					return Object && Starter && Object->GetDataModel() == World &&
						Object->FindFirstAncestorWhichIsA("StarterGui") == Starter;
				};
				const GuiLayoutNode *Hit = nullptr;
				if (LayoutObject) {
					const auto Node = Frame->Layout->NodeByObject.find(LayoutObject->ToObjectId());
					if (Node != Frame->Layout->NodeByObject.end() && IsAuthored(Frame->Layout->Nodes[Node->second]))
						Hit = &Frame->Layout->Nodes[Node->second];
				} else if (Placement.Bounds.Contains(X, Y)) {
					const Vector2 Point((X - Placement.Bounds.X) / Placement.Scale, (Y - Placement.Bounds.Y) / Placement.Scale);
					for (auto Node = Frame->Layout->Nodes.rbegin(); Node != Frame->Layout->Nodes.rend(); ++Node) {
						if (Node->EffectiveClip && !Node->EffectiveClip->Contains(Point.GetX(), Point.GetY())) continue;
						const auto Inverse = Node->Transform.Inverse();
						if (!Inverse) continue;
						const auto Local = Inverse->Apply(Point);
						if (!Node->Bounds.Contains(Local.GetX(), Local.GetY()) || !IsAuthored(*Node)) continue;
						Hit = &*Node;
						break;
					}
				}
				if (!Hit) {
					Json Result = {{"Object", nullptr}, {"Bounds", nullptr}};
					if (IncludeLayout) Result["Revision"] = Frame->ProjectRevision;
					return SerializeBoundedResponse(SuccessResponse(requestId, std::move(Result)));
				}
				const auto Unclipped = PlaceGuiRect(Hit->TransformedBounds, Placement);
				auto Bounds = Unclipped;
				if (Hit->EffectiveClip)
					Bounds = GuiRect::Intersect(Bounds, PlaceGuiRect(*Hit->EffectiveClip, Placement)).value_or(GuiRect{});
				Bounds = GuiRect::Intersect(Bounds, Placement.Bounds).value_or(GuiRect{});
				Json Result = {
					{"Object", JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(Hit->Object))},
					{"Bounds", {{"X", Bounds.X}, {"Y", Bounds.Y}, {"Width", Bounds.Width}, {"Height", Bounds.Height}}},
				};
				if (IncludeLayout) Result["Revision"] = Frame->ProjectRevision;
				if (IncludeLayout && Frame->ProjectRevision != 0) {
					const auto AxisAligned = [](const GuiTransform &Transform) {
						return std::abs(Transform.M00 - 1.0f) < 0.0001f && std::abs(Transform.M11 - 1.0f) < 0.0001f &&
							std::abs(Transform.M01) < 0.0001f && std::abs(Transform.M10) < 0.0001f;
					};
					const auto Parent = Frame->Layout->NodeByObject.find(Hit->Parent);
					Result["Layout"] = {
						{"Revision", Frame->ProjectRevision}, {"Scale", Placement.Scale},
						{"AxisAligned", AxisAligned(Hit->Transform) && Parent != Frame->Layout->NodeByObject.end() &&
							AxisAligned(Frame->Layout->Nodes[Parent->second].Transform)},
						{"Clipped", Bounds.Width < Unclipped.Width - 0.001f || Bounds.Height < Unclipped.Height - 0.001f},
						{"LayoutPositionsX", Hit->LayoutPositionsX}, {"LayoutPositionsY", Hit->LayoutPositionsY},
						{"LayoutSizesX", Hit->LayoutSizesX}, {"LayoutSizesY", Hit->LayoutSizesY},
						{"Width", Hit->Bounds.Width}, {"Height", Hit->Bounds.Height},
					};
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, std::move(Result)));
			}

			if (method == "ConfigureViewport") {
				if (!HasOnlyFields(parameters, {"Width", "Height"}) ||
					!parameters.contains("Width") || !parameters["Width"].is_number_unsigned() ||
					!parameters.contains("Height") || !parameters["Height"].is_number_unsigned())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Viewport dimensions must be unsigned"));
				auto width = JsonCodec::DecodeUnsigned32(parameters["Width"]);
				auto height = JsonCodec::DecodeUnsigned32(parameters["Height"]);
				const bool UsesCompatibilityCapture = !ViewportLatestFrameMailbox;
				if (!width || !height || *width == 0 || *height == 0 ||
					*width > EditorHostMaximumViewportWidth || *height > EditorHostMaximumViewportHeight ||
					static_cast<std::uint64_t>(*width) * *height > EditorHostMaximumViewportPixels ||
					(UsesCompatibilityCapture && (*width > 1024 || *height > 1024 ||
						static_cast<std::uint64_t>(*width) * *height > 1024ull * 1024ull)))
					return SerializeBoundedResponse(ErrorResponse(requestId, "ViewportTooLarge", "Viewport dimensions exceed protocol limits"));
				const bool firstConfiguration = ViewportWidth == 0 || ViewportHeight == 0;
				const bool DimensionsChanged = ViewportWidth != *width || ViewportHeight != *height;
				ViewportWidth = *width;
				ViewportHeight = *height;
				if (firstConfiguration)
					camera = MakeLookAtRenderCameraInput(
						{10.0f, 10.0f, 10.0f},
						{0.0f, 0.0f, 0.0f},
						{0.0f, 1.0f, 0.0f},
						camera.VerticalFieldOfView
					);
				if (DimensionsChanged) {
					AdvanceViewportGeneration();
					++ViewportResizeCount;
				}
				if (DimensionsChanged && ViewportRenderer) {
					try {
						ViewportRenderer->Resize(*width, *height);
					} catch (const std::exception &error) {
						ViewportRenderer.reset();
						return SerializeBoundedResponse(ErrorResponse(requestId, "ViewportUnavailable", error.what()));
					}
				}
				if (DimensionsChanged && ActivePlaySession) ActivePlaySession->Resize(*width, *height);
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"ViewportVersion", ViewportLatestFrameMailbox ? 2 : 1},
					{"Generation", ViewportGeneration}, {"Width", *width}, {"Height", *height},
					{"Format", ViewportLatestFrameMailbox ? "BGRA8" : "RGB8"},
				}));
			}

			if (method == "SetViewportCamera") {
				if (!HasOnlyFields(parameters, {"Position", "Target", "FieldOfView", "CameraRevision"}) ||
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
				std::optional<std::uint64_t> RequestedCameraRevision;
				if (parameters.contains("CameraRevision")) {
					if (!parameters["CameraRevision"].is_number_unsigned() ||
						parameters["CameraRevision"].get<std::uint64_t>() == 0)
						return SerializeBoundedResponse(ErrorResponse(
							requestId, "InvalidCamera", "CameraRevision must be a nonzero unsigned integer"
						));
					RequestedCameraRevision = parameters["CameraRevision"].get<std::uint64_t>();
					if (*RequestedCameraRevision < ViewportCameraRevision)
						return SerializeBoundedResponse(ErrorResponse(
							requestId, "StaleCamera", "CameraRevision is older than the accepted editor camera state"
						));
				}
				camera = MakeLookAtRenderCameraInput(*position, *target, {0.0f, 1.0f, 0.0f}, fieldOfView);
				if (RequestedCameraRevision) ViewportCameraRevision = *RequestedCameraRevision;
				else if (ViewportCameraRevision != std::numeric_limits<std::uint64_t>::max()) {
					++ViewportCameraRevision;
				}
				if (!ActivePlaySession || !ActivePlaySession->IsRootDebugPaused())
					LastViewportPublication.reset();
				NextViewportFrame = std::chrono::steady_clock::now();
				return SerializeBoundedResponse(SuccessResponse(requestId, {
					{"Position", {position->x, position->y, position->z}},
					{"Target", {target->x, target->y, target->z}},
					{"FieldOfView", fieldOfView}, {"CameraRevision", ViewportCameraRevision},
				}));
			}

			if (method == "CaptureViewport") {
				if (!parameters.empty())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "CaptureViewport takes no parameters"));
				if (GuiPreview && ViewportPresentationActive && ViewportLatestFrameMailbox && !ActivePlaySession)
					return SerializeBoundedResponse(ErrorResponse(requestId, "ViewportTransportActive", "Read the GUI preview from the active frame transport"));
				if (ViewportWidth == 0 || ViewportHeight == 0)
					return SerializeBoundedResponse(ErrorResponse(requestId, "ViewportRequired", "ConfigureViewport must succeed first"));
				try {
					EnsureViewportRenderer();
					Json PlayIdentity = nullptr;
					const char *Mode = "Edit";
					if (ActivePlaySession && ActivePlaySession->GetState() == PlaySessionState::Running) {
						ConsumePlayRenderPublications(ViewportRenderer.get());
						ActivePlaySession->Step();
						if (ActivePlaySession->GetState() != PlaySessionState::Running) {
							LastPlaySessionState = ActivePlaySession->GetState();
							AdvanceViewportGeneration();
							ViewportRenderer.reset();
							return SerializeBoundedResponse(ErrorResponse(requestId, "PlaySessionExited", "Runtime exited while capturing the viewport"));
						}
						ConsumePlayRenderPublications(ViewportRenderer.get());
						if (!LastViewportPublication)
							return SerializeBoundedResponse(ErrorResponse(requestId, "PlaySessionFailed", "Runtime produced no render publication"));
						PlayIdentity = std::to_string(ActivePlaySession->GetId().Value);
						Mode = "Play";
					} else {
						PublishEditViewport(*workspace, camera);
					}
					if (ViewportProjection.GetLastPublicationId() != LastViewportPublication->Id)
						(void)ViewportProjection.Apply(*LastViewportPublication);
					auto frame = ViewportRenderer->Capture(LastViewportPublication);
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
						RecordGuiPreviewFrame(sequence);
						return SerializeBoundedResponse(SuccessResponse(requestId, {
							{"ViewportVersion", 1}, {"FrameNumber", sequence}, {"Generation", ViewportGeneration},
							{"Width", frame.Width}, {"Height", frame.Height}, {"Format", "RGB8"},
							{"Transport", "SharedMemoryRing"}, {"TransportVersion", SharedFrameRingLayout::Version},
							{"Mode", Mode}, {"PlaySessionId", PlayIdentity},
						}));
					}
					++ViewportFrameNumber;
					RecordGuiPreviewFrame(ViewportFrameNumber);
					return SerializeBoundedResponse(SuccessResponse(requestId, {
						{"ViewportVersion", 1}, {"FrameNumber", ViewportFrameNumber}, {"Generation", ViewportGeneration},
						{"Width", frame.Width}, {"Height", frame.Height}, {"Format", "RGB8"},
						{"Transport", "Base64"}, {"Encoding", "Base64"}, {"Pixels", EncodeBase64(frame.RgbPixels)},
						{"Mode", Mode}, {"PlaySessionId", PlayIdentity},
					}));
				} catch (const std::exception &error) {
					ViewportRenderer.reset();
					ViewportPublisher->RequestFullResync();
					LastViewportPublication.reset();
					ViewportProjection.Clear();
					if (ActivePlaySession) {
						ActivePlaySession->Stop();
						LastPlaySessionState = PlaySessionState::Failed;
						return SerializeBoundedResponse(ErrorResponse(requestId, "PlaySessionExited", error.what()));
					}
					return SerializeBoundedResponse(ErrorResponse(requestId, "ViewportUnavailable", error.what()));
				}
			}

			if (method == "PickViewport") {
				if (ActivePlaySession)
					return SerializeBoundedResponse(ErrorResponse(requestId, "PlaySessionActive", "Runtime object selection is not available during Play"));
				if (GuiPreview)
					return SerializeBoundedResponse(ErrorResponse(requestId, "GuiPreviewActive", "Use PickGuiPreview while editing the GUI preview"));
				if (!HasOnlyFields(parameters, {"X", "Y"}) || !parameters.contains("X") ||
					!parameters["X"].is_number() || !parameters.contains("Y") || !parameters["Y"].is_number())
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "PickViewport requires numeric X and Y"));
				if (ViewportWidth == 0 || ViewportHeight == 0)
					return SerializeBoundedResponse(ErrorResponse(requestId, "ViewportRequired", "ConfigureViewport must succeed first"));
				if (!LastViewportPublication) {
					PublishEditViewport(*workspace, camera);
				}
				if (ViewportProjection.GetLastPublicationId() != LastViewportPublication->Id)
					(void)ViewportProjection.Apply(*LastViewportPublication);
				auto pick = PickEditorViewport(
					ViewportProjection, parameters["X"].get<float>(), parameters["Y"].get<float>()
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
				if (TeamAuthor != 0) RequestCursor.reset();
				auto snapshot = CaptureSnapshot(World, SnapshotAudience::Editor);
				if (TeamAuthor == 0) RequestCursor = snapshot.Cursor;
				auto EncodedSnapshot = ParseGeneratedJson(SerializeSnapshot(snapshot), "Snapshot response");
				AddEditorPropertyValues(EncodedSnapshot, StudioSecurity, World->GetObjectId());
				bool Encoded = false;
				auto Response = SerializeBoundedResponse(SuccessResponse(
					requestId,
					{
						{"Snapshot", std::move(EncodedSnapshot)},
						{"ProjectState", EncodeActiveProjectState(World, PersistedRevision, CurrentProject)},
					}
				), &Encoded);
				if (TeamAuthor != 0 && Encoded) RequestCursor = snapshot.Cursor;
				return Response;
			}

			if (method == "PollChanges") {
				if (!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Journal access requires ReadDataModel"));
				if (!RequestCursor)
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
				auto &Journal = ChangeJournal::Get();
				auto changes = Journal.ReadPinned(*RequestCursor, maximumRecords, TeamAuthor != 0);
				auto ResnapshotRequired = [&] {
					if (TeamAuthor != 0) RequestCursor.reset();
					return SerializeBoundedResponse(ErrorResponse(requestId, "ResnapshotRequired",
						TeamAuthor == 0 ? "Journal cursor was evicted" : "Journal page cannot be delivered as complete commits"));
				};
				if (changes.Status == ChangeReadStatus::ResnapshotRequired) return ResnapshotRequired();
				std::vector<WireJournalRecord> records;
				records.reserve(changes.Records.size());
				for (const auto &record : changes.Records) records.push_back(EncodeChangeRecord(*record));
				if (TeamAuthor == 0) RequestCursor = changes.Cursor;
				auto EncodedWire = SerializeWireJournalRecords(records);
				if (TeamAuthor != 0 && EncodedWire.size() > EditorHostMaximumResponseBytes) return ResnapshotRequired();
				auto wire = ParseGeneratedJson(EncodedWire, "Journal response");
				Json Result = {
					{"Records", std::move(wire["Records"])},
					{"Cursor", EncodeCursor(changes.Cursor)},
					{"ProjectState", EncodeProjectState(World, PersistedRevision, CurrentProject, TeamAuthor)},
				};
				if (TeamAuthor != 0)
					Result["HasMore"] = changes.Cursor.NextSequence < Journal.GetRetentionWindow(changes.Cursor.Scope).NextSequence;
				bool Encoded = false;
				auto Response = SerializeBoundedResponse(SuccessResponse(requestId, std::move(Result)), &Encoded);
				if (TeamAuthor != 0) {
					if (!Encoded) return ResnapshotRequired();
					RequestCursor = changes.Cursor;
				}
				return Response;
			}

			if (method == "GetScriptBinding" || method == "ResolveScriptBinding") {
				if (!StudioSecurity.HasCapability(ScriptCapability::ReadDataModel) ||
					!StudioSecurity.HasCapability(ScriptCapability::EditorCommands))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "Script binding requires EditorCommands and ReadDataModel"));
				if (!World || !CurrentProject)
					return SerializeBoundedResponse(ErrorResponse(requestId, "ProjectRequired", "Open a project first"));
				std::shared_ptr<LuaSourceContainer> Script;
				if (method == "GetScriptBinding") {
					if (!HasOnlyFields(parameters, {"Object"}) || !parameters.contains("Object"))
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "GetScriptBinding requires Object"));
					const auto Id = JsonCodec::DecodeObjectId(parameters["Object"]);
					if (!Id) return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "ObjectId is invalid"));
					const auto Target = ObjectRegistry::Get().Lookup(Id->ToObjectId());
					if (!Target || Target->GetReplicationScopeId() != World->GetObjectId())
						return SerializeBoundedResponse(ErrorResponse(requestId, "StaleObject", "Object is not live in the authoring project"));
					Script = std::dynamic_pointer_cast<LuaSourceContainer>(Target);
					if (!Script) return SerializeBoundedResponse(ErrorResponse(requestId, "NotScript", "Object is not a supported script"));
					const auto Found = FindScriptResource(World, Script->GetSourceId());
					if (Found.Error) return SerializeBoundedResponse(ErrorResponse(requestId, Found.Error, "Script resource lookup rejected"));
					if (Found.Script != Script)
						return SerializeBoundedResponse(ErrorResponse(requestId, "ResourceNotPersistent", "Script is not in the persistent project hierarchy"));
				} else {
				if (!HasOnlyFields(parameters, {"Binding", "ExpectedScope", "ExpectedObject", "ExpectedSourceVersion"}) ||
					!parameters.contains("Binding") ||
					parameters.contains("ExpectedScope") != parameters.contains("ExpectedObject") ||
					parameters.contains("ExpectedScope") != parameters.contains("ExpectedSourceVersion"))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "ResolveScriptBinding requires Binding and complete source location fields"));
				std::optional<WireObjectId> ExpectedObject;
				std::optional<std::uint32_t> ExpectedSourceVersion;
				if (parameters.contains("ExpectedScope")) {
					const auto Scope = JsonCodec::DecodeObjectId(parameters["ExpectedScope"]);
					ExpectedObject = JsonCodec::DecodeObjectId(parameters["ExpectedObject"]);
					ExpectedSourceVersion = JsonCodec::DecodeUnsigned32(parameters["ExpectedSourceVersion"]);
					if (!Scope || !ExpectedObject || !ExpectedSourceVersion || *ExpectedSourceVersion == 0)
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Source location fence is invalid"));
					if (Scope->ToObjectId() != World->GetObjectId())
						return SerializeBoundedResponse(ErrorResponse(requestId, "StaleProject", "Source location belongs to a retired authoring scope"));
				}
					const auto &Binding = parameters["Binding"];
					if (!Binding.is_object() || Binding.size() != 5 ||
						!HasOnlyFields(Binding, {"Version", "ProjectId", "SourceId", "ClassSchemaId", "ClassDefinitionVersion"}) ||
						!Binding.contains("Version") || !Binding["Version"].is_number_unsigned() || Binding["Version"] != 1 ||
						!Binding.contains("ProjectId") || !Binding["ProjectId"].is_string() ||
						!Binding.contains("SourceId") || !Binding["SourceId"].is_string() ||
						!Binding.contains("ClassSchemaId") || !Binding["ClassSchemaId"].is_string() ||
						!Binding.contains("ClassDefinitionVersion"))
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Script binding fields are invalid"));
					const auto ProjectIdentity = ProjectId::Parse(Binding["ProjectId"].get_ref<const std::string &>());
					const auto SourceIdentity = ProjectId::Parse(Binding["SourceId"].get_ref<const std::string &>());
					const auto ClassId = SchemaId::Parse(Binding["ClassSchemaId"].get_ref<const std::string &>());
					const auto Version = JsonCodec::DecodeUnsigned32(Binding["ClassDefinitionVersion"]);
					if (!ProjectIdentity || !SourceIdentity || !ClassId ||
						ClassId->ToString() != Binding["ClassSchemaId"].get_ref<const std::string &>() || !Version || *Version == 0)
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Script binding identities are invalid"));
					if (*ProjectIdentity != CurrentProject->Identity)
						return SerializeBoundedResponse(ErrorResponse(requestId, "StaleProject", "Binding belongs to another project"));
					const auto Found = FindScriptResource(World, Binding["SourceId"].get_ref<const std::string &>());
					if (Found.Error) return SerializeBoundedResponse(ErrorResponse(requestId, Found.Error, "Script resource lookup rejected"));
					Script = Found.Script;
					if (!Script) return SerializeBoundedResponse(ErrorResponse(requestId, "ResourceNotFound", "Script resource no longer exists"));
					const auto *Definition = InstanceClassRegistry::GetDefinition(Script.get());
				if (Definition->Id != *ClassId || Definition->DefinitionVersion != *Version)
					return SerializeBoundedResponse(ErrorResponse(requestId, "IncompatibleBinding", "Script resource schema changed"));
				if (ExpectedObject && ExpectedObject->ToObjectId() != Script->GetObjectId())
					return SerializeBoundedResponse(ErrorResponse(requestId, "StaleObject", "Source location belongs to a retired Script object"));
				if (ExpectedSourceVersion && *ExpectedSourceVersion != static_cast<std::uint32_t>(Script->GetSourceVersion()))
					return SerializeBoundedResponse(ErrorResponse(requestId, "SourceVersionConflict", "Source changed since the Play failure"));
				}
				return SerializeBoundedResponse(SuccessResponse(requestId, EncodeScriptBindingResult(*CurrentProject, World, PersistedRevision, Script)));
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
				if (!RequestCursor)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"
					));
				if (!HasOnlyFields(parameters, {"Object", "ExpectedSourceVersion", "ExpectedRevision", "Source", "TransactionId"}) ||
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
				if (HasRevisionConflict())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Conflict", "The authoritative project revision changed before script mutation"
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

			if (method == "SetTransform") {
				if (!StudioSecurity.HasCapability(ScriptCapability::MutateDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "SetTransform requires MutateDataModel"));
				if (!RequestCursor)
					return SerializeBoundedResponse(ErrorResponse(requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"));
				if (!HasOnlyFields(parameters, {"Object", "CFrame", "Size", "TransactionId", "ExpectedRevision"}) ||
					!parameters.contains("Object") ||
					(!parameters.contains("CFrame") && !parameters.contains("Size")))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "SetTransform fields are invalid"));
				auto Object = JsonCodec::DecodeObjectId(parameters["Object"]);
				if (!Object)
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "SetTransform Object is invalid"));
				if (HasRevisionConflict())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Conflict", "The authoritative project revision changed before transform mutation"
					));
				auto Target = ObjectRegistry::Get().Lookup(Object->ToObjectId());
				if (!Target || Target->GetReplicationScopeId() != World->GetObjectId())
					return SerializeBoundedResponse(ErrorResponse(requestId, "StaleObject", "Object is not live in the open project"));
				if (!std::dynamic_pointer_cast<BasePart>(Target))
					return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidClass", "SetTransform requires a BasePart target"));
				std::vector<WirePropertyMutation> Properties;
				if (parameters.contains("CFrame")) {
					auto Value = JsonCodec::DecodeWireValue(parameters["CFrame"]);
					if (!Value || !std::holds_alternative<WireCFrame>(*Value))
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "SetTransform CFrame is invalid"));
					Properties.push_back({"CFrame", std::move(*Value)});
				}
				if (parameters.contains("Size")) {
					auto Value = JsonCodec::DecodeWireValue(parameters["Size"]);
					if (!Value || !std::holds_alternative<WireVector3>(*Value))
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "SetTransform Size is invalid"));
					const auto &Size = std::get<WireVector3>(*Value);
					if (!std::isfinite(Size.X) || !std::isfinite(Size.Y) || !std::isfinite(Size.Z) ||
						Size.X < 0.01f || Size.Y < 0.01f || Size.Z < 0.01f)
						return SerializeBoundedResponse(ErrorResponse(
							requestId, "ValidationFailed", "SetTransform Size must be finite and at least 0.01 on every axis"
						));
					Properties.push_back({"Size", std::move(*Value)});
				}
				auto Mutation = Mutations.Apply(UpdateWirePropertiesCommand{
					Object->ToObjectId(), std::move(Properties)
				}, StudioMutationAuthority());
				Json Result{{"Status", MutationStatusName(Mutation.Status)}, {"Message", Mutation.Message}};
				if (Mutation.Object)
					Result["Object"] = JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(*Mutation.Object));
				return SerializeBoundedResponse(Mutation.Succeeded()
					? SuccessResponse(requestId, std::move(Result))
					: ErrorResponse(requestId, MutationStatusName(Mutation.Status), Mutation.Message));
			}

			if (method == "SetProperty") {
				if (!StudioSecurity.HasCapability(ScriptCapability::MutateDataModel))
					return SerializeBoundedResponse(ErrorResponse(requestId, "Unauthorized", "SetProperty requires MutateDataModel"));
				if (!RequestCursor)
					return SerializeBoundedResponse(ErrorResponse(requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"));
				if (!HasOnlyFields(parameters, {
						"Object", "ClassSchemaId", "ClassDefinitionVersion", "DeclaringClassSchemaId",
						"DeclaringDefinitionVersion", "Property", "Value", "TransactionId", "ExpectedRevision"
					}) ||
					!parameters.contains("Object") || !parameters.contains("Property") ||
					!parameters["Property"].is_string() || parameters["Property"].get_ref<const std::string &>().empty() ||
					parameters["Property"].get_ref<const std::string &>().size() > 256 || !parameters.contains("Value"))
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "SetProperty fields are invalid"));
				auto object = JsonCodec::DecodeObjectId(parameters["Object"]);
				auto value = JsonCodec::DecodeWireValue(parameters["Value"]);
				if (!object || !value)
					return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "SetProperty Object or WireValue is invalid"));
				if (HasRevisionConflict())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Conflict", "The authoritative project revision changed before property mutation"
					));
				auto target = ObjectRegistry::Get().Lookup(object->ToObjectId());
				if (!target || target->GetReplicationScopeId() != World->GetObjectId())
					return SerializeBoundedResponse(ErrorResponse(requestId, "StaleObject", "Object is not live in the open project"));
				const auto &PropertyName = parameters["Property"].get_ref<const std::string &>();
				if (PropertyName == "Source" && std::dynamic_pointer_cast<LuaSourceContainer>(target))
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Unauthorized", "Script source requires the dedicated source-authoring operation"
					));
				const bool SemanticRequest = parameters.contains("ClassSchemaId") ||
					parameters.contains("ClassDefinitionVersion") || parameters.contains("DeclaringClassSchemaId") ||
					parameters.contains("DeclaringDefinitionVersion");
				if (!SemanticRequest && PropertyName != "Name")
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "StaleSchema", "Native property editing requires exact class and declaring-schema identity"
					));
				if (SemanticRequest) {
					if (!parameters.contains("ClassSchemaId") || !parameters.contains("ClassDefinitionVersion") ||
						!parameters.contains("DeclaringClassSchemaId") || !parameters.contains("DeclaringDefinitionVersion") ||
						!parameters["ClassSchemaId"].is_string() || !parameters["DeclaringClassSchemaId"].is_string())
						return SerializeBoundedResponse(ErrorResponse(
							requestId, "MalformedRequest", "SetProperty semantic identity fields are incomplete"
						));
					auto ClassId = SchemaId::Parse(parameters["ClassSchemaId"].get<std::string>());
					auto DeclaringId = SchemaId::Parse(parameters["DeclaringClassSchemaId"].get<std::string>());
					auto ClassVersion = JsonCodec::DecodeUnsigned32(parameters["ClassDefinitionVersion"]);
					auto DeclaringVersion = JsonCodec::DecodeUnsigned32(parameters["DeclaringDefinitionVersion"]);
					auto *TargetClass = InstanceClassRegistry::GetDefinition(target.get());
					auto *Property = target->FindProperty(PropertyName);
					if (!ClassId || !DeclaringId || !ClassVersion || !DeclaringVersion ||
						*ClassVersion == 0 || *DeclaringVersion == 0 || !TargetClass || !Property ||
						TargetClass->Id != *ClassId || TargetClass->DefinitionVersion != *ClassVersion ||
						Property->DeclaringSchemaId != *DeclaringId ||
						Property->DeclaringDefinitionVersion != *DeclaringVersion)
						return SerializeBoundedResponse(ErrorResponse(
							requestId, "StaleSchema", "SetProperty schema identity or version is incompatible"
						));
				}
				auto mutation = Mutations.Apply(UpdateWirePropertyCommand{
					object->ToObjectId(), PropertyName, std::move(*value)
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
				if (!RequestCursor)
					return SerializeBoundedResponse(ErrorResponse(requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"));
				if (!HasOnlyFields(parameters, {"Object", "Attribute", "Value", "TransactionId", "ExpectedRevision"}) ||
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
				if (HasRevisionConflict())
					return SerializeBoundedResponse(ErrorResponse(requestId, "Conflict",
						"The authoritative project revision changed before attribute mutation"));
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
				if (!RequestCursor)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"
					));
				if (!HasOnlyFields(parameters, {
						"Object", "ExtensionSchemaId", "DefinitionVersion", "Property", "Value", "TransactionId", "ExpectedRevision"
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
				if (HasRevisionConflict())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Conflict", "The authoritative project revision changed before extension mutation"
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
				if (!RequestCursor)
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"
					));
				if (!HasOnlyFields(parameters, {
						"Object", "DeclaringClassSchemaId", "DefinitionVersion", "Property", "Value", "TransactionId", "ExpectedRevision"
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
				if (HasRevisionConflict())
					return SerializeBoundedResponse(ErrorResponse(
						requestId, "Conflict", "The authoritative project revision changed before custom property mutation"
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
				if (!RequestCursor)
					return SerializeBoundedResponse(ErrorResponse(requestId, "SnapshotRequired", "GetSnapshot must establish a cursor"));
				MutationResult Mutation;
				if (method == "CreateInstance") {
					if (!HasOnlyFields(parameters, {"ClassSchemaId", "DefinitionVersion", "Parent", "Name", "InitialProperties", "InitialSource", "TransactionId", "ExpectedRevision"}) ||
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
					std::optional<std::string> InitialSource;
					if (parameters.contains("InitialSource")) {
						if (!parameters["InitialSource"].is_string())
							return SerializeBoundedResponse(ErrorResponse(
								requestId, "MalformedRequest", "CreateInstance InitialSource must be a string"
							));
						InitialSource = parameters["InitialSource"].get<std::string>();
						try {
							ValidateProtocolString(*InitialSource, MaximumScriptSourceBytes, "Initial script source");
						} catch (const std::invalid_argument &Error) {
							return SerializeBoundedResponse(ErrorResponse(requestId, "InvalidSource", Error.what()));
						}
					}
					std::vector<InitialPropertyMutation> InitialProperties;
					if (parameters.contains("InitialProperties")) {
						if (!parameters["InitialProperties"].is_array() ||
							parameters["InitialProperties"].size() > MaximumCreateInitialProperties)
							return SerializeBoundedResponse(ErrorResponse(
								requestId, "ResourceLimit", "Create initial-property count exceeds its bound"
							));
						for (const auto &Initial : parameters["InitialProperties"]) {
							if (!Initial.is_object() || !HasOnlyFields(Initial, {
									"DeclaringClassSchemaId", "DeclaringDefinitionVersion", "Property", "Value"
								}) || !Initial.contains("DeclaringClassSchemaId") ||
								!Initial["DeclaringClassSchemaId"].is_string() ||
								!Initial.contains("DeclaringDefinitionVersion") ||
								!Initial.contains("Property") || !Initial["Property"].is_string() ||
								Initial["Property"].get_ref<const std::string &>().empty() ||
								Initial["Property"].get_ref<const std::string &>().size() > 256 ||
								!Initial.contains("Value"))
								return SerializeBoundedResponse(ErrorResponse(
									requestId, "MalformedRequest", "Create initial property is malformed"
								));
							const auto EncodedDeclaring = Initial["DeclaringClassSchemaId"].get<std::string>();
							auto Declaring = SchemaId::Parse(EncodedDeclaring);
							auto DeclaringVersion = JsonCodec::DecodeUnsigned32(Initial["DeclaringDefinitionVersion"]);
							auto Value = JsonCodec::DecodeWireValue(Initial["Value"]);
							if (!Declaring || Declaring->ToString() != EncodedDeclaring || !DeclaringVersion ||
								*DeclaringVersion == 0 || !Value)
								return SerializeBoundedResponse(ErrorResponse(
									requestId, "MalformedRequest", "Create initial property identity or value is invalid"
								));
							InitialProperties.push_back({
								*Declaring,
								*DeclaringVersion,
								Initial["Property"].get<std::string>(),
								std::move(*Value),
							});
						}
					}
					if (HasRevisionConflict())
						return SerializeBoundedResponse(ErrorResponse(
							requestId, "Conflict", "The authoritative project revision changed before create"
						));
					Mutation = Mutations.Apply(CreateObjectCommand{
						*ClassId, *Version, Parent->ToObjectId(), std::move(Name), std::move(InitialProperties),
						std::move(InitialSource)
					},
						StudioMutationAuthority());
				} else {
					const bool Reparent = method == "ReparentInstance";
					if (!HasOnlyFields(parameters, Reparent ? std::initializer_list<std::string_view>{"Object", "Parent", "TransactionId", "ExpectedRevision"}
						: std::initializer_list<std::string_view>{"Object", "TransactionId", "ExpectedRevision"}) || !parameters.contains("Object") ||
						(Reparent && !parameters.contains("Parent")))
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Structural identity fields are invalid"));
					auto Object = JsonCodec::DecodeObjectId(parameters["Object"]);
					auto Parent = Reparent ? JsonCodec::DecodeObjectId(parameters["Parent"]) : std::optional<WireObjectId>{};
					if (!Object || (Reparent && !Parent))
						return SerializeBoundedResponse(ErrorResponse(requestId, "MalformedRequest", "Structural ObjectId is invalid"));
					if (HasRevisionConflict())
						return SerializeBoundedResponse(ErrorResponse(
							requestId, "Conflict", "The authoritative project revision changed before structural mutation"
						));
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
				if (!RequestCursor)
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

	int EditorHost::Run(std::istream &input, std::ostream &output, std::function<void()> ProcessObserver) {
		struct PendingLine {
			LineReadStatus Status = LineReadStatus::Complete;
			std::string Value;
		};
		struct RequestQueue {
			std::mutex Mutex;
			std::condition_variable Changed;
			std::condition_variable SpaceAvailable;
			std::deque<PendingLine> Lines;
			bool End = false;
			bool Stop = false;
		};
		constexpr std::size_t MaximumQueuedRequests = 64;
		auto Queue = std::make_shared<RequestQueue>();
		std::thread Reader([Queue, &input] {
			for (;;) {
				PendingLine Pending;
				Pending.Status = ReadBoundedLine(input, Pending.Value);
				std::unique_lock Lock(Queue->Mutex);
				Queue->SpaceAvailable.wait(Lock, [&] {
					return Queue->Stop || Queue->Lines.size() < MaximumQueuedRequests;
				});
				if (Queue->Stop) return;
				if (Pending.Status == LineReadStatus::End) {
					Queue->End = true;
					Lock.unlock();
					Queue->Changed.notify_one();
					return;
				}
				Queue->Lines.push_back(std::move(Pending));
				Lock.unlock();
				Queue->Changed.notify_one();
			}
		});
		auto NextObserver = std::chrono::steady_clock::now();
		for (;;) {
			std::optional<PendingLine> Pending;
			{
				std::unique_lock Lock(Queue->Mutex);
				auto Wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
				if (ViewportPresentationActive && NextViewportFrame != std::chrono::steady_clock::time_point{})
					Wake = std::min(Wake, NextViewportFrame);
				if (ActivePlaySession && NextPlayStep != std::chrono::steady_clock::time_point{})
					Wake = std::min(Wake, NextPlayStep);
				if (TerrainAuthoring && TerrainAuthoring->Pending)
					Wake = std::min(Wake, std::chrono::steady_clock::now() + std::chrono::milliseconds(16));
				Queue->Changed.wait_until(Lock, Wake, [&] { return Queue->End || !Queue->Lines.empty(); });
				if (!Queue->Lines.empty()) {
					Pending = std::move(Queue->Lines.front());
					Queue->Lines.pop_front();
					Queue->SpaceAvailable.notify_one();
				} else if (Queue->End) {
					Queue->Stop = true;
					Lock.unlock();
					Queue->SpaceAvailable.notify_all();
					Reader.join();
					return 0;
				}
			}

			PumpPlaySession();
			PumpViewportPresentation();
			if (Pending) {
				const auto Response = Pending->Status == LineReadStatus::Oversized
					? SerializeBoundedResponse(ErrorResponse(nullptr, "RequestTooLarge", "Request exceeded its byte limit"))
					: HandleRequest(Pending->Value);
				output << EditorHostResponsePrefix << Response << '\n';
				output.flush();
				if (!output) {
					{
						std::scoped_lock Lock(Queue->Mutex);
						Queue->Stop = true;
					}
					Queue->SpaceAvailable.notify_all();
					Reader.detach();
					return 1;
				}
			}
			PumpTerrainAuthoring();
			const auto Now = std::chrono::steady_clock::now();
			if (ProcessObserver && Now >= NextObserver) {
				try {
					ProcessObserver();
				} catch (...) {
					// Process observers are optional and cannot affect the protocol loop.
				}
				NextObserver = Now + std::chrono::milliseconds(100);
			}
		}
	}
}
