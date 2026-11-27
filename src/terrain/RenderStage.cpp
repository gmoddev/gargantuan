// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "terrain/RenderStage.hpp"
#include "terrain/TerrainMaterialPalette.hpp"
#include "gargantuan/runtime/JobSystem.hpp"

#include "gargantuan/Profiler.hpp"

#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>

namespace gargantuan::terrain {
    namespace {
        constexpr std::size_t MaximumRenderStateBytes = 32 * 1024 * 1024;
        // Linear swatches. Field material identities remain independent.
        constexpr std::array<std::array<std::uint8_t, 4>, 6> Swatches{{
            {107, 99, 82, 255},
            {61, 94, 41, 255},
            {97, 94, 84, 255},
            {89, 59, 36, 255},
            {161, 135, 79, 255},
            {199, 209, 217, 255},
        }};
        constexpr std::array<std::uint8_t, 4> White{255, 255, 255, 255};
        std::atomic<std::uint64_t> NextSlot{1};

        std::optional<RenderMeshIdentity> AllocateIdentity(const RenderChunk *Previous) noexcept {
            constexpr auto MaxGeneration = std::numeric_limits<std::uint32_t>::max();
            if (Previous && Previous->Resource.Mesh.Generation < MaxGeneration) {
                const auto &Old = Previous->Resource.Mesh;
                return RenderMeshIdentity{Old.Slot, Old.Generation + 1, RenderMeshDomain::Terrain};
            }
            auto Slot = NextSlot.load(std::memory_order_relaxed);
            while (Slot != std::numeric_limits<std::uint64_t>::max()) {
                if (NextSlot.compare_exchange_weak(Slot, Slot + 1, std::memory_order_relaxed))
                    return RenderMeshIdentity{Slot, 1, RenderMeshDomain::Terrain};
            }
            return std::nullopt;
        }

        RenderMaterialState GetMaterial(Revision Revision) noexcept {
            RenderMaterialState Material;
            Material.Revision = Revision;
            Material.ShadingModel = RenderShadingModel::MetallicRoughness;
            Material.Roughness = 0.92f;
            return Material;
        }

        struct MaterialRanges {
            std::shared_ptr<const std::vector<std::uint32_t>> Indices;
            std::shared_ptr<const std::vector<RenderPrimitiveMaterialState>> Primitives;
            RenderMaterialState Material;
        };

        std::expected<MaterialRanges, EditStatus> MakeMaterialRanges(const SurfaceMesh &Surface,
            const TerrainMaterialPalette &Palette, std::span<const std::uint8_t> Slots, std::vector<RenderVertex> &Vertices,
            RenderMaterialState Default, std::size_t ExtraBytes) {
            constexpr auto BinCount = TerrainMaterialPalette::MaximumBindings + 1;
            constexpr auto UnmappedIndex = std::numeric_limits<std::uint32_t>::max();
            std::array<std::uint32_t, BinCount> Counts{};
            std::vector<std::uint8_t> TriangleBins(Surface.Indices.size() / 3);
            for (std::size_t Triangle = 0; Triangle < TriangleBins.size(); ++Triangle) {
                const auto First = Triangle * 3;
                const auto Slot = Slots[Surface.Indices[First]];
                const auto Bin = Slot == Slots[Surface.Indices[First + 1]] && Slot == Slots[Surface.Indices[First + 2]]
                    ? Slot : std::uint8_t{};
                TriangleBins[Triangle] = Bin;
                Counts[Bin] += 3;
            }
            std::size_t Selected = 0;
            for (std::size_t Bin = 1; Bin < BinCount; ++Bin) {
                if (!Counts[Bin]) continue;
                if (Selected < TerrainMaterialPalette::MaximumChunkMaterials) {
                    ++Selected;
                } else {
                    Counts[0] += Counts[Bin];
                    Counts[Bin] = 0;
                }
            }
            if (!Selected)
                return MaterialRanges{std::make_shared<const std::vector<std::uint32_t>>(Surface.Indices), {}, Default};
            if (Selected == 1 && Counts[0] == 0) {
                for (auto &Vertex : Vertices) Vertex.Tint = White;
                return MaterialRanges{std::make_shared<const std::vector<std::uint32_t>>(Surface.Indices), {},
                    Palette.GetMaterial(TriangleBins.front())};
            }

            constexpr std::uint8_t UsedByFallback = 1;
            constexpr std::uint8_t UsedByMapped = 2;
            std::vector<std::uint8_t> Uses(Vertices.size());
            for (std::size_t Triangle = 0; Triangle < TriangleBins.size(); ++Triangle) {
                if (Counts[TriangleBins[Triangle]] == 0) TriangleBins[Triangle] = 0;
                const auto Use = TriangleBins[Triangle] == 0 ? UsedByFallback : UsedByMapped;
                for (auto Corner = Triangle * 3; Corner < Triangle * 3 + 3; ++Corner)
                    Uses[Surface.Indices[Corner]] |= Use;
            }

            const auto Duplicates = static_cast<std::size_t>(std::ranges::count(Uses, UsedByFallback | UsedByMapped));
            const auto PrimitiveCount = Selected + (Counts[0] > 0);
            if (Duplicates * sizeof(RenderVertex) + PrimitiveCount * sizeof(RenderPrimitiveMaterialState) > ExtraBytes)
                return std::unexpected(EditStatus::LimitExceeded);
            Vertices.reserve(Vertices.size() + Duplicates);
            auto Indices = std::make_shared<std::vector<std::uint32_t>>(Surface.Indices.size());
            auto Primitives = std::make_shared<std::vector<RenderPrimitiveMaterialState>>();
            Primitives->reserve(PrimitiveCount);
            std::array<std::uint32_t, BinCount> Offsets{};
            std::uint32_t First = 0;
            for (std::size_t Bin = 0; Bin < BinCount; ++Bin) {
                if (!Counts[Bin]) continue;
                const auto &Material = Bin == 0 ? Default : Palette.GetMaterial(static_cast<std::uint8_t>(Bin));
                Primitives->push_back({First, Counts[Bin], Material});
                Offsets[Bin] = First;
                First += Counts[Bin];
            }

            std::vector<std::uint32_t> MappedIndices(Surface.Vertices.size(), UnmappedIndex);
            for (std::size_t Triangle = 0; Triangle < TriangleBins.size(); ++Triangle) {
                const auto Bin = TriangleBins[Triangle];
                for (auto Corner = Triangle * 3; Corner < Triangle * 3 + 3; ++Corner) {
                    const auto Index = Surface.Indices[Corner];
                    auto OutputIndex = Index;
                    if (Bin != 0) {
                        auto &Mapped = MappedIndices[Index];
                        if (Mapped == UnmappedIndex) {
                            Mapped = Index;
                            if (Uses[Index] & UsedByFallback) {
                                Mapped = static_cast<std::uint32_t>(Vertices.size());
                                const auto Source = Vertices[Index];
                                Vertices.push_back(Source);
                            }
                            Vertices[Mapped].Tint = White;
                        }
                        OutputIndex = Mapped;
                    }
                    (*Indices)[Offsets[Bin]++] = OutputIndex;
                }
            }
            return MaterialRanges{std::move(Indices), std::move(Primitives), Default};
        }

        std::expected<RenderChunk, EditStatus> MakeChunk(ObjectId Owner, GridSpec Grid,
            const ChunkPublicationCandidate &Candidate, const RenderChunk *Previous,
            const TerrainMaterialPalette *Palette, TerrainMaterialRenderMode Mode, std::size_t BytesRemaining) {
            const auto Mesh = AllocateIdentity(Previous);
            if (!Mesh) return std::unexpected(EditStatus::LimitExceeded);
            const auto &Surface = *Candidate.Visual;
            if (Surface.Indices.empty() || Surface.Indices.size() % 3 != 0 || Surface.Vertices.empty())
                return std::unexpected(EditStatus::InvalidInput);
            if (Candidate.RenderBytesFloor > BytesRemaining || BytesRemaining < sizeof(RenderChunk) ||
                Surface.Vertices.size() > (BytesRemaining - sizeof(RenderChunk)) / sizeof(RenderVertex) ||
                Surface.Indices.size() > (BytesRemaining - sizeof(RenderChunk) -
                    Surface.Vertices.size() * sizeof(RenderVertex)) / sizeof(std::uint32_t))
                return std::unexpected(EditStatus::LimitExceeded);
            if (std::ranges::any_of(Surface.Indices, [&](std::uint32_t Index) {
                return Index >= Surface.Vertices.size();
            })) return std::unexpected(EditStatus::InvalidInput);

            auto Vertices = std::make_shared<std::vector<RenderVertex>>();
            Vertices->reserve(Surface.Vertices.size());
            const bool UsePbrRanges = Palette && Mode == TerrainMaterialRenderMode::PrimitivePbr;
            std::vector<std::uint8_t> Slots;
            if (UsePbrRanges) Slots.reserve(Surface.Vertices.size());
            RenderBounds Bounds{glm::vec3(std::numeric_limits<float>::infinity()),
                glm::vec3(-std::numeric_limits<float>::infinity())};
            for (const auto &Vertex : Surface.Vertices) {
                const glm::vec3 Position{Vertex.Position.X, Vertex.Position.Y, Vertex.Position.Z};
                const glm::vec3 Normal{Vertex.Normal.X, Vertex.Normal.Y, Vertex.Normal.Z};
                const auto Swatch = Vertex.Material < Swatches.size() ? Vertex.Material : 0;
                const auto Slot = Palette ? Palette->Find(Vertex.Material) : std::uint8_t{};
                if (UsePbrRanges) Slots.push_back(Slot);
                Vertices->push_back({.Position = Position, .Normal = Normal,
                    .Tint = Slot ? Palette->GetTint(Slot) : Swatches[Swatch]});
                Bounds.Minimum = glm::min(Bounds.Minimum, Position);
                Bounds.Maximum = glm::max(Bounds.Maximum, Position);
            }

            const auto Default = GetMaterial(Mesh->Generation);
            const auto ExtraBytes = BytesRemaining - sizeof(RenderChunk) -
                Surface.Vertices.size() * sizeof(RenderVertex) - Surface.Indices.size() * sizeof(std::uint32_t);
            auto MaybeRanges = UsePbrRanges ? MakeMaterialRanges(Surface, *Palette, Slots, *Vertices, Default, ExtraBytes) :
                MaterialRanges{std::make_shared<const std::vector<std::uint32_t>>(Surface.Indices), {}, Default};
            if (!MaybeRanges) return std::unexpected(MaybeRanges.error());
            auto &Ranges = *MaybeRanges;

            const auto Step = ChunkSide * Grid.GetSpacing();
            const glm::vec3 Origin{
                static_cast<float>(Candidate.Key.X * Step), static_cast<float>(Candidate.Key.Y * Step),
                static_cast<float>(Candidate.Key.Z * Step)
            };
            RenderItem Item{.Object = Owner, .ModelMatrix = glm::translate(glm::mat4(1.f), Origin),
                .InverseModelMatrix = glm::translate(glm::mat4(1.f), -Origin)};
            const auto Bytes = sizeof(RenderChunk) + Vertices->capacity() * sizeof(RenderVertex) +
                Ranges.Indices->capacity() * sizeof(std::uint32_t) +
                (Ranges.Primitives ? Ranges.Primitives->capacity() * sizeof(RenderPrimitiveMaterialState) : 0);
            if (Bytes > BytesRemaining) return std::unexpected(EditStatus::LimitExceeded);
            return RenderChunk{
                {*Mesh, Mesh->Generation, Mesh->Generation, std::move(Vertices), std::move(Ranges.Indices), Bounds},
                {Item, Mesh, Ranges.Material, true, std::move(Ranges.Primitives)}, Bytes,
                std::max(Bytes, Candidate.RenderBytesFloor), Candidate.Visual};
        }

        std::shared_ptr<const RenderTerrainDraws> MakeDraws(const std::map<ChunkKey, RenderChunk> &Chunks) {
            auto Draws = std::make_shared<RenderTerrainDraws>();
            Draws->reserve(Chunks.size());
            for (const auto &[Key, Value] : Chunks) {
                static_cast<void>(Key);
                Draws->push_back(Value.Draw);
            }
            std::ranges::sort(*Draws, {}, &RenderObjectCreate::Mesh);
            return Draws;
        }
    }

    struct RenderStage::Impl {
        ObjectId Owner;
        std::shared_ptr<const TerrainMaterialPalette> Palette;
        TerrainMaterialRenderMode Mode{TerrainMaterialRenderMode::Tint};
        std::shared_ptr<RenderState> Current;
        bool Alive{true};
        bool Pending{};
        bool UnboundPreview{};
    };

    class RenderStage::Token final : public StagedPublication {
    public:
        Token(std::shared_ptr<Impl> Owner, std::shared_ptr<RenderState> Value)
            : Owner(std::move(Owner)), Value(std::move(Value)) {}
        ~Token() override { if (Value) Owner->Pending = false; }
        void Install(Revision) noexcept override {
            if (!Value || !Owner->Alive) return;
            Owner->Current = std::move(Value);
            Owner->Pending = false;
            Installed = true;
        }
        [[nodiscard]] bool IsInstalled() const noexcept override { return Installed; }
    private:
        std::shared_ptr<Impl> Owner;
        std::shared_ptr<RenderState> Value;
        bool Installed{};
    };

    RenderStage::RenderStage(ObjectId Owner) : RenderStage(Owner, {}) {}
    RenderStage::RenderStage(ObjectId Owner, std::shared_ptr<const TerrainMaterialPalette> Palette, TerrainMaterialRenderMode Mode)
        : State(std::make_shared<Impl>(Impl{Owner, std::move(Palette), Mode})) {}
    RenderStage::RenderStage() : State(std::make_shared<Impl>(Impl{.UnboundPreview = true})) {}
    RenderStage::~RenderStage() {
        State->Alive = false;
        State->Current.reset();
    }

    std::shared_ptr<const RenderState> RenderStage::Capture() const noexcept { return State->Current; }

    std::expected<std::unique_ptr<StagedPublication>, EditStatus> RenderStage::Stage(GridSpec Grid,
        std::span<const ChunkPublicationCandidate> Chunks) {
        return StageVisualDensity(Grid, Chunks, {});
    }

    std::expected<std::unique_ptr<StagedPublication>, EditStatus> RenderStage::StageVisualDensity(GridSpec Grid,
        std::span<const ChunkPublicationCandidate> Chunks, std::span<const RenderVisualChange> Changes) {
        G_PROFILE("Terrain::StageRender");
        if (State->Pending) return std::unexpected(EditStatus::Unavailable);
        if ((!State->Owner.IsValid() && !State->UnboundPreview) || !Grid.IsValid() ||
            Chunks.size() > MaximumRenderTerrainChunks || Changes.size() > 2 * MaximumVisualVariants)
            return std::unexpected(EditStatus::InvalidInput);
        try {
            auto Candidate = State->Current ? std::make_shared<RenderState>(*State->Current) : std::make_shared<RenderState>();
            std::optional<ChunkKey> Previous;
            for (const auto &Chunk : Chunks) {
                if (!IsValid(Chunk.Key) || (Previous && Chunk.Key <= *Previous))
                    return std::unexpected(EditStatus::InvalidInput);
                Previous = Chunk.Key;
                const auto Found = Candidate->Chunks.find(Chunk.Key);
                if (Found != Candidate->Chunks.end()) {
                    Candidate->Bytes -= Found->second.Bytes;
                    Candidate->AdmissionBytes -= Found->second.AdmissionBytes;
                }
            }
            for (const auto &Chunk : Chunks) {
                const auto Found = Candidate->Chunks.find(Chunk.Key);
                const auto Variant = Candidate->VisualVariants.find(Chunk.Key);
                const auto *PriorFine = Variant != Candidate->VisualVariants.end() ? &Variant->second.Fine :
                    (Found != Candidate->Chunks.end() ? &Found->second : nullptr);
                if (!Chunk.Visual) {
                    if (Found != Candidate->Chunks.end()) Candidate->Chunks.erase(Found);
                } else {
                    auto Next = MakeChunk(State->Owner, Grid, Chunk, PriorFine, State->Palette.get(), State->Mode,
                        MaximumRenderStateBytes - Candidate->AdmissionBytes);
                    if (!Next) return std::unexpected(Next.error());
                    Candidate->Bytes += Next->Bytes;
                    Candidate->AdmissionBytes += Next->AdmissionBytes;
                    Candidate->Chunks.insert_or_assign(Chunk.Key, std::move(*Next));
                }
                if (Variant != Candidate->VisualVariants.end()) {
                    Candidate->VisualVariantBytes -= Variant->second.Reduced.Bytes;
                    Candidate->VisualVariants.erase(Variant);
                }
            }
            if (Candidate->Chunks.size() > MaximumRenderTerrainChunks)
                return std::unexpected(EditStatus::LimitExceeded);

            bool DrawsChanged = !Chunks.empty();
            bool ResourcesChanged = false;
            std::size_t WarmCount = 0;
            std::size_t WarmBytes = 0;
            Previous.reset();
            for (const auto &Change : Changes) {
                if (!IsValid(Change.Key) || (Previous && Change.Key <= *Previous) ||
                    std::ranges::binary_search(Chunks, Change.Key, {}, &ChunkPublicationCandidate::Key))
                    return std::unexpected(EditStatus::InvalidInput);
                Previous = Change.Key;
                const auto At = Candidate->Chunks.find(Change.Key);
                if (At == Candidate->Chunks.end()) return std::unexpected(EditStatus::Stale);
                const auto Pair = Candidate->VisualVariants.find(Change.Key);
                const auto &Fine = Pair == Candidate->VisualVariants.end() ? At->second : Pair->second.Fine;
                if (Fine.Resource.Mesh != Change.ExpectedFine) return std::unexpected(EditStatus::Stale);
                const auto Bind = [&](const RenderChunk &Value) {
                    if (At->second.Resource.Mesh == Value.Resource.Mesh) return;
                    Candidate->Bytes = Candidate->Bytes - At->second.Bytes + Value.Bytes;
                    At->second = Value;
                    At->second.AdmissionBytes = Fine.AdmissionBytes;
                    DrawsChanged = true;
                };
                if (Change.Action == RenderVisualAction::Warm) {
                    if (++WarmCount > MaximumWarmVariants || Pair != Candidate->VisualVariants.end() ||
                        !Change.Fine || !Change.Reduced)
                        return std::unexpected(EditStatus::InvalidInput);
                    const auto Source = Fine.Source.lock();
                    if (Source != Change.Fine || Source.owner_before(Change.Fine) || Change.Fine.owner_before(Source))
                        return std::unexpected(EditStatus::Stale);
                    if (Change.Fine->Spacing != Grid.GetSpacing() || Change.Reduced->Spacing != Grid.GetSpacing() ||
                        Change.Reduced->Indices.size() >= Change.Fine->Indices.size())
                        return std::unexpected(EditStatus::InvalidInput);
                    if (Candidate->VisualVariants.size() >= MaximumVisualVariants || Fine.Bytes == 0)
                        continue;
                    const auto Remaining = std::min({MaximumVisualVariantBytes - Candidate->VisualVariantBytes,
                        MaximumWarmBytes - WarmBytes, Fine.Bytes - 1});
                    const ChunkPublicationCandidate Input{Change.Key, Change.Reduced, {}, false};
                    auto Reduced = MakeChunk(State->Owner, Grid, Input, nullptr, State->Palette.get(), State->Mode, Remaining);
                    if (!Reduced) {
                        if (Reduced.error() == EditStatus::LimitExceeded) continue;
                        return std::unexpected(Reduced.error());
                    }
                    WarmBytes += Reduced->Bytes;
                    Candidate->VisualVariantBytes += Reduced->Bytes;
                    Candidate->VisualVariants.emplace(Change.Key, RenderVisualVariant{Fine, std::move(*Reduced)});
                    ResourcesChanged = true;
                    continue;
                }
                if (Pair == Candidate->VisualVariants.end()) return std::unexpected(EditStatus::Stale);
                if (Change.Action == RenderVisualAction::Reduced) {
                    // Selection is tied to a pair from the previous publication.
                    const auto Source = Fine.Source.lock();
                    if (!Source || Source != Change.Fine || Source.owner_before(Change.Fine) || Change.Fine.owner_before(Source))
                        return std::unexpected(EditStatus::Stale);
                    Bind(Pair->second.Reduced);
                } else {
                    if (Change.Action != RenderVisualAction::Fine && Change.Action != RenderVisualAction::Evict)
                        return std::unexpected(EditStatus::InvalidInput);
                    Bind(Fine);
                    if (Change.Action == RenderVisualAction::Evict) {
                        Candidate->VisualVariantBytes -= Pair->second.Reduced.Bytes;
                        Candidate->VisualVariants.erase(Pair);
                        ResourcesChanged = true;
                    }
                }
            }
            if (DrawsChanged || !Candidate->Draws) Candidate->Draws = MakeDraws(Candidate->Chunks);
            if (State->Current && !DrawsChanged && !ResourcesChanged)
                Candidate = State->Current;
            auto Prepared = std::make_unique<Token>(State, std::move(Candidate));
            State->Pending = true;
            return Prepared;
        } catch (const std::bad_alloc &) {
            return std::unexpected(EditStatus::LimitExceeded);
        }
    }

    std::expected<TerrainRenderPreview, EditStatus> TerrainRenderPreview::Prepare(const Field &Storage) {
        TerrainRenderPreview Preview;
        if (Storage.GetStats().MaterializedPages == 0) return Preview;
        try {
            JobSystem Jobs(2, JobSystem::StartPolicy::OnFirstSubmission);
            RenderStage Stage;
            if (auto Ready = PrepareTerrainPreviewCohorts(Storage, Jobs, Stage); !Ready)
                return std::unexpected(Ready.error());
            Preview.State = std::move(Stage.State->Current);
            if (Preview.State && Preview.State->Draws) {
                Preview.Draws = std::make_shared<RenderTerrainDraws>(*Preview.State->Draws);
                Preview.State->Draws = Preview.Draws;
            }
            return Preview;
        } catch (const std::bad_alloc &) {
            return std::unexpected(EditStatus::LimitExceeded);
        } catch (...) {
            return std::unexpected(EditStatus::PreparationFailed);
        }
    }

    std::shared_ptr<const RenderState> TerrainRenderPreview::Bind(ObjectId Owner) && noexcept {
        if (!State || !Draws || !Owner.IsValid()) return nullptr;
        for (auto &[Key, Chunk] : State->Chunks) {
            static_cast<void>(Key);
            Chunk.Draw.Item.Object = Owner;
        }
        for (auto &Draw : *Draws) Draw.Item.Object = Owner;
        Draws.reset();
        return std::move(State);
    }

    std::shared_ptr<const RenderState> GetFineTerrainRenderState(const std::shared_ptr<const RenderState> &State) {
        if (!State || State->VisualVariants.empty()) return State;
        auto Fine = std::make_shared<RenderState>(*State);
        bool DrawsChanged = false;
        for (const auto &[Key, Variant] : State->VisualVariants) {
            auto &Selected = Fine->Chunks.at(Key);
            if (Selected.Resource.Mesh == Variant.Fine.Resource.Mesh) continue;
            Fine->Bytes = Fine->Bytes - Selected.Bytes + Variant.Fine.Bytes;
            Selected = Variant.Fine;
            DrawsChanged = true;
        }
        Fine->VisualVariants.clear();
        Fine->VisualVariantBytes = 0;
        if (DrawsChanged) Fine->Draws = MakeDraws(Fine->Chunks);
        return Fine;
    }

    void AppendTerrainRenderState(RenderPublication &Publication, const std::shared_ptr<const RenderState> &Before,
        const std::shared_ptr<const RenderState> &After) {
        if (!Publication.FullResync && Before == After) return;
        const auto VisitResources = [](const RenderState &State, auto Visit) {
            for (const auto &[Key, Value] : State.Chunks) {
                const auto Pair = State.VisualVariants.find(Key);
                if (Pair == State.VisualVariants.end()) {
                    Visit(Key, Value.Resource);
                } else {
                    Visit(Key, Pair->second.Fine.Resource);
                    Visit(Key, Pair->second.Reduced.Resource);
                }
            }
        };
        const auto Contains = [](const RenderState &State, ChunkKey Key, RenderMeshIdentity Mesh) {
            const auto Pair = State.VisualVariants.find(Key);
            if (Pair != State.VisualVariants.end())
                return Pair->second.Fine.Resource.Mesh == Mesh || Pair->second.Reduced.Resource.Mesh == Mesh;
            const auto Chunk = State.Chunks.find(Key);
            return Chunk != State.Chunks.end() && Chunk->second.Resource.Mesh == Mesh;
        };
        if (After) {
            VisitResources(*After, [&](ChunkKey Key, const RenderMeshCreate &Resource) {
                if (Publication.FullResync || !Before || !Contains(*Before, Key, Resource.Mesh))
                    Publication.MeshCreates.push_back(Resource);
            });
            if (Publication.FullResync || !Before || Before->Draws != After->Draws)
                Publication.TerrainDraws = After->Draws;
        } else if (!Publication.FullResync && Before) {
            static const auto Empty = std::make_shared<const RenderTerrainDraws>();
            Publication.TerrainDraws = Empty;
        }
        if (!Publication.FullResync && Before) {
            VisitResources(*Before, [&](ChunkKey Key, const RenderMeshCreate &Resource) {
                if (!After || !Contains(*After, Key, Resource.Mesh))
                    Publication.MeshRemoves.push_back({Resource.Mesh});
            });
        }
    }
}
