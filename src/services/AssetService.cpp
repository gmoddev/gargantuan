// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/services/AssetService.hpp"

#include "assets/AssetImporter.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/ImageLabel.hpp"
#include "gargantuan/classes/TextLabel.hpp"
#include "gargantuan/filesystem/BaseFilesystem.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/runtime/JobSystem.hpp"
#include "serialization/JsonCodec.hpp"

#include <lua.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

namespace gargantuan {
	namespace {
		using Json = nlohmann::ordered_json;
		constexpr std::uint32_t CatalogVersion = 1;
		constexpr std::string_view CatalogPath = ".gargantuan/assets/catalog.json";
		constexpr std::string_view ArtifactDirectory = ".gargantuan/assets/artifacts";
		constexpr std::string_view DefaultFontReference = "builtin://font/default";
		constexpr std::string_view MissingImageReference = "builtin://image/missing";

		AssetDiagnostic Error(std::string Code, std::string Message) {
			if (Message.size() > AssetLimits::MaximumDiagnosticBytes) Message.resize(AssetLimits::MaximumDiagnosticBytes);
			return {std::move(Code), std::move(Message)};
		}

		std::string LowerExtension(const std::string &Source) {
			auto Extension = std::filesystem::path(Source).extension().string();
			std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			return Extension;
		}

		std::string DefaultName(const std::string &Source) {
			auto Name = std::filesystem::path(Source).stem().string();
			if (Name.empty()) Name = "Asset";
			if (Name.size() > AssetLimits::MaximumNameBytes) Name.resize(AssetLimits::MaximumNameBytes);
			return Name;
		}

		std::size_t AssetBytes(const ImportedAsset &Asset) {
			return std::visit([](const auto &Value) -> std::size_t {
				using T = std::decay_t<decltype(Value)>;
				if constexpr (std::is_same_v<T, ImportedImage>)
					return Value.Rgba8 ? Value.Rgba8->size() : 0;
				else if constexpr (std::is_same_v<T, ImportedFont>)
					return Value.Bytes ? Value.Bytes->size() : 0;
				else
					return (Value.Vertices ? Value.Vertices->size() * sizeof(RenderVertex) : 0) +
						(Value.Indices ? Value.Indices->size() * sizeof(std::uint32_t) : 0);
			}, Asset);
		}

		RenderTextureIdentity TextureIdentity(AssetId Id, std::uint32_t Generation = 1) {
			auto Slot = 0x4153535400000000ull ^ Id.High ^ std::rotl(Id.Low, 17);
			if (Slot == 0) Slot = 0x4153535400000001ull;
			return {Slot, Generation};
		}

		RenderMeshIdentity MeshIdentity(AssetId Id, std::uint32_t Generation = 1) {
			auto Slot = 0x4153534d00000000ull ^ Id.High ^ std::rotl(Id.Low, 29);
			if (Slot == 0) Slot = 0x4153534d00000001ull;
			return {Slot, Generation};
		}

		bool IsAvailableRecord(const AssetRecord &Record) {
			return Record.Asset && (Record.State == AssetState::Ready || Record.State == AssetState::Stale);
		}

		std::string ArtifactPath(const AssetContentId &Id) {
			return std::string(ArtifactDirectory) + "/" + Id.ToString() + ".gasset";
		}

		void PushStringField(lua_State *L, const char *Name, std::string_view Value) {
			lua_pushlstring(L, Value.data(), Value.size());
			lua_setfield(L, -2, Name);
		}

		AssetService &CheckedService(Instance *InstanceValue) {
			auto *Service = dynamic_cast<AssetService *>(InstanceValue);
			if (!Service) throw std::invalid_argument("AssetService receiver is invalid");
			return *Service;
		}

		std::string_view ReadReference(lua_State *L, int Index) {
			size_t Length = 0;
			const auto *Value = luaL_checklstring(L, Index, &Length);
			if (Length > 256) throw std::invalid_argument("Asset reference exceeds 256 bytes");
			return {Value, Length};
		}
	}

	struct AssetService::Impl {
		struct TextureResidency {
			RenderTextureIdentity Identity;
			std::uint64_t Revision = 0;
			std::uint32_t Width = 0;
			std::uint32_t Height = 0;
			std::shared_ptr<const std::vector<std::uint8_t>> Pixels;
		};

		struct MeshResidency {
			RenderMeshIdentity Identity;
			std::uint64_t Revision = 0;
			ImportedMesh Mesh;
		};

		mutable std::mutex Mutex;
		JobSystem Jobs{2};
		std::vector<std::unique_ptr<IAssetImporter>> Importers = CreateFoundationAssetImporters();
		std::unordered_map<std::string, AssetRecord> Records;
		std::unordered_map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> Artifacts;
		std::unordered_map<std::string, TextureResidency> Textures;
		std::unordered_map<std::string, MeshResidency> Meshes;
		AssetTextureChanges PendingTextures;
		AssetMeshChanges PendingMeshes;
		std::deque<AssetChange> Changes;
		std::uint64_t NextChangeSequence = 1;
		std::size_t CpuBytes = 0;
		std::size_t InFlight = 0;

		Impl() { RegisterMissingImage(); }

		void RegisterMissingImage() {
			static constexpr std::array<std::uint8_t, 16> Pixels{
				255, 0, 255, 255, 20, 20, 20, 255, 20, 20, 20, 255, 255, 0, 255, 255,
			};
			const auto Id = AssetId::FromBuiltInName(MissingImageReference);
			auto PixelBytes = std::make_shared<const std::vector<std::uint8_t>>(Pixels.begin(), Pixels.end());
			auto Asset = std::make_shared<const ImportedAsset>(ImportedImage{2, 2, PixelBytes});
			AssetRecord Record{Id, *AssetReference::Parse(MissingImageReference), AssetKind::Image, "Missing Image",
				"engine-package", {}, 1, AssetState::Ready, std::nullopt, {}, Asset, true};
			Records.emplace(Record.Reference.Value, Record);
			TextureResidency Residency{TextureIdentity(Id), 1, 2, 2, PixelBytes};
			Textures.emplace(Record.Reference.Value, Residency);
			CpuBytes += PixelBytes->size();
			PendingTextures.Creates.push_back({Residency.Identity, 1, 2, 2, RenderTextureFormat::Rgba8Unorm, PixelBytes});
			PendingTextures.UploadBytes += PixelBytes->size();
		}

		void PublishChange(const AssetRecord &Record) {
			if (NextChangeSequence == 0) throw std::overflow_error("Asset change sequence is exhausted");
			if (Changes.size() == AssetLimits::MaximumChangeRecords) Changes.pop_front();
			Changes.push_back({NextChangeSequence++, Record.Reference.Value, Record.Kind, Record.ContentRevision, Record.State});
		}

		IAssetImporter *FindImporter(AssetKind Kind, std::string_view Extension) const {
			for (const auto &Importer : Importers)
				if (Importer->GetKind() == Kind && Importer->SupportsExtension(Extension)) return Importer.get();
			return nullptr;
		}

		std::optional<AssetKind> DetectKind(std::string_view Extension) const {
			std::optional<AssetKind> Result;
			for (const auto &Importer : Importers) if (Importer->SupportsExtension(Extension)) {
				if (Result && *Result != Importer->GetKind()) return std::nullopt;
				Result = Importer->GetKind();
			}
			return Result;
		}

		std::expected<AssetImportCandidate, AssetDiagnostic> Decode(
			AssetKind Kind,
			std::string Extension,
			std::shared_ptr<const std::vector<std::uint8_t>> Source,
			const AssetCancellationToken &Cancellation
		) {
			auto *Importer = FindImporter(Kind, Extension);
			if (!Importer) return std::unexpected(Error("UnsupportedFormat", "No bounded importer supports the requested kind and extension"));
			std::expected<AssetImportCandidate, AssetDiagnostic> Result = std::unexpected(Error("InternalFailure", "Import job did not run"));
			auto Group = std::make_shared<JobGroup>();
			Jobs.Submit([&Result, Importer, Kind, Extension = std::move(Extension), Source = std::move(Source), Cancellation] {
				try {
					Result = Importer->Import(*Source, {Kind, Extension, Cancellation, std::chrono::steady_clock::now() + std::chrono::seconds(2)});
				} catch (const std::exception &Failure) {
					Result = std::unexpected(Error("ImporterFailure", Failure.what()));
				} catch (...) {
					Result = std::unexpected(Error("ImporterFailure", "Importer failed with a non-standard exception"));
				}
			}, Group);
			Group->Wait();
			if (auto Failure = Group->GetFirstException()) {
				try { std::rethrow_exception(Failure); }
				catch (const std::exception &Exception) { return std::unexpected(Error("ImporterFailure", Exception.what())); }
				catch (...) { return std::unexpected(Error("ImporterFailure", "Importer worker failed")); }
			}
			return Result;
		}

		void QueueResidency(const AssetRecord &Record, bool Replacing) {
			if (!Record.Asset) return;
			if (const auto *Image = std::get_if<ImportedImage>(Record.Asset.get())) {
				auto Existing = Textures.find(Record.Reference.Value);
				if (Existing == Textures.end()) {
					TextureResidency Residency{TextureIdentity(Record.Id), Record.ContentRevision, Image->Width, Image->Height, Image->Rgba8};
					Textures.emplace(Record.Reference.Value, Residency);
					PendingTextures.Creates.push_back({Residency.Identity, Residency.Revision, Residency.Width, Residency.Height,
						RenderTextureFormat::Rgba8Unorm, Residency.Pixels});
					PendingTextures.UploadBytes += Residency.Pixels->size();
					return;
				}
				auto &Residency = Existing->second;
				if (Residency.Width == Image->Width && Residency.Height == Image->Height) {
					Residency.Revision = std::max(Residency.Revision + 1, Record.ContentRevision);
					Residency.Pixels = Image->Rgba8;
					PendingTextures.Updates.push_back({Residency.Identity, Residency.Revision, 0, 0,
						Image->Width, Image->Height, Image->Rgba8});
					PendingTextures.UploadBytes += Image->Rgba8->size();
					return;
				}
				PendingTextures.Removes.push_back({Residency.Identity});
				++Residency.Identity.Generation;
				Residency.Revision = 1;
				Residency.Width = Image->Width;
				Residency.Height = Image->Height;
				Residency.Pixels = Image->Rgba8;
				PendingTextures.Creates.push_back({Residency.Identity, 1, Image->Width, Image->Height,
					RenderTextureFormat::Rgba8Unorm, Image->Rgba8});
				PendingTextures.UploadBytes += Image->Rgba8->size();
				return;
			}
			if (const auto *Mesh = std::get_if<ImportedMesh>(Record.Asset.get())) {
				auto Existing = Meshes.find(Record.Reference.Value);
				if (Existing != Meshes.end() && Replacing) {
					PendingMeshes.Removes.push_back({Existing->second.Identity});
					++Existing->second.Identity.Generation;
					Existing->second.Revision = Record.ContentRevision;
					Existing->second.Mesh = *Mesh;
				} else if (Existing == Meshes.end()) {
					Existing = Meshes.emplace(Record.Reference.Value, MeshResidency{MeshIdentity(Record.Id), Record.ContentRevision, *Mesh}).first;
				}
				const auto &Residency = Existing->second;
				PendingMeshes.Creates.push_back({Residency.Identity, Residency.Revision, Residency.Revision,
					Residency.Mesh.Vertices, Residency.Mesh.Indices, Residency.Mesh.Bounds});
			}
		}

		void RemoveResidency(const AssetRecord &Record) {
			if (auto Texture = Textures.find(Record.Reference.Value); Texture != Textures.end()) {
				PendingTextures.Removes.push_back({Texture->second.Identity});
				Textures.erase(Texture);
			}
			if (auto Mesh = Meshes.find(Record.Reference.Value); Mesh != Meshes.end()) {
				PendingMeshes.Removes.push_back({Mesh->second.Identity});
				Meshes.erase(Mesh);
			}
		}

		using ArtifactReader = std::function<std::expected<
			std::shared_ptr<const std::vector<std::uint8_t>>, AssetDiagnostic
		>(const AssetContentId &)>;

		void LoadCatalog(std::string_view Text, const ArtifactReader &ReadArtifact) {
			auto Parsed = JsonCodec::Parse(Text, 1024 * 1024, "asset catalog");
			if (!Parsed || !Parsed->is_object() || Parsed->size() != 2 ||
				Parsed->value("Version", 0u) != CatalogVersion || !Parsed->contains("Assets") ||
				!(*Parsed)["Assets"].is_array() || (*Parsed)["Assets"].size() > AssetLimits::MaximumCatalogRecords)
				throw std::runtime_error("Asset catalog format is invalid or unsupported");
			std::scoped_lock StateLock(Mutex);
			for (const auto &[Reference, Record] : Records) {
				(void)Reference;
				if (!Record.BuiltIn) throw std::logic_error("Project assets are already loaded");
			}

			std::unordered_set<std::string> Seen;
			for (const auto &Encoded : (*Parsed)["Assets"]) {
				if (!Encoded.is_object() || Encoded.size() != 9 || !Encoded.contains("AssetId") || !Encoded["AssetId"].is_string() ||
					!Encoded.contains("Reference") || !Encoded["Reference"].is_string() ||
					!Encoded.contains("Kind") || !Encoded["Kind"].is_string() ||
					!Encoded.contains("Name") || !Encoded["Name"].is_string() ||
					!Encoded.contains("Source") || !Encoded["Source"].is_string() ||
					!Encoded.contains("ContentId") || (!Encoded["ContentId"].is_null() && !Encoded["ContentId"].is_string()) ||
					!Encoded.contains("ContentRevision") || !Encoded["ContentRevision"].is_number_unsigned() ||
					!Encoded.contains("State") || !Encoded["State"].is_string() || !Encoded.contains("Diagnostic"))
					throw std::runtime_error("Asset catalog record is malformed");
				auto Id = AssetId::Parse(Encoded["AssetId"].get_ref<const std::string &>());
				auto Reference = AssetReference::Parse(Encoded["Reference"].get_ref<const std::string &>());
				auto Kind = ParseAssetKind(Encoded["Kind"].get_ref<const std::string &>());
				std::optional<AssetContentId> ContentId;
				if (Encoded["ContentId"].is_string())
					ContentId = AssetContentId::Parse(Encoded["ContentId"].get_ref<const std::string &>());
				auto SavedState = ParseAssetState(Encoded["State"].get_ref<const std::string &>());
				const auto Revision = Encoded["ContentRevision"].get<std::uint64_t>();
				const auto Name = Encoded["Name"].get<std::string>();
				const auto Source = Encoded["Source"].get<std::string>();
				const bool HasCanonicalContent = ContentId.has_value() && Revision != 0;
				const bool HasFailedContent = !ContentId && Revision == 0 && SavedState &&
					(*SavedState == AssetState::Failed || *SavedState == AssetState::Missing);
				if (!Id || !Reference || Reference->BuiltIn || Reference->ProjectAsset != Id || !Kind || !SavedState ||
					(!HasCanonicalContent && !HasFailedContent) || *SavedState == AssetState::Importing ||
					Name.empty() || Name.size() > AssetLimits::MaximumNameBytes ||
					Source.empty() || Source.size() > AssetLimits::MaximumSourcePathBytes || !Seen.insert(Reference->Value).second)
					throw std::runtime_error("Asset catalog record identity or bounds are invalid");

				AssetRecord Record{*Id, *Reference, *Kind, Name, Source, ContentId.value_or(AssetContentId{}), Revision, AssetState::Missing};
				if (!Encoded["Diagnostic"].is_null()) {
					if (!Encoded["Diagnostic"].is_object() || Encoded["Diagnostic"].size() != 2 ||
						!Encoded["Diagnostic"].contains("Code") || !Encoded["Diagnostic"]["Code"].is_string() ||
						!Encoded["Diagnostic"].contains("Message") || !Encoded["Diagnostic"]["Message"].is_string())
						throw std::runtime_error("Asset catalog diagnostic is malformed");
					Record.Diagnostic = Error(
						Encoded["Diagnostic"]["Code"].get<std::string>(),
						Encoded["Diagnostic"]["Message"].get<std::string>()
					);
				}
				if (!ContentId) {
					if (!Record.Diagnostic) throw std::runtime_error("Failed asset catalog record has no diagnostic");
					Record.State = *SavedState;
					Records.emplace(Record.Reference.Value, std::move(Record));
					continue;
				}

				auto Artifact = ReadArtifact(*ContentId);
				if (!Artifact) {
					Record.State = AssetState::Missing;
					Record.Diagnostic = Artifact.error();
				} else {
					auto Candidate = DecodeAssetArtifact(**Artifact, *Kind, *ContentId);
					if (!Candidate) {
						Record.State = AssetState::Failed;
						Record.Diagnostic = Candidate.error();
					} else {
						const auto BytesUsed = AssetBytes(Candidate->Asset);
						if (CpuBytes > AssetLimits::MaximumCpuCacheBytes - std::min(BytesUsed, AssetLimits::MaximumCpuCacheBytes)) {
							Record.State = AssetState::Failed;
							Record.Diagnostic = Error("CacheLimit", "Loaded asset would exceed the bounded CPU cache");
						} else {
							Record.Asset = std::make_shared<const ImportedAsset>(std::move(Candidate->Asset));
							Record.State = *SavedState == AssetState::Stale ? AssetState::Stale : AssetState::Ready;
							CpuBytes += BytesUsed;
							Artifacts.insert_or_assign(ContentId->ToString(), *Artifact);
						}
					}
				}
				Records.emplace(Record.Reference.Value, Record);
				if (Record.Asset) QueueResidency(Record, false);
			}
		}
	};

	AssetService::AssetService() : State(std::make_unique<Impl>()) {}
	AssetService::~AssetService() = default;

	AssetOperationResult AssetService::ImportProjectAsset(
		SourceMount &Mount,
		std::string Source,
		std::optional<AssetKind> RequestedKind,
		std::string Name,
		const AssetCancellationToken &Cancellation
	) {
		if (Source.empty() || Source.size() > AssetLimits::MaximumSourcePathBytes)
			return {false, std::nullopt, Error("InvalidSource", "Asset source path is empty or oversized")};
		const auto Extension = LowerExtension(Source);
		auto Kind = RequestedKind ? RequestedKind : State->DetectKind(Extension);
		if (!Kind) return {false, std::nullopt, Error("UnsupportedFormat", "Asset source type cannot be determined unambiguously")};
		if (!State->FindImporter(*Kind, Extension))
			return {false, std::nullopt, Error("UnsupportedFormat", "Requested asset kind does not match a supported source extension")};
		if (Name.empty()) Name = DefaultName(Source);
		if (Name.empty() || Name.size() > AssetLimits::MaximumNameBytes)
			return {false, std::nullopt, Error("InvalidName", "Asset display name is empty or oversized")};

		AssetId Id;
		AssetRecord Initial;
		{
			std::scoped_lock Lock(State->Mutex);
			if (State->Records.size() >= AssetLimits::MaximumCatalogRecords)
				return {false, std::nullopt, Error("CatalogLimit", "Asset catalog record limit is reached")};
			if (State->InFlight >= AssetLimits::MaximumInFlightImports)
				return {false, std::nullopt, Error("ImportQueueFull", "Asset import concurrency limit is reached")};
			do Id = AssetId::New(); while (State->Records.contains(AssetReference::FromAssetId(Id).Value));
			Initial = {Id, AssetReference::FromAssetId(Id), *Kind, Name, Source, {}, 0, AssetState::Importing};
			State->Records.emplace(Initial.Reference.Value, Initial);
			++State->InFlight;
			State->PublishChange(Initial);
		}

		SourceMountBudget Budget;
		auto Read = Mount.ReadFile(std::filesystem::path(Source), AssetLimits::MaximumSourceBytes, Budget);
		std::expected<AssetImportCandidate, AssetDiagnostic> Candidate = std::unexpected(Error("Missing", "Asset source is unavailable"));
		if (!Read) Candidate = std::unexpected(Error(std::string(GetSourceMountErrorCodeName(Read.error().Code)), Read.error().Format()));
		else {
			auto Bytes = std::make_shared<const std::vector<std::uint8_t>>(Read->begin(), Read->end());
			Candidate = State->Decode(*Kind, Extension, Bytes, Cancellation);
		}

		AssetRecord Result;
		bool Changed = false;
		{
			std::scoped_lock Lock(State->Mutex);
			--State->InFlight;
			auto &Record = State->Records.at(Initial.Reference.Value);
			if (!Candidate) {
				Record.State = AssetState::Failed;
				Record.Diagnostic = Candidate.error();
				State->PublishChange(Record);
				Result = Record;
			} else {
				const auto Bytes = AssetBytes(Candidate->Asset);
				if (Bytes > AssetLimits::MaximumCpuCacheBytes || State->CpuBytes > AssetLimits::MaximumCpuCacheBytes - Bytes) {
					Record.State = AssetState::Failed;
					Record.Diagnostic = Error("CacheLimit", "Canonical asset would exceed the bounded CPU cache");
					State->PublishChange(Record);
					Result = Record;
				} else {
					Record.ContentId = Candidate->ContentId;
					Record.ContentRevision = 1;
					Record.State = AssetState::Ready;
					Record.Diagnostic.reset();
					Record.Asset = std::make_shared<const ImportedAsset>(std::move(Candidate->Asset));
					State->Artifacts.insert_or_assign(Record.ContentId.ToString(), Candidate->Artifact);
					State->CpuBytes += Bytes;
					State->QueueResidency(Record, false);
					State->PublishChange(Record);
					Result = Record;
					Changed = true;
				}
			}
		}
		if (auto World = GetDataModel(); World && (Changed || Result.State == AssetState::Failed)) World->AdvanceAuthoritativeRevision();
		return {Result.State == AssetState::Ready, Result, Result.Diagnostic.value_or(AssetDiagnostic{})};
	}

	AssetOperationResult AssetService::ReimportProjectAsset(
		SourceMount &Mount,
		std::string_view Reference,
		const AssetCancellationToken &Cancellation
	) {
		AssetRecord Previous;
		{
			std::scoped_lock Lock(State->Mutex);
			auto Existing = State->Records.find(std::string(Reference));
			if (Existing == State->Records.end() || Existing->second.BuiltIn)
				return {false, std::nullopt, Error("UnknownAsset", "Project asset reference is unknown")};
			if (State->InFlight >= AssetLimits::MaximumInFlightImports)
				return {false, Existing->second, Error("ImportQueueFull", "Asset import concurrency limit is reached")};
			Previous = Existing->second;
			Existing->second.State = AssetState::Importing;
			++State->InFlight;
			State->PublishChange(Existing->second);
		}

		SourceMountBudget Budget;
		auto Read = Mount.ReadFile(std::filesystem::path(Previous.Source), AssetLimits::MaximumSourceBytes, Budget);
		std::expected<AssetImportCandidate, AssetDiagnostic> Candidate = std::unexpected(Error("Missing", "Asset source is unavailable"));
		if (!Read) Candidate = std::unexpected(Error(std::string(GetSourceMountErrorCodeName(Read.error().Code)), Read.error().Format()));
		else {
			auto Bytes = std::make_shared<const std::vector<std::uint8_t>>(Read->begin(), Read->end());
			Candidate = State->Decode(Previous.Kind, LowerExtension(Previous.Source), Bytes, Cancellation);
		}

		AssetRecord Result;
		{
			std::scoped_lock Lock(State->Mutex);
			--State->InFlight;
			auto &Record = State->Records.at(std::string(Reference));
			if (!Candidate) {
				Record = Previous;
				Record.State = Record.Asset ? AssetState::Stale : AssetState::Failed;
				Record.Diagnostic = Candidate.error();
				State->PublishChange(Record);
				Result = Record;
			} else if (Candidate->ContentId == Previous.ContentId) {
				Record = Previous;
				Record.State = AssetState::Ready;
				Record.Diagnostic.reset();
				State->PublishChange(Record);
				Result = Record;
			} else {
				const auto PreviousBytes = Previous.Asset ? AssetBytes(*Previous.Asset) : 0;
				const auto NextBytes = AssetBytes(Candidate->Asset);
				if (NextBytes > AssetLimits::MaximumCpuCacheBytes ||
					State->CpuBytes - PreviousBytes > AssetLimits::MaximumCpuCacheBytes - NextBytes) {
					Record = Previous;
					Record.State = AssetState::Stale;
					Record.Diagnostic = Error("CacheLimit", "Reimport candidate would exceed the bounded CPU cache");
				} else {
					Record = Previous;
					Record.ContentId = Candidate->ContentId;
					++Record.ContentRevision;
					Record.State = AssetState::Ready;
					Record.Diagnostic.reset();
					Record.Asset = std::make_shared<const ImportedAsset>(std::move(Candidate->Asset));
					State->Artifacts.insert_or_assign(Record.ContentId.ToString(), Candidate->Artifact);
					State->CpuBytes = State->CpuBytes - PreviousBytes + NextBytes;
					State->QueueResidency(Record, true);
				}
				State->PublishChange(Record);
				Result = Record;
			}
		}
		if (auto World = GetDataModel(); World) World->AdvanceAuthoritativeRevision();
		return {Result.State == AssetState::Ready, Result, Result.Diagnostic.value_or(AssetDiagnostic{})};
	}

	AssetOperationResult AssetService::DeleteProjectAsset(std::string_view Reference) {
		AssetRecord Removed;
		{
			std::scoped_lock Lock(State->Mutex);
			auto Existing = State->Records.find(std::string(Reference));
			if (Existing == State->Records.end() || Existing->second.BuiltIn)
				return {false, std::nullopt, Error("UnknownAsset", "Project asset reference is unknown")};
			if (auto World = GetDataModel()) {
				for (const auto &Object : World->GetDescendants()) {
					if (auto Image = std::dynamic_pointer_cast<ImageLabel>(Object); Image && Image->GetImage() == Reference)
						return {false, Existing->second, Error("AssetReferenced", "Asset is referenced by ImageLabel")};
					if (auto Text = std::dynamic_pointer_cast<TextLabel>(Object); Text && Text->GetFontFace() == Reference)
						return {false, Existing->second, Error("AssetReferenced", "Asset is referenced by a text object")};
				}
			}
			Removed = Existing->second;
			if (Removed.Asset) State->CpuBytes -= AssetBytes(*Removed.Asset);
			State->RemoveResidency(Removed);
			State->Records.erase(Existing);
			Removed.State = AssetState::Missing;
			State->PublishChange(Removed);
		}
		if (auto World = GetDataModel()) World->AdvanceAuthoritativeRevision();
		return {true, Removed, {}};
	}

	std::optional<AssetRecord> AssetService::GetAsset(std::string_view Reference) const {
		std::scoped_lock Lock(State->Mutex);
		auto Existing = State->Records.find(std::string(Reference));
		return Existing == State->Records.end() ? std::nullopt : std::optional(Existing->second);
	}

	std::vector<AssetRecord> AssetService::GetCatalog(bool IncludeBuiltIns) const {
		std::scoped_lock Lock(State->Mutex);
		std::vector<AssetRecord> Result;
		Result.reserve(State->Records.size());
		for (const auto &[Reference, Record] : State->Records) {
			(void)Reference;
			if (IncludeBuiltIns || !Record.BuiltIn) Result.push_back(Record);
		}
		std::ranges::sort(Result, {}, [](const AssetRecord &Record) { return Record.Reference.Value; });
		return Result;
	}

	bool AssetService::IsAvailable(std::string_view Reference) const {
		auto Record = GetAsset(Reference);
		return Record && IsAvailableRecord(*Record);
	}

	std::optional<AssetImageResource> AssetService::ResolveImage(std::string_view Reference) {
		std::scoped_lock Lock(State->Mutex);
		auto Existing = State->Records.find(std::string(Reference));
		if (Existing == State->Records.end() || !IsAvailableRecord(Existing->second) ||
			!std::holds_alternative<ImportedImage>(*Existing->second.Asset)) Existing = State->Records.find(std::string(MissingImageReference));
		if (Existing == State->Records.end()) return std::nullopt;
		auto Residency = State->Textures.find(Existing->first);
		if (Residency == State->Textures.end()) return std::nullopt;
		return AssetImageResource{Residency->second.Identity, Residency->second.Width, Residency->second.Height,
			Existing->second.ContentRevision};
	}

	std::optional<AssetFontResource> AssetService::ResolveFont(std::string_view Reference) const {
		std::scoped_lock Lock(State->Mutex);
		if (Reference.empty()) Reference = DefaultFontReference;
		auto Existing = State->Records.find(std::string(Reference));
		if (Existing == State->Records.end() || !IsAvailableRecord(Existing->second))
			Existing = State->Records.find(std::string(DefaultFontReference));
		if (Existing == State->Records.end() || !Existing->second.Asset) return std::nullopt;
		const auto *Font = std::get_if<ImportedFont>(Existing->second.Asset.get());
		return Font ? std::optional(AssetFontResource{Font->Bytes, Existing->second.ContentRevision, Font->FaceCount}) : std::nullopt;
	}

	std::optional<ImportedMesh> AssetService::ResolveMesh(std::string_view Reference) const {
		auto Record = GetAsset(Reference);
		if (!Record || !IsAvailableRecord(*Record) || !Record->Asset) return std::nullopt;
		const auto *Mesh = std::get_if<ImportedMesh>(Record->Asset.get());
		return Mesh ? std::optional(*Mesh) : std::nullopt;
	}

	AssetTextureChanges AssetService::DrainTextureChanges() {
		std::scoped_lock Lock(State->Mutex);
		auto Result = std::move(State->PendingTextures);
		State->PendingTextures = {};
		return Result;
	}

	AssetMeshChanges AssetService::DrainMeshChanges() {
		std::scoped_lock Lock(State->Mutex);
		auto Result = std::move(State->PendingMeshes);
		State->PendingMeshes = {};
		return Result;
	}

	AssetChangeBatch AssetService::ReadChanges(std::uint64_t Sequence) const {
		std::scoped_lock Lock(State->Mutex);
		AssetChangeBatch Result{State->NextChangeSequence, false, {}};
		if (Sequence == 0 || (!State->Changes.empty() && Sequence < State->Changes.front().Sequence)) {
			Result.RescanRequired = true;
			return Result;
		}
		for (const auto &Change : State->Changes) if (Change.Sequence >= Sequence) Result.Changes.push_back(Change);
		return Result;
	}

	void AssetService::ConfigureBuiltInFont(const std::filesystem::path &Path) {
		std::ifstream Input(Path, std::ios::binary | std::ios::ate);
		if (!Input) return;
		const auto Size = Input.tellg();
		if (Size <= 0 || static_cast<std::uint64_t>(Size) > AssetLimits::MaximumFontBytes) return;
		Input.seekg(0);
		auto Bytes = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(Size));
		Input.read(reinterpret_cast<char *>(Bytes->data()), Size);
		if (!Input) return;
		auto *Importer = State->FindImporter(AssetKind::Font, ".ttf");
		if (!Importer) return;
		auto Candidate = Importer->Import(*Bytes, {AssetKind::Font, ".ttf", {}, std::chrono::steady_clock::now() + std::chrono::seconds(2)});
		if (!Candidate) return;
		std::scoped_lock Lock(State->Mutex);
		const auto Id = AssetId::FromBuiltInName(DefaultFontReference);
		auto Asset = std::make_shared<const ImportedAsset>(std::move(Candidate->Asset));
		AssetRecord Record{Id, *AssetReference::Parse(DefaultFontReference), AssetKind::Font, "Gargantuan Sans",
			"engine-package", Candidate->ContentId, 1, AssetState::Ready, std::nullopt, {}, Asset, true};
		auto Existing = State->Records.find(Record.Reference.Value);
		const auto PreviousBytes = Existing != State->Records.end() && Existing->second.Asset
			? AssetBytes(*Existing->second.Asset) : 0;
		const auto NextBytes = AssetBytes(*Asset);
		if (NextBytes > AssetLimits::MaximumCpuCacheBytes ||
			State->CpuBytes - PreviousBytes > AssetLimits::MaximumCpuCacheBytes - NextBytes) return;
		if (Existing != State->Records.end()) {
			if (Existing->second.ContentId == Record.ContentId) return;
			Record.ContentRevision = Existing->second.ContentRevision + 1;
			State->CpuBytes -= PreviousBytes;
			Existing->second = Record;
		} else State->Records.emplace(Record.Reference.Value, Record);
		State->CpuBytes += NextBytes;
		State->PublishChange(Record);
	}

	std::string AssetService::RegisterMemoryImage(
		std::string Name,
		std::uint32_t Width,
		std::uint32_t Height,
		std::span<const std::uint8_t> Rgba8
	) {
		if (Name.empty() || Name.size() > AssetLimits::MaximumNameBytes || Width == 0 || Height == 0 ||
			Width > AssetLimits::MaximumImageDimension || Height > AssetLimits::MaximumImageDimension ||
			Rgba8.size() != static_cast<std::size_t>(Width) * Height * 4)
			throw std::invalid_argument("Memory image does not satisfy AssetService limits");
		std::vector<std::uint8_t> IdentityBytes;
		IdentityBytes.reserve(Rgba8.size() + 8);
		for (std::size_t Shift = 0; Shift < 4; ++Shift)
			IdentityBytes.push_back(static_cast<std::uint8_t>(Width >> (Shift * 8)));
		for (std::size_t Shift = 0; Shift < 4; ++Shift)
			IdentityBytes.push_back(static_cast<std::uint8_t>(Height >> (Shift * 8)));
		IdentityBytes.insert(IdentityBytes.end(), Rgba8.begin(), Rgba8.end());
		const auto ContentId = AssetContentId::Hash(IdentityBytes);
		auto Pixels = std::make_shared<const std::vector<std::uint8_t>>(Rgba8.begin(), Rgba8.end());
		auto Asset = std::make_shared<const ImportedAsset>(ImportedImage{Width, Height, Pixels});
		std::scoped_lock Lock(State->Mutex);
		for (auto &[Reference, Existing] : State->Records) {
			if (!Existing.BuiltIn || Existing.Source != "memory" || Existing.Name != Name) continue;
			if (Existing.ContentId == ContentId) return Reference;
			const auto PreviousBytes = AssetBytes(*Existing.Asset);
			if (Pixels->size() > AssetLimits::MaximumCpuCacheBytes ||
				State->CpuBytes - PreviousBytes > AssetLimits::MaximumCpuCacheBytes - Pixels->size())
				throw std::length_error("Memory image would exceed the bounded AssetService CPU cache");
			State->CpuBytes -= PreviousBytes;
			Existing.ContentId = ContentId;
			++Existing.ContentRevision;
			Existing.Asset = Asset;
			State->CpuBytes += Pixels->size();
			State->QueueResidency(Existing, true);
			State->PublishChange(Existing);
			return Reference;
		}
		if (Pixels->size() > AssetLimits::MaximumCpuCacheBytes ||
			State->CpuBytes > AssetLimits::MaximumCpuCacheBytes - Pixels->size())
			throw std::length_error("Memory image would exceed the bounded AssetService CPU cache");
		const auto Id = AssetId::New();
		AssetRecord Record{Id, AssetReference::FromAssetId(Id), AssetKind::Image, std::move(Name), "memory", ContentId, 1,
			AssetState::Ready, std::nullopt, {}, Asset, true};
		State->Records.emplace(Record.Reference.Value, Record);
		State->CpuBytes += Pixels->size();
		State->QueueResidency(Record, false);
		State->PublishChange(Record);
		return Record.Reference.Value;
	}

	void AssetService::LoadProjectAssets(BaseFilesystem &Filesystem) {
		if (!Filesystem.Exists(Filesystem.Root / std::filesystem::path(CatalogPath))) return;
		auto Metadata = Filesystem.Metadata(Filesystem.Root / std::filesystem::path(CatalogPath));
		if (Metadata.Type != FileType::File || Metadata.Size > 1024 * 1024) throw std::runtime_error("Asset catalog exceeds its byte limit");
		auto Text = Filesystem.ReadFileToString(Filesystem.Root / std::filesystem::path(CatalogPath));
		SourceMount Mount(Filesystem);
		SourceMountBudget Budget;
		State->LoadCatalog(Text, [&](const AssetContentId &ContentId) -> std::expected<
			std::shared_ptr<const std::vector<std::uint8_t>>, AssetDiagnostic
		> {
			auto Artifact = Mount.ReadFile(
				std::filesystem::path(ArtifactPath(ContentId)), AssetLimits::MaximumArtifactBytes, Budget
			);
			if (!Artifact) return std::unexpected(Error(
				std::string(GetSourceMountErrorCodeName(Artifact.error().Code)), Artifact.error().Format()
			));
			return std::make_shared<const std::vector<std::uint8_t>>(Artifact->begin(), Artifact->end());
		});
	}

	void AssetService::LoadProjectAssetSnapshot(const AssetProjectSnapshot &Snapshot) {
		if (Snapshot.CatalogJson.empty()) return;
		if (Snapshot.CatalogJson.size() > 1024 * 1024)
			throw std::runtime_error("Asset snapshot catalog exceeds its byte limit");

		std::unordered_map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> Artifacts;
		std::size_t TotalBytes = 0;
		for (const auto &Artifact : Snapshot.Artifacts) {
			if (!Artifact.Bytes || Artifact.Bytes->size() > AssetLimits::MaximumArtifactBytes ||
				Artifact.RelativePath.size() > AssetLimits::MaximumSourcePathBytes)
				throw std::runtime_error("Asset snapshot artifact exceeds its bounds");
			if (TotalBytes > AssetLimits::MaximumCpuCacheBytes -
				std::min(Artifact.Bytes->size(), AssetLimits::MaximumCpuCacheBytes))
				throw std::runtime_error("Asset snapshot artifacts exceed their aggregate byte limit");
			TotalBytes += Artifact.Bytes->size();
			if (!Artifacts.emplace(Artifact.RelativePath, Artifact.Bytes).second)
				throw std::runtime_error("Asset snapshot contains a duplicate artifact path");
		}

		State->LoadCatalog(Snapshot.CatalogJson, [&](const AssetContentId &ContentId) -> std::expected<
			std::shared_ptr<const std::vector<std::uint8_t>>, AssetDiagnostic
		> {
			const auto Path = ArtifactPath(ContentId);
			auto Artifact = Artifacts.find(Path);
			if (Artifact == Artifacts.end())
				return std::unexpected(Error("Missing", "Asset artifact is absent from the captured Play snapshot"));
			return Artifact->second;
		});
	}

	AssetProjectSnapshot AssetService::CaptureProjectAssets() const {
		std::scoped_lock Lock(State->Mutex);
		Json Assets = Json::array();
		std::vector<const AssetRecord *> Ordered;
		for (const auto &[Reference, Record] : State->Records) {
			(void)Reference;
			if (!Record.BuiltIn && Record.State != AssetState::Importing) Ordered.push_back(&Record);
		}
		std::ranges::sort(Ordered, {}, [](const AssetRecord *Record) { return Record->Reference.Value; });
		std::unordered_set<std::string> RequiredArtifacts;
		for (const auto *Record : Ordered) {
			Json Diagnostic = nullptr;
			if (Record->Diagnostic) Diagnostic = {{"Code", Record->Diagnostic->Code}, {"Message", Record->Diagnostic->Message}};
			Assets.push_back({
				{"AssetId", Record->Id.ToString()}, {"Reference", Record->Reference.Value},
				{"Kind", GetAssetKindName(Record->Kind)}, {"Name", Record->Name}, {"Source", Record->Source},
				{"ContentId", Record->ContentId.IsValid() ? Json(Record->ContentId.ToString()) : Json(nullptr)},
				{"ContentRevision", Record->ContentRevision},
				{"State", GetAssetStateName(Record->State)}, {"Diagnostic", std::move(Diagnostic)},
			});
			if (Record->ContentId.IsValid()) RequiredArtifacts.insert(Record->ContentId.ToString());
		}
		AssetProjectSnapshot Result{Json({{"Version", CatalogVersion}, {"Assets", std::move(Assets)}}).dump(), {}};
		for (const auto &Content : RequiredArtifacts) {
			auto Artifact = State->Artifacts.find(Content);
			if (Artifact == State->Artifacts.end()) throw std::runtime_error("Asset catalog references a missing in-memory artifact");
			Result.Artifacts.push_back({std::string(ArtifactDirectory) + "/" + Content + ".gasset", Artifact->second});
		}
		std::ranges::sort(Result.Artifacts, {}, &AssetArtifactFile::RelativePath);
		return Result;
	}

	int AssetService::ResolveAsset(lua_State *L, Instance *InstanceValue) {
		auto &Service = CheckedService(InstanceValue);
		const auto Reference = ReadReference(L, 2);
		if (!AssetReference::Parse(Reference) || !Service.IsAvailable(Reference)) { lua_pushnil(L); return 1; }
		lua_pushlstring(L, Reference.data(), Reference.size());
		return 1;
	}

	int AssetService::IsAssetAvailable(lua_State *L, Instance *InstanceValue) {
		const auto Reference = ReadReference(L, 2);
		lua_pushboolean(L, AssetReference::Parse(Reference) && CheckedService(InstanceValue).IsAvailable(Reference));
		return 1;
	}

	int AssetService::GetAssetMetadata(lua_State *L, Instance *InstanceValue) {
		const auto Reference = ReadReference(L, 2);
		if (!AssetReference::Parse(Reference)) { lua_pushnil(L); return 1; }
		auto Record = CheckedService(InstanceValue).GetAsset(Reference);
		if (!Record) { lua_pushnil(L); return 1; }
		lua_createtable(L, 0, 12);
		PushStringField(L, "Reference", Record->Reference.Value);
		PushStringField(L, "AssetId", Record->Id.ToString());
		PushStringField(L, "Kind", GetAssetKindName(Record->Kind));
		PushStringField(L, "Name", Record->Name);
		PushStringField(L, "State", GetAssetStateName(Record->State));
		PushStringField(L, "ContentId", Record->ContentId.IsValid() ? Record->ContentId.ToString() : "");
		lua_pushnumber(L, static_cast<double>(Record->ContentRevision)); lua_setfield(L, -2, "ContentRevision");
		lua_pushboolean(L, IsAvailableRecord(*Record)); lua_setfield(L, -2, "Available");
		if (Record->Diagnostic) {
			PushStringField(L, "DiagnosticCode", Record->Diagnostic->Code);
			PushStringField(L, "Diagnostic", Record->Diagnostic->Message);
		}
		if (Record->Asset) std::visit([&](const auto &Asset) {
			using T = std::decay_t<decltype(Asset)>;
			if constexpr (std::is_same_v<T, ImportedImage>) {
				lua_pushnumber(L, Asset.Width); lua_setfield(L, -2, "Width");
				lua_pushnumber(L, Asset.Height); lua_setfield(L, -2, "Height");
			} else if constexpr (std::is_same_v<T, ImportedMesh>) {
				lua_pushnumber(L, Asset.Vertices ? Asset.Vertices->size() : 0); lua_setfield(L, -2, "VertexCount");
				lua_pushnumber(L, Asset.Indices ? Asset.Indices->size() : 0); lua_setfield(L, -2, "IndexCount");
			} else {
				lua_pushnumber(L, Asset.FaceCount); lua_setfield(L, -2, "FaceCount");
			}
		}, *Record->Asset);
		return 1;
	}
}
