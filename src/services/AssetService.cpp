// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/services/AssetService.hpp"

#include "assets/AssetImporter.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/ImageLabel.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/TextLabel.hpp"
#include "gargantuan/filesystem/BaseFilesystem.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/runtime/JobSystem.hpp"
#include "gargantuan/render/RenderDirtyAccumulator.hpp"
#include "serialization/JsonCodec.hpp"

#include <SDL3/SDL.h>
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
		constexpr std::uint32_t CatalogVersion = 2;
		constexpr std::uint32_t LegacyCatalogVersion = 1;
		constexpr std::string_view CatalogPath = ".gargantuan/assets/catalog.json";
		constexpr std::string_view ArtifactDirectory = ".gargantuan/assets/artifacts";
		constexpr std::string_view DefaultFontReference = "builtin://font/default";
		constexpr std::string_view MissingImageReference = "builtin://image/missing";
		constexpr std::string_view DefaultMaterialReference = "builtin://material/default";

		struct AssetImportThreadCleanup final {
			~AssetImportThreadCleanup() { SDL_CleanupTLS(); }
		};

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
				else if constexpr (std::is_same_v<T, ImportedMesh>)
					return (Value.Vertices ? Value.Vertices->size() * sizeof(RenderVertex) : 0) +
						(Value.Indices ? Value.Indices->size() * sizeof(std::uint32_t) : 0) +
						(Value.Primitives ? Value.Primitives->size() * sizeof(ImportedMeshPrimitive) : 0);
				else return sizeof(ImportedMaterial);
			}, Asset);
		}

		AssetId StableChildId(AssetId Group, std::string_view LogicalKey) {
			std::vector<std::uint8_t> Identity;
			Identity.reserve(16 + LogicalKey.size());
			for (std::size_t Shift = 0; Shift < 8; ++Shift) Identity.push_back(static_cast<std::uint8_t>(Group.High >> (Shift * 8)));
			for (std::size_t Shift = 0; Shift < 8; ++Shift) Identity.push_back(static_cast<std::uint8_t>(Group.Low >> (Shift * 8)));
			Identity.insert(Identity.end(), LogicalKey.begin(), LogicalKey.end());
			const auto Hash = AssetContentId::Hash(Identity);
			AssetId Result;
			for (std::size_t Index = 0; Index < 8; ++Index) Result.High = (Result.High << 8) | Hash.Bytes[Index];
			for (std::size_t Index = 8; Index < 16; ++Index) Result.Low = (Result.Low << 8) | Hash.Bytes[Index];
			if (!Result.IsValid()) Result.Low = 1;
			return Result;
		}

		struct PreparedGraph {
			std::vector<AssetRecord> Records;
			std::unordered_map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> Artifacts;
			std::size_t CpuBytes = 0;
		};

		std::expected<PreparedGraph, AssetDiagnostic> PrepareGraph(
			AssetImportGraphCandidate Candidate,
			AssetId GroupId,
			const std::string &Source,
			const std::string &RequestedName,
			const std::unordered_map<std::string, AssetRecord> &ExistingByKey
		) {
			if (!GroupId.IsValid() || Candidate.Nodes.empty() ||
				Candidate.Nodes.size() > AssetLimits::MaximumGeneratedAssets ||
				Candidate.Nodes.size() > AssetLimits::MaximumCatalogRecords ||
				Candidate.Diagnostics.size() > AssetLimits::MaximumImportDiagnostics)
				return std::unexpected(Error("GeneratedAssetLimit", "Import graph asset count is invalid or oversized"));
			std::unordered_map<std::string, AssetId> Ids;
			std::unordered_map<std::string, AssetKind> Kinds;
			Ids.reserve(Candidate.Nodes.size());
			Kinds.reserve(Candidate.Nodes.size());
			std::unordered_set<AssetId, AssetIdHash> UniqueIds;
			for (const auto &Node : Candidate.Nodes) {
				if (Node.LogicalKey.empty() || Node.LogicalKey.size() > AssetLimits::MaximumNameBytes ||
					!Kinds.emplace(Node.LogicalKey, Node.Kind).second)
					return std::unexpected(Error("InvalidLogicalKey", "Import graph contains an invalid or duplicate logical child key"));
				const auto Existing = ExistingByKey.find(Node.LogicalKey);
				const auto Id = Existing != ExistingByKey.end() ? Existing->second.Id :
					(Candidate.Nodes.size() == 1 && Node.LogicalKey == "asset" ? GroupId : StableChildId(GroupId, Node.LogicalKey));
				if (!Id.IsValid() || !UniqueIds.insert(Id).second)
					return std::unexpected(Error("AssetIdentityCollision", "Import graph produced a duplicate semantic AssetId"));
				Ids.emplace(Node.LogicalKey, Id);
			}
			if (!Ids.contains(Candidate.PrimaryLogicalKey))
				return std::unexpected(Error("InvalidImportGraph", "Import graph primary logical key is missing"));

			std::unordered_map<std::string, std::vector<std::string>> DependencyKeys;
			std::size_t DependencyCount = 0;
			for (const auto &Node : Candidate.Nodes) {
				auto &Edges = DependencyKeys[Node.LogicalKey];
				for (const auto &Binding : Node.Bindings) {
					if (!Ids.contains(Binding.LogicalKey) || Binding.LogicalKey == Node.LogicalKey)
						return std::unexpected(Error("InvalidDependency", "Import graph dependency target is missing or self-referential"));
					if (Binding.Kind == AssetImportBindingKind::MeshPrimitiveMaterial &&
						(Node.Kind != AssetKind::Mesh || Kinds.at(Binding.LogicalKey) != AssetKind::Material))
						return std::unexpected(Error("InvalidDependency", "Mesh primitive dependency must target a Material asset"));
					if ((Binding.Kind == AssetImportBindingKind::MaterialBaseColorTexture ||
						Binding.Kind == AssetImportBindingKind::MaterialNormalTexture) &&
						(Node.Kind != AssetKind::Material || Kinds.at(Binding.LogicalKey) != AssetKind::Image))
						return std::unexpected(Error("InvalidDependency", "Material texture dependency must target an Image asset"));
					if (!std::ranges::contains(Edges, Binding.LogicalKey)) Edges.push_back(Binding.LogicalKey);
				}
				if (Edges.size() > AssetLimits::MaximumDependencies)
					return std::unexpected(Error("DependencyLimit", "Import graph dependency count exceeds its limit"));
				if (Edges.size() > AssetLimits::MaximumGraphDependencies -
					std::min(DependencyCount, AssetLimits::MaximumGraphDependencies))
					return std::unexpected(Error("DependencyLimit", "Import graph aggregate dependency count exceeds its limit"));
				DependencyCount += Edges.size();
			}
			std::unordered_map<std::string, std::uint8_t> Visit;
			std::function<bool(const std::string &)> IsAcyclic = [&](const std::string &Key) {
				auto &StateValue = Visit[Key];
				if (StateValue == 1) return false;
				if (StateValue == 2) return true;
				StateValue = 1;
				for (const auto &Dependency : DependencyKeys[Key]) if (!IsAcyclic(Dependency)) return false;
				StateValue = 2;
				return true;
			};
			for (const auto &[Key, Id] : Ids) { (void)Id; if (!IsAcyclic(Key))
				return std::unexpected(Error("DependencyCycle", "Import graph dependency cycle is not allowed")); }

			PreparedGraph Result;
			Result.Records.reserve(Candidate.Nodes.size());
			std::size_t ArtifactBytes = 0;
			for (auto &Node : Candidate.Nodes) {
				for (const auto &Binding : Node.Bindings) {
					const auto DependencyId = Ids.at(Binding.LogicalKey);
					if (Binding.Kind == AssetImportBindingKind::MeshPrimitiveMaterial) {
						auto *Mesh = std::get_if<ImportedMesh>(&Node.Asset);
						if (!Mesh || !Mesh->Primitives || Binding.TargetIndex >= Mesh->Primitives->size())
							return std::unexpected(Error("InvalidDependency", "Mesh material binding target is invalid"));
						auto Primitives = std::make_shared<std::vector<ImportedMeshPrimitive>>(*Mesh->Primitives);
						(*Primitives)[Binding.TargetIndex].Material = DependencyId;
						Mesh->Primitives = std::move(Primitives);
					} else {
						auto *Material = std::get_if<ImportedMaterial>(&Node.Asset);
						if (!Material) return std::unexpected(Error("InvalidDependency", "Material texture binding target is invalid"));
						if (Binding.Kind == AssetImportBindingKind::MaterialBaseColorTexture)
							Material->BaseColorTexture = DependencyId;
						else Material->NormalTexture = DependencyId;
					}
				}
				auto Artifact = EncodeAssetArtifact(Node.Asset, Node.Kind);
				if (!Artifact) return std::unexpected(Artifact.error());
				if (!*Artifact || (*Artifact)->size() > AssetLimits::MaximumArtifactBytes ||
					ArtifactBytes > AssetLimits::MaximumArtifactBytes - (*Artifact)->size())
					return std::unexpected(Error("ArtifactLimit", "Import graph canonical artifacts exceed their aggregate byte limit"));
				ArtifactBytes += (*Artifact)->size();
				const auto ContentId = AssetContentId::Hash(**Artifact);
				std::vector<AssetId> Dependencies;
				for (const auto &Key : DependencyKeys[Node.LogicalKey]) Dependencies.push_back(Ids.at(Key));
				const auto Existing = ExistingByKey.find(Node.LogicalKey);
				const bool Primary = Node.LogicalKey == Candidate.PrimaryLogicalKey;
				std::string Name = Existing != ExistingByKey.end() ? Existing->second.Name : Node.Name;
				if (Primary && Existing == ExistingByKey.end() && !RequestedName.empty()) Name = RequestedName;
				if (Name.empty()) Name = Primary ? DefaultName(Source) : "Generated Asset";
				if (Name.size() > AssetLimits::MaximumNameBytes) Name.resize(AssetLimits::MaximumNameBytes);
				const bool SameContent = Existing != ExistingByKey.end() && Existing->second.ContentId == ContentId;
				const bool SameDependencies = Existing != ExistingByKey.end() && Existing->second.Dependencies == Dependencies;
				std::uint64_t Revision = Existing == ExistingByKey.end() ? 1 : Existing->second.ContentRevision;
				if (Existing != ExistingByKey.end() && (!SameContent || !SameDependencies)) {
					if (Revision == std::numeric_limits<std::uint64_t>::max())
						return std::unexpected(Error("RevisionExhausted", "Asset content revision is exhausted"));
					++Revision;
				}
				auto Asset = std::make_shared<const ImportedAsset>(std::move(Node.Asset));
				AssetRecord Record{Ids.at(Node.LogicalKey), AssetReference::FromAssetId(Ids.at(Node.LogicalKey)), Node.Kind,
					std::move(Name), Source, ContentId, Revision, AssetState::Ready, std::nullopt,
					std::move(Dependencies), std::move(Asset), false};
				Record.SourceGroupId = GroupId;
				Record.LogicalKey = Node.LogicalKey;
				Record.PrimarySourceAsset = Primary;
				if (Primary && !Candidate.Diagnostics.empty()) Record.Diagnostic = Candidate.Diagnostics.front();
				const auto Bytes = AssetBytes(*Record.Asset);
				if (Bytes > AssetLimits::MaximumCpuCacheBytes ||
					Result.CpuBytes > AssetLimits::MaximumCpuCacheBytes - Bytes)
					return std::unexpected(Error("CacheLimit", "Import graph exceeds the bounded CPU cache"));
				Result.CpuBytes += Bytes;
				Result.Artifacts.insert_or_assign(ContentId.ToString(), *Artifact);
				Result.Records.push_back(std::move(Record));
			}
			std::ranges::sort(Result.Records, {}, [](const AssetRecord &Record) { return Record.Reference.Value; });
			return Result;
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
		std::unordered_set<AssetId, AssetIdHash> InFlightGroups;

		Impl() { RegisterMissingImage(); RegisterDefaultMaterial(); }

		void RegisterMissingImage() {
			static constexpr std::array<std::uint8_t, 16> Pixels{
				255, 0, 255, 255, 20, 20, 20, 255, 20, 20, 20, 255, 255, 0, 255, 255,
			};
			const auto Id = AssetId::FromBuiltInName(MissingImageReference);
			auto PixelBytes = std::make_shared<const std::vector<std::uint8_t>>(Pixels.begin(), Pixels.end());
			auto Asset = std::make_shared<const ImportedAsset>(ImportedImage{2, 2, PixelBytes});
			AssetRecord Record{Id, *AssetReference::Parse(MissingImageReference), AssetKind::Image, "Missing Image",
				"engine-package", {}, 1, AssetState::Ready, std::nullopt, {}, Asset, true};
			Record.SourceGroupId = Id;
			Records.emplace(Record.Reference.Value, Record);
			TextureResidency Residency{TextureIdentity(Id), 1, 2, 2, PixelBytes};
			Textures.emplace(Record.Reference.Value, Residency);
			CpuBytes += PixelBytes->size();
			PendingTextures.Creates.push_back({Residency.Identity, 1, 2, 2, RenderTextureFormat::Rgba8Unorm, PixelBytes});
			PendingTextures.UploadBytes += PixelBytes->size();
		}

		void RegisterDefaultMaterial() {
			const auto Id = AssetId::FromBuiltInName(DefaultMaterialReference);
			auto Asset = std::make_shared<const ImportedAsset>(ImportedMaterial{});
			AssetRecord Record{Id, *AssetReference::Parse(DefaultMaterialReference), AssetKind::Material,
				"Default Material", "engine-package", {}, 1, AssetState::Ready, std::nullopt, {}, Asset, true};
			Record.SourceGroupId = Id;
			Records.emplace(Record.Reference.Value, std::move(Record));
			CpuBytes += sizeof(ImportedMaterial);
		}

		void PublishChange(const AssetRecord &Record, AssetChangeKind ChangeKind = AssetChangeKind::StateChanged) {
			if (NextChangeSequence == 0) throw std::overflow_error("Asset change sequence is exhausted");
			if (Changes.size() == AssetLimits::MaximumChangeRecords) Changes.pop_front();
			Changes.push_back({NextChangeSequence++, Record.Reference.Value, Record.Kind,
				Record.ContentRevision, Record.State, ChangeKind});
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

		std::expected<AssetImportGraphCandidate, AssetDiagnostic> Decode(
			AssetKind Kind,
			std::string Extension,
			std::shared_ptr<const std::vector<std::uint8_t>> Source,
			std::shared_ptr<const std::unordered_map<std::string,
				std::shared_ptr<const std::vector<std::uint8_t>>>> ExternalResources,
			const AssetCancellationToken &Cancellation
		) {
			auto *Importer = FindImporter(Kind, Extension);
			if (!Importer) return std::unexpected(Error("UnsupportedFormat", "No bounded importer supports the requested kind and extension"));
			std::expected<AssetImportGraphCandidate, AssetDiagnostic> Result = std::unexpected(Error("InternalFailure", "Import job did not run"));
			auto Group = std::make_shared<JobGroup>();
			Jobs.Submit([&Result, Importer, Kind, Extension = std::move(Extension), Source = std::move(Source),
				ExternalResources = std::move(ExternalResources), Cancellation] {
				AssetImportThreadCleanup ThreadCleanup;
				try {
					Result = Importer->ImportGraph(*Source, {Kind, Extension, Cancellation,
						std::chrono::steady_clock::now() + std::chrono::seconds(5), ExternalResources});
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

		std::expected<AssetImportGraphCandidate, AssetDiagnostic> DecodeFromMount(
			SourceMount &Mount,
			const std::string &SourcePath,
			AssetKind Kind,
			const AssetCancellationToken &Cancellation
		) {
			const auto Extension = LowerExtension(SourcePath);
			auto *Importer = FindImporter(Kind, Extension);
			if (!Importer) return std::unexpected(Error("UnsupportedFormat", "No bounded importer supports the requested source"));
			SourceMountBudget Budget;
			auto Read = Mount.ReadFile(std::filesystem::path(SourcePath), AssetLimits::MaximumSourceBytes, Budget);
			if (!Read) return std::unexpected(Error(std::string(GetSourceMountErrorCodeName(Read.error().Code)), Read.error().Format()));
			auto Bytes = std::make_shared<const std::vector<std::uint8_t>>(Read->begin(), Read->end());
			AssetImportContext DiscoveryContext{Kind, Extension, Cancellation,
				std::chrono::steady_clock::now() + std::chrono::seconds(2), {}};
			auto ResourceUris = Importer->DiscoverExternalResources(*Bytes, DiscoveryContext);
			if (!ResourceUris) return std::unexpected(ResourceUris.error());
			if (ResourceUris->size() > AssetLimits::MaximumExternalResources)
				return std::unexpected(Error("ExternalResourceLimit", "Import external resource count exceeds its limit"));
			auto Resources = std::make_shared<std::unordered_map<std::string,
				std::shared_ptr<const std::vector<std::uint8_t>>>>();
			Resources->reserve(ResourceUris->size());
			const auto SourceDirectory = std::filesystem::path(SourcePath).parent_path();
			for (const auto &Uri : *ResourceUris) {
				auto External = Mount.ReadFile(SourceDirectory / std::filesystem::u8path(Uri),
					AssetLimits::MaximumSourceBytes, Budget);
				if (!External) return std::unexpected(Error(
					std::string(GetSourceMountErrorCodeName(External.error().Code)), External.error().Format()
				));
				auto ExternalBytes = std::make_shared<const std::vector<std::uint8_t>>(External->begin(), External->end());
				Resources->emplace(Uri, std::move(ExternalBytes));
			}
			return Decode(Kind, Extension, std::move(Bytes), std::move(Resources), Cancellation);
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
			if (!Parsed || !Parsed->is_object() || Parsed->size() != 2 || !Parsed->contains("Version") ||
				!(*Parsed)["Version"].is_number_unsigned() || !Parsed->contains("Assets") ||
				!(*Parsed)["Assets"].is_array() || (*Parsed)["Assets"].size() > AssetLimits::MaximumCatalogRecords)
				throw std::runtime_error("Asset catalog format is invalid or unsupported");
			const auto Version = (*Parsed)["Version"].get<std::uint32_t>();
			if (Version != LegacyCatalogVersion && Version != CatalogVersion)
				throw std::runtime_error("Asset catalog version is unsupported");
			std::scoped_lock StateLock(Mutex);
			for (const auto &[Reference, Record] : Records) {
				(void)Reference;
				if (!Record.BuiltIn) throw std::logic_error("Project assets are already loaded");
			}

			std::unordered_set<std::string> Seen;
			for (const auto &Encoded : (*Parsed)["Assets"]) {
				const auto ExpectedFields = Version == LegacyCatalogVersion ? 9u : 13u;
				if (!Encoded.is_object() || Encoded.size() != ExpectedFields || !Encoded.contains("AssetId") || !Encoded["AssetId"].is_string() ||
					!Encoded.contains("Reference") || !Encoded["Reference"].is_string() ||
					!Encoded.contains("Kind") || !Encoded["Kind"].is_string() ||
					!Encoded.contains("Name") || !Encoded["Name"].is_string() ||
					!Encoded.contains("Source") || !Encoded["Source"].is_string() ||
					!Encoded.contains("ContentId") || (!Encoded["ContentId"].is_null() && !Encoded["ContentId"].is_string()) ||
					!Encoded.contains("ContentRevision") || !Encoded["ContentRevision"].is_number_unsigned() ||
					!Encoded.contains("State") || !Encoded["State"].is_string() || !Encoded.contains("Diagnostic") ||
					(Version == CatalogVersion && (!Encoded.contains("SourceGroupId") || !Encoded["SourceGroupId"].is_string() ||
						!Encoded.contains("LogicalKey") || !Encoded["LogicalKey"].is_string() ||
						!Encoded.contains("PrimarySourceAsset") || !Encoded["PrimarySourceAsset"].is_boolean() ||
						!Encoded.contains("Dependencies") || !Encoded["Dependencies"].is_array())))
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
				Record.SourceGroupId = *Id;
				if (Version == CatalogVersion) {
					auto GroupId = AssetId::Parse(Encoded["SourceGroupId"].get_ref<const std::string &>());
					const auto LogicalKey = Encoded["LogicalKey"].get<std::string>();
					if (!GroupId || LogicalKey.empty() || LogicalKey.size() > AssetLimits::MaximumNameBytes ||
						Encoded["Dependencies"].size() > AssetLimits::MaximumDependencies)
						throw std::runtime_error("Asset catalog source grouping is invalid");
					Record.SourceGroupId = *GroupId;
					Record.LogicalKey = LogicalKey;
					Record.PrimarySourceAsset = Encoded["PrimarySourceAsset"].get<bool>();
					std::unordered_set<AssetId, AssetIdHash> UniqueDependencies;
					for (const auto &DependencyValue : Encoded["Dependencies"]) {
						if (!DependencyValue.is_string()) throw std::runtime_error("Asset catalog dependency is malformed");
						auto Dependency = AssetId::Parse(DependencyValue.get_ref<const std::string &>());
						if (!Dependency || *Dependency == *Id || !UniqueDependencies.insert(*Dependency).second)
							throw std::runtime_error("Asset catalog dependency identity is invalid");
						Record.Dependencies.push_back(*Dependency);
					}
				}
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
			}

			std::unordered_map<AssetId, const AssetRecord *, AssetIdHash> ById;
			std::unordered_map<AssetId, std::size_t, AssetIdHash> PrimaryCounts;
			std::unordered_map<AssetId, std::string, AssetIdHash> GroupSources;
			for (const auto &[Reference, Record] : Records) if (!Record.BuiltIn) {
				(void)Reference;
				if (!ById.emplace(Record.Id, &Record).second) throw std::runtime_error("Asset catalog contains duplicate AssetIds");
				if (Record.PrimarySourceAsset) ++PrimaryCounts[Record.SourceGroupId];
				auto [Source, Inserted] = GroupSources.emplace(Record.SourceGroupId, Record.Source);
				if (!Inserted && Source->second != Record.Source) throw std::runtime_error("Asset source group has inconsistent provenance");
			}
			for (const auto &[Group, Source] : GroupSources) {
				(void)Source;
				if (PrimaryCounts[Group] != 1) throw std::runtime_error("Asset source group must contain exactly one primary record");
			}
			std::unordered_map<AssetId, std::uint8_t, AssetIdHash> Visit;
			std::function<bool(AssetId)> IsAcyclic = [&](AssetId Id) {
				auto &StateValue = Visit[Id];
				if (StateValue == 1) return false;
				if (StateValue == 2) return true;
				StateValue = 1;
				for (const auto Dependency : ById.at(Id)->Dependencies) {
					if (!ById.contains(Dependency) || !IsAcyclic(Dependency)) return false;
				}
				StateValue = 2;
				return true;
			};
			for (const auto &[Id, Record] : ById) {
				if (!IsAcyclic(Id)) throw std::runtime_error("Asset catalog dependency graph is missing a target or cyclic");
				std::vector<AssetId> SemanticDependencies;
				if (Record->Asset) {
					if (const auto *Material = std::get_if<ImportedMaterial>(Record->Asset.get())) {
						if (Material->BaseColorTexture) SemanticDependencies.push_back(*Material->BaseColorTexture);
						if (Material->NormalTexture && !std::ranges::contains(SemanticDependencies, *Material->NormalTexture))
							SemanticDependencies.push_back(*Material->NormalTexture);
					} else if (const auto *Mesh = std::get_if<ImportedMesh>(Record->Asset.get()); Mesh && Mesh->Primitives) {
						for (const auto &Primitive : *Mesh->Primitives) if (Primitive.Material &&
							!std::ranges::contains(SemanticDependencies, *Primitive.Material)) SemanticDependencies.push_back(*Primitive.Material);
					}
				}
				if (SemanticDependencies != Record->Dependencies)
					throw std::runtime_error("Asset artifact dependencies disagree with the catalog graph");
			}
			for (const auto &[Reference, Record] : Records) {
				(void)Reference;
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

		const bool Compound = State->FindImporter(*Kind, Extension)->IsCompound();
		AssetId GroupId;
		{
			std::scoped_lock Lock(State->Mutex);
			if (State->Records.size() >= AssetLimits::MaximumCatalogRecords)
				return {false, std::nullopt, Error("CatalogLimit", "Asset catalog record limit is reached")};
			if (State->InFlight >= AssetLimits::MaximumInFlightImports)
				return {false, std::nullopt, Error("ImportQueueFull", "Asset import concurrency limit is reached")};
			do GroupId = AssetId::New(); while (State->InFlightGroups.contains(GroupId) ||
				State->Records.contains(AssetReference::FromAssetId(GroupId).Value));
			++State->InFlight;
			State->InFlightGroups.insert(GroupId);
		}

		auto Candidate = State->DecodeFromMount(Mount, Source, *Kind, Cancellation);
		std::expected<PreparedGraph, AssetDiagnostic> Prepared = std::unexpected(
			Candidate ? Error("InternalFailure", "Import graph preparation did not run") : Candidate.error()
		);
		if (Candidate) Prepared = PrepareGraph(std::move(*Candidate), GroupId, Source, Name, {});
		AssetOperationResult Operation;
		bool Mutated = false;
		{
			std::scoped_lock Lock(State->Mutex);
			--State->InFlight;
			State->InFlightGroups.erase(GroupId);
			if (!Prepared) {
				Operation.Diagnostic = Prepared.error();
				if (!Compound) {
					AssetRecord Failed{GroupId, AssetReference::FromAssetId(GroupId), *Kind, Name, Source, {}, 0,
						AssetState::Failed, Prepared.error()};
					Failed.SourceGroupId = GroupId;
					State->Records.emplace(Failed.Reference.Value, Failed);
					State->PublishChange(Failed, AssetChangeKind::Added);
					Operation.Record = Failed;
					Operation.Records = {Failed};
					Mutated = true;
				}
			} else if (Prepared->Records.size() > AssetLimits::MaximumCatalogRecords - State->Records.size() ||
				Prepared->CpuBytes > AssetLimits::MaximumCpuCacheBytes - State->CpuBytes) {
				Operation.Diagnostic = Error("CacheLimit", "Import graph would exceed catalog or CPU cache limits");
			} else {
				for (const auto &Record : Prepared->Records)
					if (State->Records.contains(Record.Reference.Value)) {
						Operation.Diagnostic = Error("AssetIdentityCollision", "Import graph AssetId collides with an existing record");
						break;
					}
				if (Operation.Diagnostic.Code.empty()) {
					for (const auto &[Content, Artifact] : Prepared->Artifacts)
						State->Artifacts.insert_or_assign(Content, Artifact);
					for (const auto &Record : Prepared->Records) {
						State->Records.emplace(Record.Reference.Value, Record);
						State->QueueResidency(Record, false);
						State->PublishChange(Record, AssetChangeKind::Added);
						if (Record.PrimarySourceAsset) Operation.Record = Record;
					}
					State->CpuBytes += Prepared->CpuBytes;
					Operation.Ok = true;
					Operation.Records = Prepared->Records;
					Mutated = true;
				}
			}
		}
		if (auto World = GetDataModel(); World && Mutated) World->AdvanceAuthoritativeRevision();
		return Operation;
	}

	AssetOperationResult AssetService::ReimportProjectAsset(
		SourceMount &Mount,
		std::string_view Reference,
		const AssetCancellationToken &Cancellation
	) {
		AssetRecord Requested;
		AssetKind SourceKind = AssetKind::Image;
		std::vector<AssetRecord> PreviousRecords;
		std::unordered_map<std::string, AssetRecord> PreviousByKey;
		{
			std::scoped_lock Lock(State->Mutex);
			auto Existing = State->Records.find(std::string(Reference));
			if (Existing == State->Records.end() || Existing->second.BuiltIn)
				return {false, std::nullopt, Error("UnknownAsset", "Project asset reference is unknown")};
			Requested = Existing->second;
			if (State->InFlight >= AssetLimits::MaximumInFlightImports || State->InFlightGroups.contains(Requested.SourceGroupId))
				return {false, Existing->second, Error("ImportQueueFull", "Asset import concurrency limit is reached")};
			for (auto &[RecordReference, Record] : State->Records) if (!Record.BuiltIn && Record.SourceGroupId == Requested.SourceGroupId) {
				(void)RecordReference;
				PreviousRecords.push_back(Record);
				PreviousByKey.emplace(Record.LogicalKey, Record);
				if (Record.PrimarySourceAsset) SourceKind = Record.Kind;
				Record.State = AssetState::Importing;
				State->PublishChange(Record, AssetChangeKind::StateChanged);
			}
			if (PreviousRecords.empty()) return {false, Existing->second, Error("InvalidImportGroup", "Asset source group is empty")};
			++State->InFlight;
			State->InFlightGroups.insert(Requested.SourceGroupId);
		}

		auto Candidate = State->DecodeFromMount(Mount, Requested.Source, SourceKind, Cancellation);
		std::expected<PreparedGraph, AssetDiagnostic> Prepared = std::unexpected(
			Candidate ? Error("InternalFailure", "Reimport graph preparation did not run") : Candidate.error()
		);
		if (Candidate) Prepared = PrepareGraph(std::move(*Candidate), Requested.SourceGroupId,
			Requested.Source, {}, PreviousByKey);
		AssetOperationResult Operation;
		std::unordered_set<AssetId, AssetIdHash> ContentChanged;
		std::unordered_set<AssetId, AssetIdHash> SemanticallyChanged;
		{
			std::scoped_lock Lock(State->Mutex);
			--State->InFlight;
			State->InFlightGroups.erase(Requested.SourceGroupId);
			if (!Prepared) {
				for (const auto &Previous : PreviousRecords) {
					auto Restored = Previous;
					Restored.State = Restored.Asset ? AssetState::Stale : AssetState::Failed;
					Restored.Diagnostic = Prepared.error();
					State->Records.insert_or_assign(Restored.Reference.Value, Restored);
					State->PublishChange(Restored, AssetChangeKind::StateChanged);
					Operation.Records.push_back(Restored);
					if (Restored.PrimarySourceAsset) Operation.Record = Restored;
				}
				Operation.Diagnostic = Prepared.error();
			} else {
				std::unordered_set<AssetId, AssetIdHash> OldIds;
				std::unordered_set<AssetId, AssetIdHash> NextIds;
				std::size_t PreviousBytes = 0;
				for (const auto &Previous : PreviousRecords) {
					OldIds.insert(Previous.Id);
					if (Previous.Asset) PreviousBytes += AssetBytes(*Previous.Asset);
				}
				for (const auto &Next : Prepared->Records) NextIds.insert(Next.Id);
				AssetDiagnostic CommitFailure;
				for (const auto &Next : Prepared->Records) {
					auto Collision = State->Records.find(Next.Reference.Value);
					if (Collision != State->Records.end() && !OldIds.contains(Collision->second.Id)) {
						CommitFailure = Error("AssetIdentityCollision", "Reimport generated AssetId collides with another source group");
						break;
					}
				}
				for (const auto &Previous : PreviousRecords) if (!NextIds.contains(Previous.Id) && CommitFailure.Code.empty()) {
					for (const auto &[OtherReference, Other] : State->Records) {
						(void)OtherReference;
						if (OldIds.contains(Other.Id)) continue;
						if (std::ranges::contains(Other.Dependencies, Previous.Id)) {
							CommitFailure = Error("AssetReferenced", "Removed generated asset still has a catalog dependent");
							break;
						}
					}
					if (CommitFailure.Code.empty()) if (auto World = GetDataModel()) {
						const auto RemovedReference = Previous.Reference.Value;
						for (const auto &Object : World->GetDescendants()) {
							if (auto Image = std::dynamic_pointer_cast<ImageLabel>(Object); Image && Image->GetImage() == RemovedReference) {
								CommitFailure = Error("AssetReferenced", "Removed generated asset is referenced by ImageLabel");
								break;
							}
							if (auto Text = std::dynamic_pointer_cast<TextLabel>(Object); Text && Text->GetFontFace() == RemovedReference) {
								CommitFailure = Error("AssetReferenced", "Removed generated asset is referenced by a text object");
								break;
							}
							if (auto Mesh = std::dynamic_pointer_cast<MeshPart>(Object); Mesh &&
								(Mesh->GetMesh() == RemovedReference || Mesh->GetMaterial() == RemovedReference)) {
								CommitFailure = Error("AssetReferenced", "Removed generated asset is referenced by MeshPart");
								break;
							}
						}
					}
				}
				if (Prepared->CpuBytes > AssetLimits::MaximumCpuCacheBytes ||
					State->CpuBytes - PreviousBytes > AssetLimits::MaximumCpuCacheBytes - Prepared->CpuBytes)
					CommitFailure = Error("CacheLimit", "Reimport graph would exceed the bounded CPU cache");
				if (!CommitFailure.Code.empty()) {
					for (const auto &Previous : PreviousRecords) {
						auto Restored = Previous;
						Restored.State = AssetState::Stale;
						Restored.Diagnostic = CommitFailure;
						State->Records.insert_or_assign(Restored.Reference.Value, Restored);
						State->PublishChange(Restored, AssetChangeKind::StateChanged);
						Operation.Records.push_back(Restored);
						if (Restored.PrimarySourceAsset) Operation.Record = Restored;
					}
					Operation.Diagnostic = CommitFailure;
				} else {
					for (auto &Next : Prepared->Records) {
						auto Previous = PreviousByKey.find(Next.LogicalKey);
						if (Previous == PreviousByKey.end() || Previous->second.ContentId != Next.ContentId)
							ContentChanged.insert(Next.Id);
						if (Previous == PreviousByKey.end() || Previous->second.ContentId != Next.ContentId ||
							Previous->second.Dependencies != Next.Dependencies) SemanticallyChanged.insert(Next.Id);
					}
					bool Propagated = true;
					while (Propagated) {
						Propagated = false;
						for (auto &Next : Prepared->Records) {
							if (SemanticallyChanged.contains(Next.Id) ||
								!std::ranges::any_of(Next.Dependencies, [&](AssetId Id) { return SemanticallyChanged.contains(Id); })) continue;
							if (Next.ContentRevision == std::numeric_limits<std::uint64_t>::max()) {
								CommitFailure = Error("RevisionExhausted", "Dependency revision propagation is exhausted");
								break;
							}
							++Next.ContentRevision;
							SemanticallyChanged.insert(Next.Id);
							Propagated = true;
						}
						if (!CommitFailure.Code.empty()) break;
					}
					if (!CommitFailure.Code.empty()) {
						for (const auto &Previous : PreviousRecords) {
							auto Restored = Previous;
							Restored.State = AssetState::Stale;
							Restored.Diagnostic = CommitFailure;
							State->Records.insert_or_assign(Restored.Reference.Value, Restored);
							State->PublishChange(Restored, AssetChangeKind::StateChanged);
							Operation.Records.push_back(Restored);
							if (Restored.PrimarySourceAsset) Operation.Record = Restored;
						}
						Operation.Diagnostic = CommitFailure;
					} else {
						for (const auto &Previous : PreviousRecords) if (!NextIds.contains(Previous.Id)) {
							State->RemoveResidency(Previous);
							State->Records.erase(Previous.Reference.Value);
							auto Removed = Previous;
							Removed.State = AssetState::Missing;
							State->PublishChange(Removed, AssetChangeKind::Removed);
						}
						for (const auto &[Content, Artifact] : Prepared->Artifacts)
							State->Artifacts.insert_or_assign(Content, Artifact);
						for (const auto &Next : Prepared->Records) {
							auto Previous = PreviousByKey.find(Next.LogicalKey);
							const bool NewRecord = Previous == PreviousByKey.end();
							if (!NewRecord && ContentChanged.contains(Next.Id)) State->QueueResidency(Next, true);
							else if (NewRecord) State->QueueResidency(Next, false);
							State->Records.insert_or_assign(Next.Reference.Value, Next);
							const auto ChangeKind = NewRecord ? AssetChangeKind::Added :
								(ContentChanged.contains(Next.Id) ? AssetChangeKind::ContentChanged :
								(SemanticallyChanged.contains(Next.Id) ? AssetChangeKind::DependencyChanged : AssetChangeKind::StateChanged));
							State->PublishChange(Next, ChangeKind);
							if (Next.PrimarySourceAsset) Operation.Record = Next;
						}
						State->CpuBytes = State->CpuBytes - PreviousBytes + Prepared->CpuBytes;
						Operation.Ok = true;
						Operation.Records = Prepared->Records;
					}
				}
			}
		}
		if (auto World = GetDataModel()) {
			World->AdvanceAuthoritativeRevision();
			if (Operation.Ok && !SemanticallyChanged.empty()) {
				for (const auto &Object : World->GetDescendants()) if (auto Mesh = std::dynamic_pointer_cast<MeshPart>(Object)) {
					auto MeshReference = AssetReference::Parse(Mesh->GetMesh());
					auto MaterialReference = AssetReference::Parse(Mesh->GetMaterial());
					RenderUpdateDomain Domains = RenderUpdateDomain::None;
					if (MeshReference && MeshReference->ProjectAsset && SemanticallyChanged.contains(*MeshReference->ProjectAsset))
						Domains = Domains | (ContentChanged.contains(*MeshReference->ProjectAsset) ?
							RenderUpdateDomain::Geometry : RenderUpdateDomain::Material);
					if (MaterialReference && MaterialReference->ProjectAsset && SemanticallyChanged.contains(*MaterialReference->ProjectAsset))
						Domains = Domains | RenderUpdateDomain::Material;
					if (Domains != RenderUpdateDomain::None)
						RenderDirtyAccumulator::Get().Mark(World->GetReplicationScopeId(), Mesh->GetObjectId(), Domains);
				}
			}
		}
		return Operation;
	}

	AssetOperationResult AssetService::DeleteProjectAsset(std::string_view Reference) {
		AssetOperationResult Operation;
		{
			std::scoped_lock Lock(State->Mutex);
			auto Existing = State->Records.find(std::string(Reference));
			if (Existing == State->Records.end() || Existing->second.BuiltIn)
				return {false, std::nullopt, Error("UnknownAsset", "Project asset reference is unknown")};
			if (State->InFlightGroups.contains(Existing->second.SourceGroupId))
				return {false, Existing->second, Error("ImportInProgress", "Asset source group is being imported")};
			std::vector<AssetRecord> Group;
			std::unordered_set<AssetId, AssetIdHash> GroupIds;
			for (const auto &[RecordReference, Record] : State->Records) if (!Record.BuiltIn &&
				Record.SourceGroupId == Existing->second.SourceGroupId) {
				(void)RecordReference;
				Group.push_back(Record);
				GroupIds.insert(Record.Id);
			}
			for (const auto &[RecordReference, Record] : State->Records) {
				(void)RecordReference;
				if (GroupIds.contains(Record.Id)) continue;
				for (const auto Dependency : Record.Dependencies) if (GroupIds.contains(Dependency))
					return {false, Existing->second, Error("AssetReferenced", "Asset source group has a live catalog dependent")};
			}
			if (auto World = GetDataModel()) {
				for (const auto &Object : World->GetDescendants()) {
					auto IsGroupReference = [&](std::string_view Value) {
						auto Parsed = AssetReference::Parse(Value);
						return Parsed && Parsed->ProjectAsset && GroupIds.contains(*Parsed->ProjectAsset);
					};
					if (auto Image = std::dynamic_pointer_cast<ImageLabel>(Object); Image && IsGroupReference(Image->GetImage()))
						return {false, Existing->second, Error("AssetReferenced", "Asset source group is referenced by ImageLabel")};
					if (auto Text = std::dynamic_pointer_cast<TextLabel>(Object); Text && IsGroupReference(Text->GetFontFace()))
						return {false, Existing->second, Error("AssetReferenced", "Asset source group is referenced by a text object")};
					if (auto Mesh = std::dynamic_pointer_cast<MeshPart>(Object); Mesh &&
						(IsGroupReference(Mesh->GetMesh()) || IsGroupReference(Mesh->GetMaterial())))
						return {false, Existing->second, Error("AssetReferenced", "Asset source group is referenced by MeshPart")};
				}
			}
			for (auto Removed : Group) {
				if (Removed.Asset) State->CpuBytes -= AssetBytes(*Removed.Asset);
				State->RemoveResidency(Removed);
				State->Records.erase(Removed.Reference.Value);
				Removed.State = AssetState::Missing;
				State->PublishChange(Removed, AssetChangeKind::Removed);
				Operation.Records.push_back(Removed);
				if (Removed.PrimarySourceAsset) Operation.Record = Removed;
			}
			Operation.Ok = true;
		}
		if (auto World = GetDataModel()) World->AdvanceAuthoritativeRevision();
		return Operation;
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

	std::optional<AssetMeshResource> AssetService::ResolveMeshResource(std::string_view Reference) const {
		std::scoped_lock Lock(State->Mutex);
		auto Record = State->Records.find(std::string(Reference));
		if (Record == State->Records.end() || !IsAvailableRecord(Record->second) || !Record->second.Asset)
			return std::nullopt;
		const auto *Mesh = std::get_if<ImportedMesh>(Record->second.Asset.get());
		const auto Residency = State->Meshes.find(Record->first);
		if (!Mesh || Residency == State->Meshes.end()) return std::nullopt;
		return AssetMeshResource{Residency->second.Identity, *Mesh, Record->second.ContentRevision};
	}

	std::optional<AssetMaterialResource> AssetService::ResolveMaterial(std::string_view Reference) {
		std::scoped_lock Lock(State->Mutex);
		if (Reference.empty()) Reference = DefaultMaterialReference;
		auto Record = State->Records.find(std::string(Reference));
		if (Record == State->Records.end() || !IsAvailableRecord(Record->second) || !Record->second.Asset ||
			!std::holds_alternative<ImportedMaterial>(*Record->second.Asset))
			Record = State->Records.find(std::string(DefaultMaterialReference));
		if (Record == State->Records.end() || !Record->second.Asset) return std::nullopt;
		const auto *Material = std::get_if<ImportedMaterial>(Record->second.Asset.get());
		if (!Material) return std::nullopt;
		RenderMaterialState Render;
		Render.Revision = Record->second.ContentRevision;
		Render.BaseColorFactor = Material->BaseColorFactor;
		Render.Metallic = Material->MetallicFactor;
		Render.Roughness = Material->RoughnessFactor;
		Render.AlphaCutoff = Material->AlphaCutoff;
		Render.DoubleSided = Material->DoubleSided;
		switch (Material->AlphaMode) {
			case AssetMaterialAlphaMode::Opaque: Render.OpacityMode = RenderOpacityMode::Opaque; break;
			case AssetMaterialAlphaMode::Mask: Render.OpacityMode = RenderOpacityMode::Masked; break;
			case AssetMaterialAlphaMode::Blend: Render.OpacityMode = RenderOpacityMode::Transparent; break;
		}
		auto ResolveTexture = [&](const std::optional<AssetId> &Id) -> std::optional<RenderTextureIdentity> {
			if (!Id) return std::nullopt;
			const auto Texture = State->Textures.find(AssetReference::FromAssetId(*Id).Value);
			return Texture == State->Textures.end() ? std::nullopt : std::optional(Texture->second.Identity);
		};
		Render.BaseColorTexture = ResolveTexture(Material->BaseColorTexture);
		Render.NormalTexture = ResolveTexture(Material->NormalTexture);
		return AssetMaterialResource{*Material, Render, Record->second.ContentRevision};
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
		Record.SourceGroupId = Id;
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
		Record.SourceGroupId = Id;
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
			Json Dependencies = Json::array();
			for (const auto Dependency : Record->Dependencies) Dependencies.push_back(Dependency.ToString());
			Assets.push_back({
				{"AssetId", Record->Id.ToString()}, {"Reference", Record->Reference.Value},
				{"Kind", GetAssetKindName(Record->Kind)}, {"Name", Record->Name}, {"Source", Record->Source},
				{"ContentId", Record->ContentId.IsValid() ? Json(Record->ContentId.ToString()) : Json(nullptr)},
				{"ContentRevision", Record->ContentRevision},
				{"State", GetAssetStateName(Record->State)}, {"Diagnostic", std::move(Diagnostic)},
				{"SourceGroupId", Record->SourceGroupId.ToString()}, {"LogicalKey", Record->LogicalKey},
				{"PrimarySourceAsset", Record->PrimarySourceAsset}, {"Dependencies", std::move(Dependencies)},
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
			} else if constexpr (std::is_same_v<T, ImportedFont>) {
				lua_pushnumber(L, Asset.FaceCount); lua_setfield(L, -2, "FaceCount");
			} else {
				lua_pushnumber(L, Asset.MetallicFactor); lua_setfield(L, -2, "MetallicFactor");
				lua_pushnumber(L, Asset.RoughnessFactor); lua_setfield(L, -2, "RoughnessFactor");
				lua_pushboolean(L, Asset.DoubleSided); lua_setfield(L, -2, "DoubleSided");
			}
		}, *Record->Asset);
		return 1;
	}
}
