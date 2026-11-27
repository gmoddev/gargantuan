// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "terrain/EditCoordinator.hpp"
#include "terrain/TerrainProfile.hpp"
#include "gargantuan/Profiler.hpp"
#include "gargantuan/runtime/JobSystem.hpp"
#include "gargantuan/terrain/ActiveChunkRegistry.hpp"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <limits>
#include <set>
#include <type_traits>

namespace gargantuan::terrain {
    namespace {
        constexpr std::size_t MaximumOutstanding = 4;
        constexpr std::size_t MaximumCohortChunks = 8;
        constexpr std::size_t MaximumActiveChunks = 512;
        constexpr std::size_t MaximumCohortGeometryBytes = 16 * 1024 * 1024;
        constexpr std::size_t MaximumResidentVisualBytes = 64 * 1024 * 1024;
        constexpr int SurfaceSampleSide = ChunkSide + 3;
        constexpr int SurfaceSampleCount = SurfaceSampleSide * SurfaceSampleSide * SurfaceSampleSide;
        constexpr SurfaceLimits MeshLimits{ChunkSide * ChunkSide * ChunkSide, 65'536, 262'143};

        struct Target {
            detail::ChunkWorkTicket Activity;
            std::shared_ptr<const SurfaceMesh> Previous;
        };

        struct Cohort {
            PreparedEdit Candidate;
            std::vector<ChunkPublicationCandidate> Chunks;
        };

        struct PendingEdit {
            PendingEdit(EditTicket Number, std::optional<Revision> ExpectedRevision)
                : Number(Number), ExpectedRevision(ExpectedRevision) {}
            EditTicket Number;
            std::optional<Revision> ExpectedRevision;
            std::stop_source Stop;
            std::vector<ChunkKey> Dependencies;
            std::vector<Target> Targets;
            std::expected<Cohort, EditStatus> Output{std::unexpected(EditStatus::PreparationFailed)};
            std::atomic<bool> Finished{false};
        };

        EditStatus Convert(FieldError Error) noexcept {
            switch (Error) {
                case FieldError::LimitExceeded:
                case FieldError::OutOfMemory: return EditStatus::LimitExceeded;
                case FieldError::StaleDependency:
                case FieldError::WrongField: return EditStatus::Stale;
                case FieldError::RevisionExhausted: return EditStatus::Unavailable;
                default: return EditStatus::InvalidInput;
            }
        }

        EditStatus Convert(SurfaceStatus Status) noexcept {
            switch (Status) {
                case SurfaceStatus::Canceled: return EditStatus::Canceled;
                case SurfaceStatus::WorkLimit:
                case SurfaceStatus::AllocationFailure: return EditStatus::LimitExceeded;
                default: return EditStatus::PreparationFailed;
            }
        }

        template <class Visit>
        void VisitContourOwners(ChunkKey Source, Visit Apply) {
            // Cells are owned by their minima. Low sample planes may feed a
            // contour in the neighboring chunk on the lower side.
            for (int X = -1; X <= 0; ++X)
                for (int Y = -1; Y <= 0; ++Y)
                    for (int Z = -1; Z <= 0; ++Z) {
                        const ChunkKey Target{Source.X + X, Source.Y + Y, Source.Z + Z};
                        if (IsValid(Target)) Apply(Target);
                    }
        }

        struct ReopenFootprint {
            std::size_t Sources{};
            std::map<ChunkKey, int> Contour;

            static std::expected<ReopenFootprint, EditStatus> Build(const Field &Storage) {
                const auto Materialized = Storage.GetMaterializedChunkKeys(MaximumStartupSourceChunks);
                if (!Materialized)
                    return std::unexpected(Materialized.error() == FieldError::LimitExceeded
                        ? EditStatus::ReopenCapacityExceeded : Convert(Materialized.error()));
                ReopenFootprint Result{Materialized->size()};
                for (const auto Key : *Materialized) {
                    // A contour has at least one negative sample. Positive pages
                    // remain available to the halo of nearby solid pages.
                    if (!Storage.ContainsSolid(Key).value()) continue;
                    VisitContourOwners(Key, [&](ChunkKey Owner) { ++Result.Contour[Owner]; });
                    if (Result.Contour.size() > MaximumStartupContourChunks)
                        return std::unexpected(EditStatus::ReopenCapacityExceeded);
                }
                return Result;
            }

            std::expected<ReopenFootprint, EditStatus> Prepare(const Field &Storage, const PreparedEdit &Edit) const {
                ReopenFootprint Plan{Sources};
                for (const auto &Change : Edit.GetChanges()) {
                    const auto Before = Storage.IsMaterialized(Change.Key);
                    const auto After = Edit.GetPreview().IsMaterialized(Change.Key);
                    if (!Before || !After) return std::unexpected(EditStatus::PreparationFailed);
                    if (*Before != *After) {
                        if (*After) ++Plan.Sources;
                        else --Plan.Sources;
                    }
                    if (!Change.Geometry) continue;
                    const auto SolidBefore = Storage.ContainsSolid(Change.Key);
                    const auto SolidAfter = Edit.GetPreview().ContainsSolid(Change.Key);
                    if (!SolidBefore || !SolidAfter) return std::unexpected(EditStatus::PreparationFailed);
                    if (*SolidBefore == *SolidAfter) continue;
                    const int Difference = *SolidAfter ? 1 : -1;
                    VisitContourOwners(Change.Key, [&](ChunkKey Owner) { Plan.Contour[Owner] += Difference; });
                }
                if (Plan.Sources > MaximumStartupSourceChunks)
                    return std::unexpected(EditStatus::ReopenCapacityExceeded);
                auto Owners = Contour.size();
                for (auto &[Key, Count] : Plan.Contour) {
                    if (const auto Found = Contour.find(Key); Found != Contour.end()) {
                        Count += Found->second;
                        if (Count == 0) --Owners;
                    } else if (Count > 0) {
                        ++Owners;
                    }
                    if (Count < 0) return std::unexpected(EditStatus::PreparationFailed);
                }
                if (Owners > MaximumStartupContourChunks)
                    return std::unexpected(EditStatus::ReopenCapacityExceeded);
                return Plan;
            }

            void Install(ReopenFootprint Plan) noexcept {
                Sources = Plan.Sources;
                // All new nodes are allocated before the authoritative commit.
                while (!Plan.Contour.empty()) {
                    auto Node = Plan.Contour.extract(Plan.Contour.begin());
                    if (!Node.mapped()) Contour.erase(Node.key());
                    else if (const auto Found = Contour.find(Node.key()); Found != Contour.end())
                        Found->second = Node.mapped();
                    else
                        Contour.insert(std::move(Node));
                }
            }
        };

        std::expected<SampleBox, FieldError> GetWriteBounds(GridSpec Grid, const EditRequest &Request) {
            return std::visit([Grid](const auto &Typed) -> std::expected<SampleBox,FieldError> {
                using T = std::decay_t<decltype(Typed)>;
                if constexpr (std::is_same_v<T, SmoothEdit>) {
                    return Typed.Bounds;
                } else if constexpr (std::is_same_v<T, std::vector<SampleWrite>>) {
                    if (Typed.empty() || Typed.size() > MaximumEditSamples)
                        return std::unexpected(FieldError::LimitExceeded);
                    SampleBox Result{Typed.front().Point, Typed.front().Point};
                    for (const auto &Write : Typed) {
                        const auto P = Write.Point;
                        if (!IsValid(P)) return std::unexpected(FieldError::InvalidCoordinate);
                        Result.Min.X = std::min(Result.Min.X, P.X);
                        Result.Min.Y = std::min(Result.Min.Y, P.Y);
                        Result.Min.Z = std::min(Result.Min.Z, P.Z);
                        Result.MaxExclusive.X = std::max(Result.MaxExclusive.X, P.X + 1);
                        Result.MaxExclusive.Y = std::max(Result.MaxExclusive.Y, P.Y + 1);
                        Result.MaxExclusive.Z = std::max(Result.MaxExclusive.Z, P.Z + 1);
                    }
                    return Result;
                } else {
                    return GetBrushBounds(Grid, Typed);
                }
            }, Request);
        }

        SamplePoint GetOrigin(ChunkKey Key) noexcept {
            return {Key.X * ChunkSide, Key.Y * ChunkSide, Key.Z * ChunkSide};
        }

        SampleBox ExpandWithinField(SampleBox Bounds, int Halo) noexcept {
            constexpr auto Min = -MaximumSampleCoordinate;
            constexpr auto Max = MaximumSampleCoordinate + 1;
            return {{
                std::max(Min, Bounds.Min.X - Halo), std::max(Min, Bounds.Min.Y - Halo),
                std::max(Min, Bounds.Min.Z - Halo)
            }, {
                std::min(Max, Bounds.MaxExclusive.X + Halo), std::min(Max, Bounds.MaxExclusive.Y + Halo),
                std::min(Max, Bounds.MaxExclusive.Z + Halo)
            }};
        }

        std::expected<std::vector<ChunkKey>, FieldError> GetDependencies(std::span<const ChunkKey> Targets) {
            std::vector<ChunkKey> Result;
            Result.reserve(Targets.size() * 27);
            for (const auto Key : Targets) {
                const auto Origin = GetOrigin(Key);
                const auto Keys = GetRegionChunks(ExpandWithinField({Origin, {
                    Origin.X + ChunkSide + 1, Origin.Y + ChunkSide + 1, Origin.Z + ChunkSide + 1
                }}, 1));
                if (!Keys) return std::unexpected(Keys.error());
                Result.insert(Result.end(), Keys->begin(), Keys->end());
            }
            std::ranges::sort(Result);
            Result.erase(std::unique(Result.begin(), Result.end()), Result.end());
            if (Result.size() > MaximumCapturedChunks) return std::unexpected(FieldError::LimitExceeded);
            return Result;
        }

        bool Intersects(std::span<const ChunkKey> A, std::span<const ChunkKey> B) {
            auto I = A.begin(), J = B.begin();
            while (I != A.end() && J != B.end()) {
                if (*I == *J) return true;
                if (*I < *J) ++I;
                else ++J;
            }
            return false;
        }

        std::size_t CountVisualBytes(const std::shared_ptr<const SurfaceMesh> &Mesh) noexcept {
            return Mesh ? sizeof(SurfaceMesh) + Mesh->Vertices.capacity() * sizeof(SurfaceVertex) +
                Mesh->Indices.capacity() * sizeof(std::uint32_t) : 0;
        }

        std::size_t CountGeometryBytes(const ChunkPublicationCandidate &Chunk) noexcept {
            return CountVisualBytes(Chunk.Visual) +
                Chunk.Collision.Positions.capacity() * sizeof(SurfacePoint) +
                Chunk.Collision.Indices.capacity() * sizeof(std::uint32_t) +
                Chunk.Collision.TriangleMaterials.capacity() * sizeof(MaterialId);
        }

        std::expected<ChunkPublicationCandidate, EditStatus> PrepareChunk(const Snapshot &Source,
            ChunkKey Key, const SurfaceMesh *Previous, std::span<Sample> Samples, std::stop_token Stop = {},
            bool NeedCollision = true) {
            if (Stop.stop_requested()) return std::unexpected(EditStatus::Canceled);
            const auto [X, Y, Z] = GetOrigin(Key);
            std::size_t At = 0;
            bool ContainsSolid = false;
            for (int K = -1; K <= ChunkSide + 1; ++K)
                for (int J = -1; J <= ChunkSide + 1; ++J)
                    for (int I = -1; I <= ChunkSide + 1; ++I) {
                        const SamplePoint Point{X + I, Y + J, Z + K};
                        if (!IsValid(Point)) {
                            Samples[At++] = AirSample;
                            continue;
                        }
                        const auto Value = Source.Read(Point);
                        if (!Value) return std::unexpected(Convert(Value.error()));
                        ContainsSolid |= Value->Density < 0;
                        Samples[At++] = *Value;
                    }

            ChunkPublicationCandidate Chunk{Key, {}, {}, Previous == nullptr};
            if (!ContainsSolid) return Chunk;
            auto Visual = Previous ? std::make_shared<SurfaceMesh>(*Previous) : std::make_shared<SurfaceMesh>();
            const auto Status = Previous
                ? RefreshSurfaceMaterials(Samples, *Visual)
                : ExtractSurface({Samples, ChunkSide, ChunkSide, ChunkSide, Source.GetGrid().GetSpacing()},
                    MeshLimits, *Visual, Stop);
            if (Status != SurfaceStatus::Success) return std::unexpected(Convert(Status));
            if (!Visual->Indices.empty()) {
                if (NeedCollision) {
                    const auto Conditioning = BuildCollisionSurface(*Visual, {}, Chunk.Collision, Stop);
                    if (Conditioning != SurfaceStatus::Success) return std::unexpected(Convert(Conditioning));
                }
                Chunk.Visual = std::move(Visual);
            }
            return Chunk;
        }

        std::expected<Cohort, EditStatus> Prepare(PendingEdit &Work, Snapshot Base, const EditRequest &Request) {
            G_PROFILE("Terrain::PrepareCohort");
            const auto Stop = Work.Stop.get_token();
            if (Stop.stop_requested()) return std::unexpected(EditStatus::Canceled);
            auto Candidate = std::visit([&](const auto &Typed) {
                using T = std::decay_t<decltype(Typed)>;
                if constexpr (std::is_same_v<T, SmoothEdit>) {
                    return Field::PrepareSmooth(std::move(Base), Typed.Bounds, Typed.Strength);
                } else {
                    return Field::Prepare(std::move(Base), Typed);
                }
            }, Request);
            if (!Candidate) return std::unexpected(Convert(Candidate.error()));
            Cohort Result{std::move(*Candidate), {}};
            const auto Changes = Result.Candidate.GetChanges();
            if (Changes.empty()) return Result;

            const auto &Preview = Result.Candidate.GetPreview();
            const bool Geometry = std::ranges::any_of(Changes, &ChangedChunk::Geometry);
            std::vector<Sample> Samples(SurfaceSampleCount);
            std::size_t GeometryBytes = 0;
            Result.Chunks.reserve(Work.Targets.size());
            for (const auto &Target : Work.Targets) {
                if (!Geometry && !Target.Previous) continue;
                auto Chunk = PrepareChunk(Preview, Target.Activity.Key, Geometry ? nullptr : Target.Previous.get(),
                    Samples, Stop);
                if (!Chunk) return std::unexpected(Chunk.error());
                const auto Bytes = CountGeometryBytes(*Chunk);
                if (Bytes > MaximumCohortGeometryBytes - GeometryBytes)
                    return std::unexpected(EditStatus::LimitExceeded);
                GeometryBytes += Bytes;
                Result.Chunks.push_back(std::move(*Chunk));
            }
            return Result;
        }

        struct StartupSource {
            ChunkKey Key;
            Snapshot Samples;
        };

        std::expected<std::vector<ChunkPublicationCandidate>, EditStatus> PrepareStartupBatch(
            std::span<const StartupSource> Sources, bool NeedCollision, std::stop_token Stop = {}) {
            G_PROFILE("Terrain::PrepareStartup");
            std::vector<ChunkPublicationCandidate> Ready;
            Ready.reserve(Sources.size());
            std::vector<Sample> Samples(SurfaceSampleCount);
            std::size_t Bytes = 0;
            for (const auto &Source : Sources) {
                if (Stop.stop_requested()) return std::unexpected(EditStatus::Canceled);
                auto Chunk = PrepareChunk(Source.Samples, Source.Key, nullptr, Samples, Stop, NeedCollision);
                if (!Chunk) return std::unexpected(Chunk.error());
                if (!Chunk->Visual) continue;
                const auto ChunkBytes = CountGeometryBytes(*Chunk);
                if (ChunkBytes > MaximumCohortGeometryBytes - Bytes)
                    return std::unexpected(EditStatus::LimitExceeded);
                Bytes += ChunkBytes;
                Ready.push_back(std::move(*Chunk));
            }
            return Ready;
        }
    }

    struct EditCoordinator::Impl {
        Field &Storage;
        JobSystem &Jobs;
        detail::ActiveChunkRegistry Active;
        std::vector<std::shared_ptr<PendingEdit>> Pending;
        std::map<ChunkKey, std::shared_ptr<const SurfaceMesh>> Resident;
        std::optional<ReopenFootprint> Footprint;
        std::size_t ResidentBytes{};
        EditTicket Next{1};

        Impl(Field &Storage, JobSystem &Jobs)
            : Storage(Storage), Jobs(Jobs), Active(MaximumActiveChunks, Storage.GetLifetime()) {
            Pending.reserve(MaximumOutstanding);
        }

        std::expected<void, EditStatus> InitializeFootprint() {
            if (Footprint) return {};
            auto Ready = ReopenFootprint::Build(Storage);
            if (!Ready) return std::unexpected(Ready.error());
            Footprint.emplace(std::move(*Ready));
            return {};
        }

        void ReleaseTargets(std::span<const Target> Targets) noexcept {
            for (const auto &Target : Targets) {
                const auto Key = Target.Activity.Key;
                if (Resident.contains(Key)) continue;
                const bool PendingUse = std::ranges::any_of(Pending, [Key](const auto &Job) {
                    return std::ranges::any_of(Job->Targets, [Key](const auto &Item) { return Item.Activity.Key == Key; });
                });
                if (!PendingUse) static_cast<void>(Active.Retire(Key));
            }
        }

        EditReceipt Publish(PendingEdit &Job, CohortPublisher &Publisher) {
            G_PROFILE("Terrain::PublishCohort");
            const auto Fail = [&](EditStatus Status) { return EditReceipt{Job.Number, Status, 0}; };
            if (Job.Stop.stop_requested()) return Fail(EditStatus::Canceled);
            if (!Job.Output) return Fail(Job.Output.error());
            auto &Output = *Job.Output;
            const auto &Preview = Output.Candidate.GetPreview();
            if ((Job.ExpectedRevision && Storage.GetRevision() != *Job.ExpectedRevision) ||
                !Storage.IsCurrent(Preview) || !std::ranges::all_of(Job.Targets, [&](const auto &Item) {
                return Active.IsCurrent(Item.Activity);
            })) return Fail(EditStatus::Stale);

            auto FootprintPlan = Footprint->Prepare(Storage, Output.Candidate);
            if (!FootprintPlan) return Fail(FootprintPlan.error());

            // Reserve all persistent visual map nodes before any provider stage
            // or authoritative field commit.
            std::map<ChunkKey, std::shared_ptr<const SurfaceMesh>> Additions;
            auto NextBytes = ResidentBytes;
            for (const auto &Chunk : Output.Chunks)
                if (const auto At = Resident.find(Chunk.Key); At != Resident.end())
                    NextBytes -= CountVisualBytes(At->second);
            for (const auto &Chunk : Output.Chunks) {
                const auto Bytes = CountVisualBytes(Chunk.Visual);
                if (Bytes > MaximumResidentVisualBytes - NextBytes) return Fail(EditStatus::LimitExceeded);
                NextBytes += Bytes;
                if (Chunk.Visual && !Resident.contains(Chunk.Key)) Additions.emplace(Chunk.Key, Chunk.Visual);
            }

            std::unique_ptr<StagedPublication> Stage;
            if (!Output.Chunks.empty()) {
                auto Prepared = Publisher.Stage(Preview.GetGrid(), Output.Chunks);
                if (!Prepared || !*Prepared) return Fail(Prepared ? EditStatus::PublicationDenied : Prepared.error());
                Stage = std::move(*Prepared);
            }
            const auto Commit = Storage.Commit(std::move(Output.Candidate));
            if (!Commit) return Fail(Convert(Commit.error()));
            Footprint->Install(std::move(*FootprintPlan));
            if (Stage) {
                Stage->Install(Commit->CommittedRevision);
                if (!Stage->IsInstalled())
                    return {Job.Number, EditStatus::Unavailable, Commit->CommittedRevision};
            }

            for (const auto &Chunk : Output.Chunks) {
                const auto At = Resident.find(Chunk.Key);
                if (At != Resident.end()) {
                    if (Chunk.Visual) At->second = Chunk.Visual;
                    else Resident.erase(At);
                } else if (Chunk.Visual) {
                    Resident.insert(Additions.extract(Chunk.Key));
                }
            }
            ResidentBytes = NextBytes;
            for (const auto &Change : Commit->Changed) {
                if (const auto Ticket = Active.Capture(Change.Key)) {
                    auto Version = Ticket->Version;
                    if (Change.Geometry) Version.Geometry = Commit->CommittedRevision;
                    if (Change.Material) Version.Material = Commit->CommittedRevision;
                    static_cast<void>(Active.Update(Change.Key, Version));
                }
            }
            return {Job.Number, EditStatus::Committed, Commit->CommittedRevision};
        }
    };

    std::expected<void, EditStatus> ValidateTerrainReopenProfile(const Field &Storage) {
        const auto Footprint = ReopenFootprint::Build(Storage);
        if (!Footprint) return std::unexpected(Footprint.error());
        return {};
    }

    EditCoordinator::EditCoordinator(Field &Storage, JobSystem &Jobs) : State(std::make_unique<Impl>(Storage, Jobs)) {}

    template <class AdmitChunk>
    static std::expected<void, EditStatus> BuildInitialCohorts(const Field &Storage, JobSystem &Jobs,
        CohortPublisher &Publisher, const ReopenFootprint &Footprint, bool NeedCollision, AdmitChunk Admit) {
        G_PROFILE("Terrain::RestoreResidency");
        try {
            if (Footprint.Contour.empty()) return {};
            const auto RestoredRevision = Storage.GetRevision();
            if (RestoredRevision == 0) return std::unexpected(EditStatus::InvalidInput);

            auto Next = Footprint.Contour.begin();
            while (Next != Footprint.Contour.end()) {
                auto Sources = std::make_shared<std::vector<StartupSource>>();
                Sources->reserve(MaximumCohortChunks);
                for (; Next != Footprint.Contour.end() && Sources->size() < MaximumCohortChunks; ++Next) {
                    const auto Key = Next->first;
                    auto Dependencies = GetDependencies(std::span{&Key, 1});
                    if (!Dependencies) return std::unexpected(Convert(Dependencies.error()));
                    auto Samples = Storage.Capture(*Dependencies);
                    if (!Samples) return std::unexpected(Convert(Samples.error()));
                    Sources->push_back({Key, std::move(*Samples)});
                }
                using BatchResult = std::expected<std::vector<ChunkPublicationCandidate>, EditStatus>;
                auto Result = std::make_shared<BatchResult>(std::unexpected(EditStatus::PreparationFailed));
                auto Completed = std::make_shared<JobGroup>();
                Jobs.Submit([Result, Sources, NeedCollision] {
                    *Result = PrepareStartupBatch(*Sources, NeedCollision);
                }, Completed);
                Completed->Wait();
                if (const auto Failure = Completed->GetFirstException()) std::rethrow_exception(Failure);
                if (!*Result) return std::unexpected(Result->error());
                if (Storage.GetRevision() != RestoredRevision ||
                    !std::ranges::all_of(*Sources, [&](const auto &Source) {
                        return Storage.IsCurrent(Source.Samples);
                    }))
                    return std::unexpected(EditStatus::Stale);
                if (Result->value().empty()) continue;

                auto &Chunks = Result->value();
                for (const auto &Chunk : Chunks) {
                    const auto Source = std::ranges::lower_bound(*Sources, Chunk.Key, {}, &StartupSource::Key);
                    if (Source == Sources->end() || Source->Key != Chunk.Key)
                        return std::unexpected(EditStatus::PreparationFailed);
                    const auto Version = Source->Samples.GetVersion(Chunk.Key);
                    if (!Version) return std::unexpected(Convert(Version.error()));
                    if (auto Admitted = Admit(Chunk, *Version); !Admitted) return Admitted;
                }
                auto Staged = Publisher.Stage(Storage.GetGrid(), Chunks);
                if (!Staged) return std::unexpected(Staged.error());
                if (!*Staged) return std::unexpected(EditStatus::PublicationDenied);
                (*Staged)->Install(RestoredRevision);
                if (!(*Staged)->IsInstalled()) return std::unexpected(EditStatus::Unavailable);
            }
            return {};
        } catch (const std::bad_alloc &) {
            return std::unexpected(EditStatus::LimitExceeded);
        } catch (...) {
            return std::unexpected(EditStatus::PreparationFailed);
        }
    }

    std::expected<void, EditStatus> EditCoordinator::BuildInitialResidency(CohortPublisher &Publisher) {
        if (auto Ready = State->InitializeFootprint(); !Ready) return Ready;
        return BuildInitialCohorts(State->Storage, State->Jobs, Publisher, *State->Footprint, true,
            [&](const ChunkPublicationCandidate &Chunk, ChunkVersion Version) -> std::expected<void, EditStatus> {
                const auto Bytes = CountVisualBytes(Chunk.Visual);
                if (Bytes > MaximumResidentVisualBytes - State->ResidentBytes ||
                    !State->Active.Activate(Chunk.Key, Version))
                    return std::unexpected(EditStatus::LimitExceeded);
                State->Resident.emplace(Chunk.Key, Chunk.Visual);
                State->ResidentBytes += Bytes;
                return {};
            });
    }

    std::map<ChunkKey, std::shared_ptr<const SurfaceMesh>> EditCoordinator::TakeResidentSurfaces() {
        return std::move(State->Resident);
    }

    const std::map<ChunkKey, std::shared_ptr<const SurfaceMesh>> &EditCoordinator::GetResidentSurfaces() const {
        return State->Resident;
    }

    std::expected<void, EditStatus> PrepareTerrainPreviewCohorts(
        const Field &Storage, JobSystem &Jobs, CohortPublisher &Publisher) {
        const auto Footprint = ReopenFootprint::Build(Storage);
        if (!Footprint) return std::unexpected(Footprint.error());
        return BuildInitialCohorts(Storage, Jobs, Publisher, *Footprint, false,
            [](const ChunkPublicationCandidate &, ChunkVersion) { return std::expected<void, EditStatus>{}; });
    }

    std::expected<std::vector<ChunkPublicationCandidate>, EditStatus>
    PrepareTerrainReplica(const Field &Storage, std::stop_token Stop) {
        try {
            const auto Footprint = ReopenFootprint::Build(Storage);
            if (!Footprint) return std::unexpected(Footprint.error());
            if (Footprint->Contour.size() > MaximumReplicaContourChunks)
                return std::unexpected(EditStatus::LimitExceeded);
            std::vector<StartupSource> Sources;
            Sources.reserve(Footprint->Contour.size());
            for (const auto &[Key, References] : Footprint->Contour) {
                static_cast<void>(References);
                if (Stop.stop_requested()) return std::unexpected(EditStatus::Canceled);
                const auto Dependencies = GetDependencies(std::span{&Key, 1});
                if (!Dependencies) return std::unexpected(Convert(Dependencies.error()));
                auto Samples = Storage.Capture(*Dependencies);
                if (!Samples) return std::unexpected(Convert(Samples.error()));
                Sources.push_back({Key, std::move(*Samples)});
            }
            return PrepareStartupBatch(Sources, true, Stop);
        } catch (const std::bad_alloc &) {
            return std::unexpected(EditStatus::LimitExceeded);
        } catch (...) {
            return std::unexpected(EditStatus::PreparationFailed);
        }
    }

    EditCoordinator::~EditCoordinator() {
        for (const auto &Job : State->Pending) Job->Stop.request_stop();
    }

    std::expected<EditTicket, EditStatus> EditCoordinator::Submit(EditRequest Request,
        std::optional<Revision> ExpectedRevision) {
        G_PROFILE("Terrain::AdmitEdit");
        if (ExpectedRevision && State->Storage.GetRevision() != *ExpectedRevision)
            return std::unexpected(EditStatus::Stale);
        if (State->Pending.size() == MaximumOutstanding) return std::unexpected(EditStatus::LimitExceeded);
        if (State->Next == std::numeric_limits<EditTicket>::max()) return std::unexpected(EditStatus::Unavailable);
        if (State->Pending.empty()) static_cast<void>(State->Storage.RetireEmptyEntries());
        std::shared_ptr<PendingEdit> Work;
        try {
            if (const auto Ready = State->InitializeFootprint(); !Ready) return std::unexpected(Ready.error());
            const auto Bounds = GetWriteBounds(State->Storage.GetGrid(), Request);
            if (!Bounds) return std::unexpected(Convert(Bounds.error()));
            // Check raw bounds first. Expansion is only for contour dependency
            // discovery, with exterior samples consistently treated as air.
            const auto WriteKeys = GetRegionChunks(*Bounds);
            if (!WriteKeys) return std::unexpected(Convert(WriteKeys.error()));
            const auto Keys = GetRegionChunks(ExpandWithinField(*Bounds, 2));
            if (!Keys) return std::unexpected(Convert(Keys.error()));
            if (Keys->size() > MaximumCohortChunks) return std::unexpected(EditStatus::LimitExceeded);
            auto Dependencies = GetDependencies(*Keys);
            if (!Dependencies) return std::unexpected(Convert(Dependencies.error()));
            auto Base = State->Storage.Capture(*Dependencies);
            if (!Base) return std::unexpected(Convert(Base.error()));

            Work = std::make_shared<PendingEdit>(State->Next, ExpectedRevision);
            Work->Dependencies = std::move(*Dependencies);
            Work->Targets.reserve(Keys->size());
            for (const auto Key : *Keys) {
                const auto Activity = State->Active.Activate(Key, Base->GetVersion(Key).value());
                if (!Activity) {
                    State->ReleaseTargets(Work->Targets);
                    return std::unexpected(EditStatus::LimitExceeded);
                }
                const auto Previous = State->Resident.find(Key);
                Work->Targets.push_back({*Activity, Previous == State->Resident.end() ? nullptr : Previous->second});
            }

            State->Jobs.Submit([Work, Base = std::move(*Base), Request = std::move(Request)]() mutable {
                try {
                    Work->Output = Prepare(*Work, std::move(Base), Request);
                } catch (const std::bad_alloc &) {
                    Work->Output = std::unexpected(EditStatus::LimitExceeded);
                } catch (...) {
                    Work->Output = std::unexpected(EditStatus::PreparationFailed);
                }
                Work->Finished.store(true, std::memory_order_release);
            });
            State->Pending.push_back(Work);
            return State->Next++;
        } catch (const std::bad_alloc &) {
            if (Work) State->ReleaseTargets(Work->Targets);
            return std::unexpected(EditStatus::LimitExceeded);
        } catch (...) {
            if (Work) State->ReleaseTargets(Work->Targets);
            return std::unexpected(EditStatus::Unavailable);
        }
    }

    bool EditCoordinator::Cancel(EditTicket Ticket) noexcept {
        const auto Found = std::ranges::find_if(State->Pending, [Ticket](const auto &Job) { return Job->Number == Ticket; });
        return Found != State->Pending.end() && (*Found)->Stop.request_stop();
    }

    std::vector<EditReceipt> EditCoordinator::Poll(CohortPublisher &Publisher) {
        std::vector<EditReceipt> Ready;
        Ready.reserve(State->Pending.size());
        auto At = State->Pending.begin();
        while (At != State->Pending.end()) {
            const auto &Job = **At;
            const bool Blocked = std::any_of(State->Pending.begin(), At, [&](const auto &Earlier) {
                return Intersects(Earlier->Dependencies, Job.Dependencies);
            });
            if (!Job.Finished.load(std::memory_order_acquire) || Blocked) {
                ++At;
                continue;
            }
            try {
                Ready.push_back(State->Publish(**At, Publisher));
            } catch (const std::bad_alloc &) {
                Ready.push_back({Job.Number, EditStatus::LimitExceeded, 0});
            } catch (...) {
                Ready.push_back({Job.Number, EditStatus::PublicationDenied, 0});
            }
            auto Finished = std::move(*At);
            At = State->Pending.erase(At);
            State->ReleaseTargets(Finished->Targets);
        }
        if (State->Pending.empty()) static_cast<void>(State->Storage.RetireEmptyEntries());
        return Ready;
    }

    std::size_t EditCoordinator::GetPendingCount() const noexcept { return State->Pending.size(); }
    std::size_t EditCoordinator::GetActiveCount() const noexcept { return State->Active.GetCount(); }
}
