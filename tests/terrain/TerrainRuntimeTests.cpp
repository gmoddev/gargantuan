// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "terrain/TerrainRuntime.hpp"
#include "terrain/PhysicsStage.hpp"
#include "terrain/ScenePersistence.hpp"
#include "gargantuan/render/RenderProjection.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

using namespace gargantuan;
using namespace gargantuan::terrain;

namespace {
    void Require(bool Condition, const char *Message) {
        if (!Condition) throw std::runtime_error(Message);
    }

    EditReceipt Await(TerrainRuntime &Runtime, EditTicket Ticket, double *CommitPollUs = nullptr) {
        constexpr auto Timeout = std::chrono::seconds(3);
        const auto Deadline = std::chrono::steady_clock::now() + Timeout;
        while (std::chrono::steady_clock::now() < Deadline) {
            const auto Begin = std::chrono::steady_clock::now();
            const auto Ready = Runtime.Poll();
            const double PollUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - Begin).count();
            for (const auto &Receipt : Ready) {
                if (Receipt.Ticket != Ticket) continue;
                if (CommitPollUs) *CommitPollUs = PollUs;
                return Receipt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("Timed out waiting for the terrain edit");
    }

    void Publish(RenderProjection &Projection, const std::shared_ptr<const RenderState> &Before,
        const std::shared_ptr<const RenderState> &After) {
        RenderPublication Publication;
        Publication.BaseId = Projection.GetLastPublicationId();
        Publication.Id = Publication.BaseId + 1;
        Publication.FullResync = Publication.BaseId == 0;
        Publication.Frame.ViewportWidth = 640;
        Publication.Frame.ViewportHeight = 360;
        AppendTerrainRenderState(Publication, Before, After);
        static_cast<void>(Projection.Apply(Publication));
    }

    bool HasSnowTint(const RenderState &State) {
        return !State.Chunks.empty() && std::ranges::all_of(State.Chunks, [](const auto &Entry) {
            return std::ranges::all_of(*Entry.second.Resource.Vertices, [](const RenderVertex &Vertex) {
                return Vertex.Tint == std::array<std::uint8_t, 4>{199, 209, 217, 255};
            });
        });
    }

    void TestLiveCohort() {
        Field FieldAuthority;
        PhysicsWorld Provider(PhysicsWorldConfig{});
        Require(Provider.IsValid(), "expected a native physics world");
        auto Owner = TerrainRuntime::Start(FieldAuthority, ObjectId{8, 2}, Provider, true).value();
        auto &Runtime = *Owner;
        const auto &Collision = Runtime.GetPhysicsStage();
        RenderProjection Projection;
        const WorldPoint Center{12.33, 13.17, 14.21};
        const PhysicsRaycastRequest Down{{12.33f, 26.f, 14.21f}, {0.f, -22.f, 0.f}, {}};
        const SphereBrush Fill{Center, 4.13, BrushMode::Add, 1};
        const auto Ticket = Runtime.Submit(Fill).value();
        Require(!Provider.Raycast(Down).HasHit() && !Runtime.CaptureRender() && FieldAuthority.GetRevision() == 0,
            "preparation is hidden");
        const auto Initial = Await(Runtime, Ticket);
        Require(Initial.Status == EditStatus::Committed && Initial.CommittedRevision == 1 && Collision.IsHealthy(),
            "native fill ready");
        const auto Live = Runtime.CaptureRender();
        Publish(Projection, {}, Live);
        Require(Projection.GetTerrainDraws().size() == 1 && Projection.GetObjects().empty(), "separate render owner");
        const auto Hit = Provider.Raycast(Down);
        Require(Hit.HasHit() && Hit.Candidates.front().Position.y > 17.f, "real provider ray hits generated terrain");
        const auto Body = Hit.Candidates.front().Body;
        Require(Collision.FindOwner(Body)->SurfaceRevision == Initial.CommittedRevision, "collision tracks commit revision");
        Require(FieldAuthority.Read({12, 13, 14})->Material == 1, "authoritative sample visible");

        const auto NativeBeforePaint = Provider.GetCollisionResidency();
        const auto Paint = Runtime.Submit(SphereBrush{{Center.X, Center.Y + 1.5, Center.Z}, 3.6,
            BrushMode::Paint, 5}).value();
        Require(Await(Runtime, Paint).Status == EditStatus::Committed, "paint admitted");
        const auto Painted = Runtime.CaptureRender();
        Publish(Projection, Live, Painted);
        const auto &BeforePaintMesh = Live->Chunks.begin()->second.Resource;
        const auto &PaintedMesh = Painted->Chunks.begin()->second.Resource;
        const auto &PaintedVertices = *PaintedMesh.Vertices;
        Require(std::ranges::any_of(PaintedVertices, [](const RenderVertex &Vertex) { return Vertex.Tint[2] == 217; }) &&
            std::ranges::any_of(PaintedVertices, [](const RenderVertex &Vertex) { return Vertex.Tint[2] == 41; }) &&
            std::ranges::equal(*BeforePaintMesh.Vertices, PaintedVertices, [](const auto &Before, const auto &After) {
                return Before.Position == After.Position && Before.Normal == After.Normal;
            }) && *BeforePaintMesh.Indices == *PaintedMesh.Indices, "partial paint publishes mixed vertex colors over the same mesh geometry");
        Require(Provider.Raycast(Down).Candidates.front().Body == Body &&
            Provider.GetCollisionResidency().CreatedResources == NativeBeforePaint.CreatedResources,
            "paint retains provider geometry without recooking");
        Require(FieldAuthority.Read({12, 13, 14})->Material == 5, "paint changes field material");

        const auto Clear = Runtime.Submit(SphereBrush{Center, 5.0, BrushMode::Subtract, 0}).value();
        Require(Await(Runtime, Clear).Status == EditStatus::Committed, "subtraction admitted");
        Publish(Projection, Painted, Runtime.CaptureRender());
        Require(Projection.GetTerrainDraws().empty() && !Provider.Raycast(Down).HasHit() &&
            !Collision.FindOwner(Body) && FieldAuthority.Read({12, 13, 14})->Density > 0, "subtraction retires both surfaces");
        Require(Provider.GetCollisionResidency().ActiveResources == 0, "no native resources remain");
    }

    void CommitSphere(Field &Storage, const SphereBrush &Brush) {
        const auto Bounds = GetBrushBounds(Storage.GetGrid(), Brush).value();
        const auto Keys = GetRegionChunks(Bounds).value();
        auto Snapshot = Storage.Capture(Keys).value();
        auto Candidate = Field::Prepare(std::move(Snapshot), Brush).value();
        Require(Storage.Commit(std::move(Candidate)).has_value(), "native fixture construction");
    }

    void TestReplicaCutover() {
        Field Initial;
        const WorldPoint Center{12.33, 13.17, 14.21};
        CommitSphere(Initial, {Center, 4.13, BrushMode::Add, 1});
        const auto Grid = Initial.GetGrid();
        PhysicsWorldConfig Configuration;
        Configuration.CollisionResourceLimit = 1;
        PhysicsWorld Provider(Configuration);
        auto Replica = TerrainRuntime::Start(Initial, ObjectId{9, 4}, Provider, false, true).value();
        const PhysicsRaycastRequest Down{{12.33f, 26.f, 14.21f}, {0.f, -22.f, 0.f}, {}};
        const auto InitialBody = Provider.Raycast(Down).Candidates.front().Body;
        const auto Fine = Replica->CaptureRender();
        const auto NativeBeforePaint = Provider.GetCollisionResidency();

        Field Painted;
        CommitSphere(Painted, {Center, 4.13, BrushMode::Add, 1});
        CommitSphere(Painted, {Center, 4.15, BrushMode::Paint, 5});
        Require(Replica->InstallReplica(Grid, PrepareTerrainReplica(Painted, {}).value(), 2).has_value(),
            "a replica commits paint under a one-resource collision budget");
        const auto PaintRender = Replica->CaptureRender();
        Require(PaintRender != Fine && HasSnowTint(*PaintRender) &&
            Provider.Raycast(Down).Candidates.front().Body == InitialBody &&
            Provider.GetCollisionResidency().CreatedResources == NativeBeforePaint.CreatedResources,
            "paint replaces the visible material and retains the native body");
        Require(Replica->InstallReplica(Grid, {}, 2).error() == EditStatus::Stale,
            "an old regional publication cannot retire current collision");

        Field Raised;
        CommitSphere(Raised, {{Center.X, Center.Y + 2, Center.Z}, 4.13, BrushMode::Add, 3});
        const auto Denied = Replica->InstallReplica(Grid, PrepareTerrainReplica(Raised, {}).value(), 3);
        Require(!Denied && Replica->CaptureRender() == PaintRender &&
            Provider.Raycast(Down).Candidates.front().Body == InitialBody,
            "provider denial preserves the prior regional pair");
        Require(Replica->InstallReplica(Grid, {}, 4).has_value() &&
            Replica->CaptureRender()->Chunks.empty() && !Provider.Raycast(Down).HasHit() &&
            Provider.GetCollisionResidency().ActiveResources == 0,
            "a new empty region retires the old provider and visual resources together");
    }

    void TestRestoreAtStartup() {
        Field Restored;
        const WorldPoint Center{-0.22, 13.17, 14.21};
        CommitSphere(Restored, {Center, 4.13, BrushMode::Add, 2});
        CommitSphere(Restored, {Center, 4.15, BrushMode::Paint, 5});
        const auto RevisionBeforeStart = Restored.GetRevision();
        const auto SamplesBeforeStart = Restored.GetStats();
        const PhysicsRaycastRequest Left{{-.8f, 26.f, 14.21f}, {0.f, -22.f, 0.f}, {}};
        const PhysicsRaycastRequest Right{{.8f, 26.f, 14.21f}, {0.f, -22.f, 0.f}, {}};

        // The two nonempty output meshes are in different sorted startup batches.
        // A later failure must retire the native resource admitted by the first.
        PhysicsWorldConfig Limited;
        Limited.CollisionResourceLimit = 1;
        PhysicsWorld DenyingProvider(Limited);
        const auto Denied = TerrainRuntime::Start(Restored, ObjectId{8, 3}, DenyingProvider, true);
        const auto DeniedCounts = DenyingProvider.GetCollisionResidency();
        Require(!Denied && Denied.error() == EditStatus::PublicationDenied &&
            DeniedCounts.CreatedResources == 1 && DeniedCounts.RetiredResources == 1 &&
            DeniedCounts.FailedAdmissions == 1 && DeniedCounts.ActiveResources == 0 &&
            !DenyingProvider.Raycast(Left).HasHit() && !DenyingProvider.Raycast(Right).HasHit(),
            "late startup admission denial destroys previously installed private bodies");
        Require(Restored.GetRevision() == RevisionBeforeStart &&
            Restored.GetStats().OwnedBytes == SamplesBeforeStart.OwnedBytes &&
            Restored.Read({0,13,14})->Material == 5, "failed startup preserves the supplied authoritative field");

        PhysicsWorld Provider(PhysicsWorldConfig{});
        auto Owner = TerrainRuntime::Start(Restored, ObjectId{8, 3}, Provider, true).value();
        const auto &Physics = Owner->GetPhysicsStage();
        const auto LeftHit = Provider.Raycast(Left);
        const auto RightHit = Provider.Raycast(Right);
        Require(LeftHit.HasHit() && RightHit.HasHit() && Physics.GetBodyCount() == 2, "restored terrain covers seam");
        const auto LeftId = LeftHit.Candidates.front().Body;
        const auto RightId = RightHit.Candidates.front().Body;
        const auto LeftOwner = Physics.FindOwner(LeftId);
        const auto RightOwner = Physics.FindOwner(RightId);
        Require(LeftOwner && RightOwner && LeftOwner->Key.X == -1 && RightOwner->Key.X == 0 &&
            LeftOwner->SurfaceRevision == RevisionBeforeStart && RightOwner->SurfaceRevision == RevisionBeforeStart,
            "startup publishes original revision and canonical seam owners");

        RenderProjection Projection;
        const auto Before = Owner->CaptureRender();
        Publish(Projection, {}, Before);
        Require(Projection.GetTerrainDraws().size() == 2 &&
            HasSnowTint(*Before) &&
            Restored.GetRevision() == RevisionBeforeStart && Owner->Poll().empty(),
            "restore binds saved material without fabricating edit receipts");

        const auto Ticket = Owner->Submit(SphereBrush{{Center.X, Center.Y + 1.5, Center.Z}, 3.6,
            BrushMode::Paint, 4}, RevisionBeforeStart).value();
        Require(Ticket == 1 && Await(*Owner, Ticket).CommittedRevision == RevisionBeforeStart + 1 &&
            Provider.Raycast(Left).Candidates.front().Body == LeftId &&
            Provider.Raycast(Right).Candidates.front().Body == RightId,
            "first edit derives from restored mesh and preserves collider identity on paint");
        const auto Painted = Owner->CaptureRender();
        const auto CollectSeam = [](const auto &Chunk, float X) {
            std::map<std::pair<float, float>, std::array<std::uint8_t, 4>> Tints;
            for (const auto &Vertex : *Chunk.Resource.Vertices)
                if (Vertex.Position.x == X)
                    Tints.emplace(std::pair{Vertex.Position.y, Vertex.Position.z}, Vertex.Tint);
            return Tints;
        };
        const auto LeftSeam = CollectSeam(Painted->Chunks.at({-1, 0, 0}), ChunkSide);
        const auto RightSeam = CollectSeam(Painted->Chunks.at({0, 0, 0}), 0.f);
        Require(LeftSeam == RightSeam &&
            std::ranges::any_of(LeftSeam, [](const auto &Tint) { return Tint.second[2] == 79; }) &&
            std::ranges::any_of(LeftSeam, [](const auto &Tint) { return Tint.second[2] == 217; }),
            "independently derived chunks agree on both colors across a partially painted seam");
        Owner.reset();
        Require(Provider.GetCollisionResidency().ActiveResources == 0, "restored scope teardown");

        auto ReadOnly = TerrainRuntime::Start(Restored, ObjectId{8, 4}, Provider, false).value();
        const auto ServerEdit = ReadOnly->Submit(SphereBrush{Center, 4.15, BrushMode::Paint, 1});
        Require(Provider.Raycast(Right).HasHit() && !ServerEdit && ServerEdit.error() == EditStatus::Unavailable,
            "read-only server startup enables verified collision without enabling edits");
    }

    void TestEmptyStartup() {
        Field Restored(GridSpec{1});
        PhysicsWorld Provider(PhysicsWorldConfig{});
        auto Owner = TerrainRuntime::Start(Restored, ObjectId{8, 5}, Provider, true).value();
        Require(Restored.GetGrid().GetSpacing() == 2.0 && Restored.GetRevision() == 0 &&
            Owner->GetPhysicsStage().GetBodyCount() == 0 && !Owner->CaptureRender(),
            "empty restored field keeps spacing without allocating native resources");
    }

    void TestDetachedEditPreview() {
        Field Restored;
        CommitSphere(Restored, {{-.22, 13.17, 14.21}, 4.13, BrushMode::Add, 5});
        const auto RevisionBeforePreview = Restored.GetRevision();
        auto Preview = TerrainRenderPreview::Prepare(Restored).value();
        constexpr ObjectId TerrainOwner{9, 1};
        const auto Terrain = std::move(Preview).Bind(TerrainOwner);
        RenderProjection Projection;
        Publish(Projection, {}, Terrain);
        Require(Projection.GetTerrainDraws().size() == 2 && Projection.GetObjects().empty() &&
            std::ranges::all_of(Projection.GetTerrainDraws(), [&](const auto &Draw) {
                return Draw.Item.Object == TerrainOwner;
            }) && HasSnowTint(*Terrain) && Restored.GetRevision() == RevisionBeforePreview,
            "detached preview binds saved material and canonical owner without a field edit");
        Field Empty(GridSpec{1});
        auto EmptyPreview = TerrainRenderPreview::Prepare(Empty).value();
        const auto EmptyState = std::move(EmptyPreview).Bind(ObjectId{9, 2});
        Publish(Projection, Terrain, EmptyState);
        Require(!EmptyState && Projection.GetTerrainDraws().empty() && Projection.GetMeshCount() == 0,
            "empty edit preview clears earlier terrain draw and resource leases");
    }

    void TestStartupFindsUnstoredSeamOwner() {
        const std::array Keys{ChunkKey{0,0,0}};
        const std::array Writes{SampleWrite{{0,13,14}, {-32767,3}}};
        for (const auto Grid : {GridSpec{-1}, GridSpec{0}, GridSpec{1}}) {
            Field Restored(Grid);
            auto Base = Restored.Capture(Keys).value();
            auto Candidate = Field::Prepare(std::move(Base), Writes).value();
            Require(Restored.Commit(std::move(Candidate)).has_value() &&
                Restored.GetStats().MaterializedPages == 1, "fixture has only one materialized owner");
            PhysicsWorld Provider(PhysicsWorldConfig{});
            auto Owner = TerrainRuntime::Start(Restored, ObjectId{8, 6}, Provider, true).value();
            const auto Spacing = static_cast<float>(Grid.GetSpacing());
            const auto Hit = Provider.Raycast({{-.125f * Spacing,16 * Spacing,14 * Spacing},
                {0,-6 * Spacing,0}, {}});
            Require(Hit.HasHit() && std::abs(Hit.Candidates.front().Position.y / Spacing - 13.375f) < .02f,
                "startup uses saved spacing across the unstored seam neighbor");
            const auto Seam = Owner->GetPhysicsStage().FindOwner(Hit.Candidates.front().Body);
            Require(Seam && Seam->Key == ChunkKey{-1,0,0} && Restored.GetStats().MaterializedPages == 1,
                "derived chunk stays separate from authoritative sample ownership");
        }
    }

    void TestStartupWorkLimit() {
        Field Restored;
        // Disconnected owners maximize the extra seam-neighbor contour keys.
        for (int Batch = 0; Batch < 3; ++Batch) {
            std::vector<ChunkKey> Keys;
            std::vector<SampleWrite> Writes;
            for (int Item = Batch * 64; Item < std::min((Batch + 1) * 64, 129); ++Item) {
                Keys.push_back({4 * Item,0,0});
                Writes.push_back({{4 * Item * ChunkSide,13,14}, {-32767,3}});
            }
            auto Base = Restored.Capture(Keys).value();
            auto Candidate = Field::Prepare(std::move(Base), Writes).value();
            Require(Restored.Commit(std::move(Candidate)).has_value(), "sparse work-limit fixture");
        }
        PhysicsWorld Provider(PhysicsWorldConfig{});
        const auto Denied = TerrainRuntime::Start(Restored, ObjectId{8, 7}, Provider, true);
        const auto Preview = TerrainRenderPreview::Prepare(Restored);
        Require(!Denied && Denied.error() == EditStatus::ReopenCapacityExceeded &&
            !Preview && Preview.error() == EditStatus::ReopenCapacityExceeded &&
            Provider.GetCollisionResidency().CreatedResources == 0 && Restored.GetStats().MaterializedPages == 129,
            "startup preflight bounds disconnected contour candidate work");
    }

    void TestEditsKeepReopenCapacity() {
        constexpr Sample PositiveDensity{1, 0};
        constexpr Sample SolidDensity{-16384, 3};
        for (const bool SourceLimit : {false, true}) {
            Field Restored;
            std::vector<ChunkKey> Keys;
            const int Count = SourceLimit ? 512 : 128;
            const auto Stored = SourceLimit ? PositiveDensity : SolidDensity;
            for (int Index = 0; Index < Count; ++Index) Keys.push_back({4 * Index, 0, 0});
            const auto PointIn = [](ChunkKey Key) {
                return SamplePoint{Key.X * ChunkSide + 14, Key.Y * ChunkSide + 13, Key.Z * ChunkSide + 12};
            };
            const ChunkKey ExtraKey{4 * Count, 0, 0};
            const auto Extra = PointIn(ExtraKey);
            auto Remaining = std::span<const ChunkKey>(Keys);
            while (!Remaining.empty()) {
                const auto Batch = Remaining.first(std::min(Remaining.size(), MaximumCapturedChunks));
                std::vector<SampleWrite> Writes;
                for (const auto Key : Batch) Writes.push_back({PointIn(Key), Stored});
                auto Base = Restored.Capture(Batch).value();
                auto Candidate = Field::Prepare(std::move(Base), Writes).value();
                Require(Restored.Commit(std::move(Candidate)).has_value(), "independent source and contour cap fixture");
                Remaining = Remaining.subspan(Batch.size());
            }
            if (!SourceLimit) {
                const std::array ExtraKeys{ExtraKey};
                const std::array ExtraWrites{SampleWrite{Extra, PositiveDensity}};
                auto Candidate = Field::Prepare(Restored.Capture(ExtraKeys).value(), ExtraWrites).value();
                Require(Restored.Commit(std::move(Candidate)).has_value(), "store positive data beyond the solid contour cap");
            }
            PhysicsWorld Provider(PhysicsWorldConfig{});
            auto Runtime = TerrainRuntime::Start(Restored, ObjectId{9, 3}, Provider, true).value();
            const auto TryWrite = [&](SamplePoint Point, Sample Value) {
                return Await(*Runtime, Runtime->Submit(std::vector{SampleWrite{Point, Value}}).value());
            };
            const auto RevisionBeforeDenial = Restored.GetRevision();
            Require(TryWrite(Extra, Stored).Status == EditStatus::ReopenCapacityExceeded &&
                Restored.Read(Extra) == (SourceLimit ? AirSample : PositiveDensity) &&
                Restored.GetRevision() == RevisionBeforeDenial,
                "new signed data must fit the source budget and solid promotion must fit the contour budget");
            Require(TryWrite(PointIn(Keys.front()), AirSample).Status == EditStatus::Committed &&
                TryWrite(Extra, Stored).Status == EditStatus::Committed &&
                Restored.GetStats().MaterializedPages == Keys.size() && Restored.Read(Extra) == Stored,
                "clearing a page frees its original capacity for exact replacement samples");
            Runtime.reset();
            Runtime = TerrainRuntime::Start(Restored, ObjectId{9, 4}, Provider, true).value();
            const auto Resident = Provider.GetCollisionResidency();
            Require(SourceLimit ? (!Runtime->CaptureRender() && Resident.CreatedResources == 0)
                                : (Runtime->CaptureRender() && Resident.ActiveMeshShapes == Keys.size()),
                "only the solid profile constructs render and native collision resources");
        }
    }

    void Measure() {
        std::cout << std::fixed << std::setprecision(3)
            << "fixture,operation,trial,admit_us,ready_wall_us,completion_poll_us,active_field_bytes,"
               "render_state_bytes,native_mesh_bytes,provider_peak_build_blob_bytes,shapes\n";
        for (const bool Seam : {false, true}) {
            Field FieldAuthority;
            PhysicsWorld Provider(PhysicsWorldConfig{});
            auto Owner = TerrainRuntime::Start(FieldAuthority, ObjectId{8, 2}, Provider, true).value();
            auto &Runtime = *Owner;
            const WorldPoint Center{Seam ? 31.78 : 12.33, 13.17, 14.21};
            const auto Record = [&](SphereBrush Brush, const char *Operation, int Trial) {
                using Clock = std::chrono::steady_clock;
                const auto Start = Clock::now();
                const auto Ticket = Runtime.Submit(Brush).value();
                const double Admit = std::chrono::duration<double, std::micro>(Clock::now() - Start).count();
                double Poll = 0;
                Require(Await(Runtime, Ticket, &Poll).Status == EditStatus::Committed, "measured edit failed");
                const double Wall = std::chrono::duration<double, std::micro>(Clock::now() - Start).count();
                const auto Visual = Runtime.CaptureRender();
                const auto Physical = Provider.GetCollisionResidency();
                if (Trial >= 0) {
                    std::cout << (Seam ? "seam" : "interior") << ',' << Operation << ',' << Trial << ',' << Admit << ','
                        << Wall << ',' << Poll << ',' << FieldAuthority.GetStats().OwnedBytes << ','
                        << (Visual ? Visual->Bytes : 0) << ',' << Physical.NativeMeshBytes << ','
                        << Physical.PeakBuildNativeBytes << ',' << Physical.ActiveMeshShapes << '\n';
                }
            };
            for (int Trial = -2; Trial < 25; ++Trial) {
                Record({Center, 4.13, BrushMode::Add, 1}, "fill", Trial);
                Record({Center, 4.15, BrushMode::Paint, 5}, "paint", Trial);
                Record({Center, 5.00, BrushMode::Subtract, 0}, "remove", Trial);
            }
        }
    }

    void SeedScaleField(Field &Storage, int Pages, bool MostlyInactive) {
        for (int Index = 0; Index < Pages; ++Index) {
            const bool Inactive = MostlyInactive && Index >= 4;
            const ChunkKey Key = Inactive ? ChunkKey{Index * 4, 0, 8} : ChunkKey{Index, 0, 0};
            std::vector<SampleWrite> Writes;
            if (Inactive) {
                Writes.push_back({{Key.X * ChunkSide + 16, 16, Key.Z * ChunkSide + 16}, {1, 0}});
            } else {
                Writes.reserve(MaximumEditSamples);
                for (int Z = 0; Z < ChunkSide; ++Z)
                    for (int Y = 0; Y < ChunkSide; ++Y)
                        for (int X = 0; X < ChunkSide; ++X) {
                            const auto Wave = (Index * ChunkSide + X + Z / 2) % 24;
                            const auto Height = 5 * 1024 + 311 + std::min(Wave, 24 - Wave) * 384;
                            const auto Density = std::clamp(Y * 1024 - Height, -32767, 32767);
                            if (Density == SaturatedDensity) continue;
                            const MaterialId Material = Density < 0 ? (Z < ChunkSide / 2 ? 2 : 5) : 0;
                            Writes.push_back({{Key.X * ChunkSide + X, Y, Z},
                                {static_cast<std::int16_t>(Density), Material}});
                        }
            }
            const std::array Keys{Key};
            auto Base = Storage.Capture(Keys).value();
            auto Candidate = Field::Prepare(std::move(Base), Writes).value();
            Require(Storage.Commit(std::move(Candidate)).has_value(), "scaled field seed failed");
        }
    }

    void MeasureScale() {
        using Clock = std::chrono::steady_clock;
        const auto Elapsed = [](Clock::time_point Started) {
            return std::chrono::duration<double, std::milli>(Clock::now() - Started).count();
        };
        std::cout << std::fixed << std::setprecision(3)
            << "layout,pages,storage,seed_ms,encode_ms,decode_ms,admit_ms,field_bytes,closure_bytes,"
               "draw_chunks,triangles,render_bytes,native_mesh_bytes,peak_build_native_bytes,edit_p50_ms,edit_p95_ms,"
               "incremental_encode_ms,reused_leaves,new_leaf_bytes\n";
        for (const bool MostlyInactive : {false, true}) {
            const std::array Counts = MostlyInactive ? std::array{4, 24, 96} : std::array{4, 24, 64};
            for (const auto Count : Counts) {
                const auto Project = ProjectId::New();
                const auto Dataset = GetTerrainDatasetId(Project);
                std::optional<AssetContentId> Canonical;
                for (const auto Mode : {BrickStorageMode::DensePalette, BrickStorageMode::LosslessColumns}) {
                    const auto BeganSeed = Clock::now();
                    const FieldLimits Limits{.BrickStorage = Mode};
                    Field Storage({}, Limits);
                    SeedScaleField(Storage, Count, MostlyInactive);
                    const auto Seed = Elapsed(BeganSeed);
                    const auto BeganEncode = Clock::now();
                    const auto Captured = CaptureTerrain(Storage, Project, Dataset).value();
                    const auto Encode = Elapsed(BeganEncode);
                    if (Canonical) Require(Captured.ManifestDigest == Canonical, "scaled storage changed canonical terrain");
                    Canonical = Captured.ManifestDigest;
                    const auto BeganDecode = Clock::now();
                    const auto Restored = LoadTerrain(Project, Dataset, *Canonical,
                        [&](const TerrainArtifactId &Id, std::size_t Bound) {
                            return ReadCapturedTerrainArtifact(Captured.Artifacts, Id, Bound);
                        }, TerrainPersistenceLimits{.RestoredField = Limits}).value();
                    const auto Decode = Elapsed(BeganDecode);
                    Require(Restored->GetStats().MaterializedPages == static_cast<std::size_t>(Count),
                        "scaled persistence lost stored field pages");
                    const auto Physical = Restored->GetStats().OwnedBytes;
                    PhysicsWorld Provider(PhysicsWorldConfig{});
                    const auto BeganAdmission = Clock::now();
                    auto Runtime = TerrainRuntime::Start(*Restored, ObjectId{8, 2}, Provider, true).value();
                    const auto Admit = Elapsed(BeganAdmission);
                    const auto Visual = Runtime->CaptureRender();
                    const auto Native = Provider.GetCollisionResidency();
                    Require(Visual && Native.ActiveMeshShapes != 0 &&
                        Provider.Raycast({{12.0f, 20.0f, 12.0f}, {0.0f, -20.0f, 0.0f}, {}}).HasHit(),
                        "scaled terrain needs native collision and drawable terrain");
                    std::size_t Triangles = 0;
                    for (const auto &[Key, Chunk] : Visual->Chunks) {
                        static_cast<void>(Key);
                        Triangles += Chunk.Resource.Indices->size() / 3;
                    }

                    const auto Durable = CaptureTerrain(*Restored, Project, Dataset).value();
                    std::array<double, 20> EditTimes;
                    std::size_t Trial = 0;
                    for (auto &Duration : EditTimes) {
                        const auto Started = Clock::now();
                        auto Receipt = Await(*Runtime, Runtime->Submit(SphereBrush{
                            {31.5, 5.1, 12}, 1.7, BrushMode::Paint, ++Trial % 2 == 1 ? MaterialId{7} : MaterialId{2}
                        }).value());
                        Require(Receipt.Status == EditStatus::Committed, "scaled seam paint failed");
                        Duration = Elapsed(Started);
                    }
                    std::ranges::sort(EditTimes);
                    Require(Native.CreatedResources == Provider.GetCollisionResidency().CreatedResources,
                        "paint churn creates no native geometry");
                    const auto BeganCapture = Clock::now();
                    const auto Incremental = CaptureTerrain(*Restored, Project, Dataset, &Durable.Index).value();
                    const auto Capture = Elapsed(BeganCapture);
                    std::size_t LeafBytes = 0;
                    for (const auto &Artifact : Incremental.Artifacts)
                        if (Artifact.Id.Purpose == TerrainArtifactPurpose::Chunk) LeafBytes += Artifact.Bytes->size();
                    Require(Incremental.ReusedChunks >= static_cast<std::size_t>(Count - 2),
                        "a seam paint re-encodes at most its two source pages");
                    std::cout << (MostlyInactive ? "sparse" : "ridge") << ',' << Count << ','
                        << (Mode == BrickStorageMode::DensePalette ? "palette" : "columns") << ','
                        << Seed << ',' << Encode << ',' << Decode << ',' << Admit << ',' << Physical << ','
                        << Captured.ClosureBytes << ',' << Visual->Chunks.size() << ',' << Triangles << ','
                        << Visual->Bytes << ',' << Native.NativeMeshBytes << ',' << Native.PeakBuildNativeBytes << ','
                        << (EditTimes[9] + EditTimes[10]) / 2 << ',' << EditTimes[18] << ',' << Capture << ','
                        << Incremental.ReusedChunks << ',' << LeafBytes << '\n';
                }
            }
        }
    }
}

int main(int ArgumentCount, char **Arguments) try {
    if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--measure") {
        Measure();
        return 0;
    }
    if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--measure-scale") {
        MeasureScale();
        return 0;
    }
    TestLiveCohort();
    TestReplicaCutover();
    TestRestoreAtStartup();
    TestEmptyStartup();
    TestDetachedEditPreview();
    TestStartupFindsUnstoredSeamOwner();
    TestStartupWorkLimit();
    TestEditsKeepReopenCapacity();
    std::cout << "[Terrain:RuntimeTests] Passed\n";
    return 0;
} catch (const std::exception &Error) {
    std::cerr << "[Terrain:RuntimeTests] " << Error.what() << '\n';
    return 1;
}
