#include "RigidJoltBackend.hpp"
#include "ContactProbe.hpp"
#include "Provider/UnifiedJoltProvider.hpp"
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
#include "ExperimentSupport.hpp"
#include <chrono>
#endif

#if defined(PHYSICS_GANTRIA_CONVERGENCE) && !defined(PHYSICS_GANTRIA_JOLT)
#error "The repeated-sweep provider profile requires the corrected Gantria Jolt solver"
#endif
#if defined(PHYSICS_GANTRIA_SLEEP_FIX) && !defined(PHYSICS_GANTRIA_JOLT)
#error "The full-update sleep clock requires the maintained Gantria Jolt source"
#endif

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyContactListener.h>
#include <Jolt/Physics/SoftBody/SoftBodyManifold.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#ifdef PHYSICS_GANTRIA_ATTACHMENTS
#include <Jolt/Physics/SoftBody/SoftBodyRigidAttachment.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace PhysicsEvaluation {
namespace {
    using namespace gargantuan;

#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
    using AdmissionClock = std::chrono::steady_clock;
    thread_local JoltAdmissionProfile AdmissionProfile;
    thread_local JoltWorldAdmissionProfile WorldAdmissionProfile;
    void CompleteAdmissionPhase(JoltAdmissionPhase &Target, AdmissionClock::time_point Start,
                                const ResourceSnapshot &Before) {
        const auto After = CaptureResources();
        Target.Milliseconds += std::chrono::duration<double, std::milli>(
            AdmissionClock::now() - Start).count();
        Target.AllocationCalls += After.AllocationCalls - Before.AllocationCalls;
        Target.AllocatedBytes += After.AllocatedBytes - Before.AllocatedBytes;
    }
    class AdmissionMeasure {
    public:
        explicit AdmissionMeasure(JoltAdmissionPhase &Phase)
            : Target(Phase), Start(AdmissionClock::now()), Before(CaptureResources()) {}
        ~AdmissionMeasure() { CompleteAdmissionPhase(Target, Start, Before); }
    private:
        JoltAdmissionPhase &Target;
        AdmissionClock::time_point Start;
        ResourceSnapshot Before;
    };
    struct AdmissionStart {
        AdmissionClock::time_point Start = AdmissionClock::now();
        ResourceSnapshot Before = CaptureResources();
    };
    struct AdmissionStop {
        AdmissionStop(const AdmissionStart &Start, JoltAdmissionPhase &Target) {
            CompleteAdmissionPhase(Target, Start.Start, Start.Before);
        }
    };
#endif

    JPH::Vec3 ToJolt(glm::vec3 Value) { return {Value.x, Value.y, Value.z}; }
    JPH::RVec3 ToRVec(glm::vec3 Value) { return {Value.x, Value.y, Value.z}; }
    glm::vec3 FromJolt(JPH::Vec3Arg Value) { return {Value.GetX(), Value.GetY(), Value.GetZ()}; }
    glm::vec3 FromRVec(JPH::RVec3Arg Value) { return {float(Value.GetX()), float(Value.GetY()), float(Value.GetZ())}; }
    JPH::Quat ToJolt(const CFrame &Value) {
        const auto Rotation = Value.ToQuaternion();
        return {Rotation.x, Rotation.y, Rotation.z, Rotation.w};
    }
    CFrame FromJolt(JPH::RVec3Arg Position, JPH::QuatArg Rotation) {
        return CFrame::fromQuaternion(Rotation.GetX(), Rotation.GetY(), Rotation.GetZ(),
            Rotation.GetW(), FromRVec(Position));
    }
    bool Finite(glm::vec3 Value) {
        return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
    }
    bool Finite(const CFrame &Value) {
        if (!Finite(Value.Position)) return false;
        for (int Column = 0; Column != 3; ++Column)
            for (int Row = 0; Row != 3; ++Row)
                if (!std::isfinite(Value.Rotation[Column][Row])) return false;
        return true;
    }
    bool RigidFrame(const CFrame &Value) {
        if (!Finite(Value) || glm::length(Value.Position) > 100'000.0f) return false;
        constexpr float Tolerance = 1.0e-5f;
        for (int Axis = 0; Axis < 3; ++Axis) {
            if (std::abs(glm::dot(Value.Rotation[Axis], Value.Rotation[Axis]) - 1.0f) > Tolerance)
                return false;
            for (int Other = Axis + 1; Other < 3; ++Other)
                if (std::abs(glm::dot(Value.Rotation[Axis], Value.Rotation[Other])) > Tolerance)
                    return false;
        }
        return std::abs(glm::dot(glm::cross(Value.Rotation[0], Value.Rotation[1]),
            Value.Rotation[2]) - 1.0f) <= Tolerance;
    }
    bool ValidConstraint(const PhysicsConstraintDesc &Description) {
        if (!RigidFrame(Description.LocalFrameA) || !RigidFrame(Description.LocalFrameB) ||
            !std::isfinite(Description.LowerLimit) || !std::isfinite(Description.UpperLimit) ||
            !std::isfinite(Description.MotorSpeed) || !std::isfinite(Description.MaximumMotorForce) ||
            !std::isfinite(Description.RestLength) || !std::isfinite(Description.SpringFrequency) ||
            !std::isfinite(Description.SpringDamping) || Description.MaximumMotorForce < 0 ||
            Description.SpringFrequency < 0 || Description.SpringFrequency > 15 ||
            Description.SpringDamping < 0 || Description.SpringDamping > 2 ||
            Description.LowerLimit > Description.UpperLimit) return false;
        switch (Description.Kind) {
        case PhysicsConstraintKind::Weld:
        case PhysicsConstraintKind::BallSocket:
            return !Description.EnableLimit && !Description.EnableMotor &&
                Description.MotorSpeed == 0 && Description.MaximumMotorForce == 0 &&
                Description.SpringFrequency == 0;
        case PhysicsConstraintKind::Hinge:
            return Description.SpringFrequency == 0 &&
                (!Description.EnableLimit ||
                    (Description.LowerLimit >= -3.1f && Description.UpperLimit <= 3.1f)) &&
                (!Description.EnableMotor || Description.MaximumMotorForce > 0);
        case PhysicsConstraintKind::Slider:
            return Description.SpringFrequency == 0 &&
                (!Description.EnableLimit ||
                    (Description.LowerLimit >= -100'000.0f && Description.UpperLimit <= 100'000.0f)) &&
                (!Description.EnableMotor || Description.MaximumMotorForce > 0);
        case PhysicsConstraintKind::Distance:
            return !Description.EnableMotor && Description.MotorSpeed == 0 &&
                Description.MaximumMotorForce == 0 && Description.RestLength >= 0.001f &&
                Description.RestLength <= 100'000.0f &&
                (!Description.EnableLimit || (Description.SpringFrequency > 0 &&
                    Description.LowerLimit >= 0.001f && Description.UpperLimit <= 100'000.0f));
        }
        return false;
    }
#ifdef PHYSICS_GANTRIA_CONVERGENCE
    constexpr std::uint32_t SoftIterationsPerCollisionStep = 5;
    constexpr std::uint32_t ConstraintSweepsPerIteration = 2;
#else
    constexpr std::uint32_t SoftIterationsPerCollisionStep = 12;
    constexpr std::uint32_t ConstraintSweepsPerIteration = 1;
#endif
    constexpr std::uint32_t SoftIntegrationsPerFrame =
        2 * 8 * SoftIterationsPerCollisionStep;

    float SoftDampingRate(float Damping, float DeltaTime) {
        constexpr double Integrations = double(SoftIntegrationsPerFrame);
        if (Damping == 0.0f) return 0.0f;
        if (Damping == 1.0f)
            return static_cast<float>(Integrations / double(DeltaTime));
        return static_cast<float>(-std::expm1(std::log1p(-double(Damping)) /
            Integrations) * Integrations / double(DeltaTime));
    }
    bool SameTransform(const CFrame &A, const CFrame &B) {
        if (A.Position.x != B.Position.x || A.Position.y != B.Position.y ||
            A.Position.z != B.Position.z) return false;
        for (int Column = 0; Column != 3; ++Column)
            for (int Row = 0; Row != 3; ++Row)
                if (A.Rotation[Column][Row] != B.Rotation[Column][Row]) return false;
        return true;
    }
    PhysicsOperationResult InvalidBody() {
        return {PhysicsOperationStatus::InvalidId, "Experimental Jolt body ID is invalid or stale"};
    }
    PhysicsOperationResult Unsupported(const char *Message) {
        return {PhysicsOperationStatus::BackendFailure, Message};
    }
    float BodyMass(const PhysicsBodyDesc &Description) {
        const auto Size = Description.Shape.Size;
        float Volume = Size.x * Size.y * Size.z;
        if (Description.Shape.Kind == PhysicsShapeKind::Ball) {
            const float Radius = std::min({Size.x, Size.y, Size.z}) * 0.5f;
            Volume = (4.0f / 3.0f) * 3.14159265358979323846f * Radius * Radius * Radius;
        } else if (Description.Shape.Kind == PhysicsShapeKind::Cylinder) {
            const float Radius = std::min(Size.x * 0.5f, Size.z * 0.25f);
            Volume = 3.14159265358979323846f * Radius * Radius * Size.y;
        } else if (Description.Shape.Kind == PhysicsShapeKind::Wedge) Volume *= 0.5f;
        else if (Description.Shape.Kind == PhysicsShapeKind::CornerWedge) Volume *= 0.25f;
        return std::max(0.001f, Description.Density * Volume);
    }
    bool ValidMassProperties(const PhysicsBodyDesc &Description,
                             const JPH::ShapeRefC &Shape) {
        if (Description.Anchored || Description.Kinematic) return true;
        auto Mass = Shape->GetMassProperties();
        if (!(Mass.mMass > 0) || !std::isfinite(Mass.mMass)) return false;
        const float TargetMass = BodyMass(Description);
        if (!(TargetMass > 0) || !std::isfinite(TargetMass)) return false;
        Mass.ScaleToMass(TargetMass);
        if (!(Mass.mMass > 0) || !std::isfinite(Mass.mMass)) return false;
        for (unsigned Index = 0; Index < 3; ++Index) {
            const auto Column = Mass.mInertia.GetColumn3(Index);
            if (!std::isfinite(Column.GetX()) || !std::isfinite(Column.GetY()) ||
                !std::isfinite(Column.GetZ()) || !std::isfinite(Column.LengthSq()))
                return false;
        }
        return Mass.mInertia.GetColumn3(0).GetX() > 0 &&
            Mass.mInertia.GetColumn3(1).GetY() > 0 &&
            Mass.mInertia.GetColumn3(2).GetZ() > 0;
    }
    bool SameMesh(const std::shared_ptr<const PhysicsTriangleMesh> &A,
                  const std::shared_ptr<const PhysicsTriangleMesh> &B) {
        if (A == B) return true;
        return A && B && A->ResourceId == B->ResourceId &&
            A->ContentRevision == B->ContentRevision &&
            A->Positions == B->Positions && A->Indices == B->Indices;
    }
    class JoltRuntime final {
    public:
        JoltRuntime() {
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
            if (!JPH::Allocate && !JPH::Reallocate && !JPH::Free &&
                !JPH::AlignedAllocate && !JPH::AlignedFree)
                JPH::RegisterDefaultAllocator();
            if (!JPH::Allocate || !JPH::Reallocate || !JPH::Free ||
                !JPH::AlignedAllocate || !JPH::AlignedFree)
                throw std::runtime_error("[Physics:JoltProvider] incomplete Jolt allocator registration");
#endif
            if (JPH::Factory::sInstance) return;
            OwnedFactory = std::make_unique<JPH::Factory>();
            JPH::Factory::sInstance = OwnedFactory.get();
            JPH::RegisterTypes();
        }
        ~JoltRuntime() {
            if (!OwnedFactory) return;
            JPH::UnregisterTypes();
            JPH::Factory::sInstance = nullptr;
        }
        JoltRuntime(const JoltRuntime &) = delete;
        JoltRuntime &operator=(const JoltRuntime &) = delete;
    private:
        std::unique_ptr<JPH::Factory> OwnedFactory;
    };
    PhysicsRaycastResult NormalizeHits(PhysicsRaycastResult Result) {
        std::map<PhysicsBodyId, PhysicsRaycastHit> Nearest;
        for (auto Hit : Result.Candidates) {
            const float NormalLength = glm::length(Hit.Normal);
            if (!Hit.Body.IsValid() || !Finite(Hit.Position) || !Finite(Hit.Normal) ||
                !std::isfinite(Hit.Distance) || Hit.Distance < 0 ||
                !std::isfinite(NormalLength) || NormalLength < 1.0e-6f)
                return {.Status = PhysicsOperationStatus::BackendFailure,
                    .Message = "Jolt query returned nonfinite or invalid hit geometry"};
            Hit.Normal /= NormalLength;
            const auto Existing = Nearest.find(Hit.Body);
            const bool PreferredNormal = Existing != Nearest.end() && Hit.Distance == Existing->second.Distance &&
                std::tie(Hit.Normal.x, Hit.Normal.y, Hit.Normal.z) <
                    std::tie(Existing->second.Normal.x, Existing->second.Normal.y, Existing->second.Normal.z);
            if (Existing == Nearest.end() || Hit.Distance < Existing->second.Distance || PreferredNormal)
                Nearest[Hit.Body] = Hit;
        }
        if (Nearest.size() > MaximumRaycastCandidates)
            return {.Status = PhysicsOperationStatus::BackendFailure,
                .Message = "Jolt query unique-body candidate limit exceeded"};
        Result.Candidates.clear();
        Result.Candidates.reserve(Nearest.size());
        for (const auto &[Body, Hit] : Nearest) Result.Candidates.push_back(Hit);
        std::ranges::sort(Result.Candidates, [](const auto &A, const auto &B) {
            return A.Distance != B.Distance ? A.Distance < B.Distance : A.Body < B.Body;
        });
        return Result;
    }

    struct LayerPolicy {
        std::uint16_t Group = 0;
        std::uint16_t Mask = MaximumPhysicsCollisionMask;
        bool Static = false;
        bool Participates = true;
        bool Soft = false;
    };
    class LayerTable final : public JPH::BroadPhaseLayerInterface,
                             public JPH::ObjectLayerPairFilter,
                             public JPH::ObjectVsBroadPhaseLayerFilter {
    public:
        LayerTable() { Policies.push_back({}); }
        JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer Layer) const override {
            return JPH::BroadPhaseLayer(Layer < Policies.size() && !Policies[Layer].Static ? 1 : 0);
        }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer Layer) const override {
            return Layer.GetValue() == 0 ? "Static" : "Moving";
        }
#endif
        bool ShouldCollide(JPH::ObjectLayer A, JPH::ObjectLayer B) const override {
            if (A >= Policies.size() || B >= Policies.size()) return false;
            const auto &First = Policies[A];
            const auto &Second = Policies[B];
            return First.Participates && Second.Participates && !(First.Static && Second.Static) &&
                !(First.Soft && Second.Soft) &&
                PhysicsCanCollide(First.Group, First.Mask, Second.Group, Second.Mask);
        }
        bool ShouldCollide(JPH::ObjectLayer A, JPH::BroadPhaseLayer B) const override {
            return A < Policies.size() && Policies[A].Participates &&
                !(Policies[A].Static && B.GetValue() == 0);
        }
        JPH::ObjectLayer GetLayer(const PhysicsBodyDesc &Description, bool Soft = false) {
            const LayerPolicy Candidate{Description.CollisionGroup, Description.CollisionMask,
                Description.Anchored, Description.CanCollide || Description.CanTouch, Soft};
            for (std::size_t Index = 0; Index < Policies.size(); ++Index) {
                const auto &Existing = Policies[Index];
                if (Existing.Group == Candidate.Group && Existing.Mask == Candidate.Mask &&
                    Existing.Static == Candidate.Static && Existing.Participates == Candidate.Participates &&
                    Existing.Soft == Candidate.Soft)
                    return static_cast<JPH::ObjectLayer>(Index);
            }
            if (Policies.size() >= JPH::cObjectLayerInvalid) return JPH::cObjectLayerInvalid;
            Policies.push_back(Candidate);
            return static_cast<JPH::ObjectLayer>(Policies.size() - 1);
        }
    private:
        std::vector<LayerPolicy> Policies;
    };

    struct BodySlot {
        std::uint32_t Generation = 1;
        bool Live = false;
        bool Retired = false;
        JPH::BodyID Native;
        PhysicsBodyDesc Description;
        JPH::ShapeRefC Shape;
        std::size_t NativeMeshBytes = 0;
        glm::vec3 PendingForce{0.0f};
        glm::vec3 PendingTorque{0.0f};
    };
    struct ConstraintSlot {
        std::uint32_t Generation = 1;
        bool Live = false;
        bool Retired = false;
        PhysicsConstraintDesc Description;
        JPH::Ref<JPH::Constraint> Native;
    };

    class ContactCapture final : public JPH::ContactListener {
    public:
        JPH::ValidateResult OnContactValidate(const JPH::Body &A, const JPH::Body &B,
                                               JPH::RVec3Arg, const JPH::CollideShapeResult &) override {
            std::scoped_lock Lock(Mutex);
            return Suppressed.contains(PairOf(A.GetID(), B.GetID()))
                ? JPH::ValidateResult::RejectAllContactsForThisBodyPair
                : JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }
        void OnContactAdded(const JPH::Body &A, const JPH::Body &B,
                            const JPH::ContactManifold &Manifold, JPH::ContactSettings &Settings) override {
            Record(A, B, Manifold, Settings);
        }
        void OnContactPersisted(const JPH::Body &A, const JPH::Body &B,
                                const JPH::ContactManifold &Manifold, JPH::ContactSettings &Settings) override {
            Record(A, B, Manifold, Settings);
        }
        void OnContactRemoved(const JPH::SubShapeIDPair &Pair) override {
            std::scoped_lock Lock(Mutex);
            ActiveSubShapes.erase(Pair);
        }
        void ForgetBody(std::uint32_t Native) {
            std::scoped_lock Lock(Mutex);
            for (auto It = ActiveSubShapes.begin(); It != ActiveSubShapes.end();) {
                if (It->GetBody1ID().GetIndexAndSequenceNumber() == Native ||
                    It->GetBody2ID().GetIndexAndSequenceNumber() == Native)
                    It = ActiveSubShapes.erase(It);
                else ++It;
            }
            for (auto It = Suppressed.begin(); It != Suppressed.end();) {
                if (It->first.first == Native || It->first.second == Native)
                    It = Suppressed.erase(It);
                else ++It;
            }
        }
        void SuppressPair(JPH::BodyID A, JPH::BodyID B) {
            std::scoped_lock Lock(Mutex);
            const auto Pair = PairOf(A, B);
            ++Suppressed[Pair];
            for (auto It = ActiveSubShapes.begin(); It != ActiveSubShapes.end();) {
                if (PairOf(It->GetBody1ID(), It->GetBody2ID()) == Pair)
                    It = ActiveSubShapes.erase(It);
                else ++It;
            }
        }
        void RestorePair(JPH::BodyID A, JPH::BodyID B) {
            std::scoped_lock Lock(Mutex);
            const auto Pair = PairOf(A, B);
            const auto It = Suppressed.find(Pair);
            if (It != Suppressed.end() && --It->second == 0) Suppressed.erase(It);
        }
        std::set<std::pair<std::uint32_t, std::uint32_t>> CurrentPairs() const {
            std::scoped_lock Lock(Mutex);
            std::set<std::pair<std::uint32_t, std::uint32_t>> Result;
            for (const auto &Pair : ActiveSubShapes) {
                const auto First = Pair.GetBody1ID().GetIndexAndSequenceNumber();
                const auto Second = Pair.GetBody2ID().GetIndexAndSequenceNumber();
                Result.emplace(std::min(First, Second), std::max(First, Second));
            }
            return Result;
        }
    private:
        static std::pair<std::uint32_t, std::uint32_t> PairOf(JPH::BodyID A, JPH::BodyID B) {
            const auto First = A.GetIndexAndSequenceNumber();
            const auto Second = B.GetIndexAndSequenceNumber();
            return {std::min(First, Second), std::max(First, Second)};
        }
        void Record(const JPH::Body &A, const JPH::Body &B,
                    const JPH::ContactManifold &Manifold, JPH::ContactSettings &Settings) {
            std::scoped_lock Lock(Mutex);
            if (Suppressed.contains(PairOf(A.GetID(), B.GetID()))) {
                Settings.mIsSensor = true;
                return;
            }
            Settings.mIsSensor = A.IsSensor() || B.IsSensor();
            ActiveSubShapes.emplace(A.GetID(), Manifold.mSubShapeID1,
                B.GetID(), Manifold.mSubShapeID2);
        }
        mutable std::mutex Mutex;
        std::set<JPH::SubShapeIDPair> ActiveSubShapes;
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> Suppressed;
    };

    class SoftContactCapture final : public JPH::SoftBodyContactListener {
    public:
        void OnSoftBodyContactAdded(const JPH::Body &Soft,
                                    const JPH::SoftBodyManifold &Manifold) override {
            std::scoped_lock Lock(Mutex);
            for (const auto &Vertex : Manifold.GetVertices()) {
                if (!Manifold.HasContact(Vertex)) continue;
                const auto Other = Manifold.GetContactBodyID(Vertex);
                if (Other.IsInvalid()) continue;
                auto &Count = ContactedVertices[{Soft.GetID().GetIndexAndSequenceNumber(),
                    Other.GetIndexAndSequenceNumber()}];
                if (Count != std::numeric_limits<std::uint64_t>::max()) ++Count;
            }
        }
        std::uint64_t VertexCount(JPH::BodyID Soft, JPH::BodyID Rigid) const {
            std::scoped_lock Lock(Mutex);
            const auto Found = ContactedVertices.find({Soft.GetIndexAndSequenceNumber(),
                Rigid.GetIndexAndSequenceNumber()});
            return Found == ContactedVertices.end() ? 0 : Found->second;
        }
        void Clear() {
            std::scoped_lock Lock(Mutex);
            ContactedVertices.clear();
        }
        void ForgetBody(JPH::BodyID Body) {
            std::scoped_lock Lock(Mutex);
            const auto Native = Body.GetIndexAndSequenceNumber();
            for (auto It = ContactedVertices.begin(); It != ContactedVertices.end();) {
                if (It->first.first == Native || It->first.second == Native)
                    It = ContactedVertices.erase(It);
                else ++It;
            }
        }
    private:
        mutable std::mutex Mutex;
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint64_t> ContactedVertices;
    };

    class QueryBodyFilter final : public JPH::BodyFilter {
    public:
        QueryBodyFilter(const std::unordered_map<std::uint32_t, PhysicsBodyId> &Map,
                        const std::vector<BodySlot> &Slots, const PhysicsQueryFilter &Filter)
            : NativeMap(Map), BodySlots(Slots), Request(Filter) {}
        bool ShouldCollide(const JPH::BodyID &Native) const override {
            const auto Found = NativeMap.find(Native.GetIndexAndSequenceNumber());
            if (Found == NativeMap.end()) return false;
            const auto &Record = BodySlots[Found->second.Slot];
            if (!Record.Live || !Record.Description.CanCollide ||
                !PhysicsCanCollide(Request.CollisionGroup, Request.CollisionMask,
                    Record.Description.CollisionGroup, Record.Description.CollisionMask)) return false;
            const bool Listed = std::find(Request.Bodies.begin(), Request.Bodies.end(), Found->second) != Request.Bodies.end();
            return Request.Type == PhysicsQueryFilterType::Include ? Listed : !Listed;
        }
    private:
        const std::unordered_map<std::uint32_t, PhysicsBodyId> &NativeMap;
        const std::vector<BodySlot> &BodySlots;
        const PhysicsQueryFilter &Request;
    };
#ifdef PHYSICS_GANTRIA_ATTACHMENTS
    struct RigidSoftAttachmentRecord {
        SoftBodyId Soft;
        PhysicsBodyId Rigid;
        std::uint32_t Vertex = 0;
        std::unique_ptr<JPH::SoftBodyRigidAttachment> Native;
    };
#endif

    class RigidJoltBackend final : public IPhysicsBackend {
    public:
        friend class JoltSoftBackend;
        RigidJoltBackend(const PhysicsWorldConfig &Config, int WorkerBudget, bool Unified = false)
            : Configuration(Config), Temp(32 * 1024 * 1024),
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
              TempStop(TempStart, WorldAdmissionProfile.TempArena),
#endif
              UnifiedMode(Unified) {
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
            {
                AdmissionMeasure Measure(WorldAdmissionProfile.Runtime);
#endif
            static JoltRuntime Runtime;
            (void)Runtime;
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
            }
#endif
            if (!Finite(Config.Gravity) || WorkerBudget < 1 || WorkerBudget > 8) return;
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
            {
                AdmissionMeasure Measure(WorldAdmissionProfile.Jobs);
#endif
            if (WorkerBudget == 1) Jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
            else Jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs,
                JPH::cMaxPhysicsBarriers, WorkerBudget - 1);
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
            }
            {
                AdmissionMeasure Measure(WorldAdmissionProfile.WorldInit);
#endif
            World.Init(65536, 0, 65536, 65536, Layers, Layers, Layers);
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
            }
#endif
#ifdef PHYSICS_GANTRIA_SLEEP_FIX
            auto PhysicsSettings = World.GetPhysicsSettings();
            PhysicsSettings.mUseFullUpdateDeltaForSleep = true;
            World.SetPhysicsSettings(PhysicsSettings);
#endif
            World.SetGravity(ToJolt(Config.Gravity));
            World.SetContactListener(&Contacts);
            BodySlots.emplace_back();
            ConstraintSlots.emplace_back();
            Residency.ResourceLimit = std::min(Config.CollisionResourceLimit,
                MaximumPhysicsCollisionResources);
            Residency.ConsumerLimit = std::min(Config.CollisionConsumerLimit,
                MaximumPhysicsCollisionConsumers);
            Residency.CanonicalByteLimit = std::min(Config.CollisionCanonicalByteLimit,
                MaximumPhysicsCollisionCanonicalBytes);
            Residency.NativeByteLimit = std::min(Config.CollisionNativeByteLimit,
                MaximumPhysicsCollisionNativeBytes);
            Residency.MeshShapeLimit = std::min(Config.CollisionMeshShapeLimit,
                MaximumPhysicsCollisionMeshShapes);
            Residency.NativeShapeBytesKnown = true;
            Valid = true;
        }
        ~RigidJoltBackend() override {
            if (!Valid) return;
            for (std::uint32_t Index = 1; Index < ConstraintSlots.size(); ++Index)
                if (ConstraintSlots[Index].Live) (void)DestroyConstraint({Index, ConstraintSlots[Index].Generation});
            for (std::uint32_t Index = 1; Index < BodySlots.size(); ++Index)
                if (BodySlots[Index].Live) (void)DestroyBody({Index, BodySlots[Index].Generation});
        }
        bool IsValid() const override { return Valid; }
        void SetBeforeUnifiedStep(std::function<bool(float)> Callback) {
            BeforeUnifiedStep = std::move(Callback);
        }
        std::optional<JoltStepSchedule> GetStepSchedule() const {
            return UnifiedMode ? LastStepSchedule : std::nullopt;
        }
        void SetRigidFastScheduleEnabled(bool Enabled) { RigidFastScheduleEnabled = Enabled; }
        std::size_t NativeRigidContactPairCount() const { return Contacts.CurrentPairs().size(); }
        void FailNextTouchUpdateAfterNativeMutation() {
            FailTouchUpdateAfterNativeMutationOnce = true;
        }
        void SetSoftContactCapture(bool Enabled) {
            // Private fixture setup only: caller must not change listeners during Step.
            if (Enabled) SoftContacts.Clear();
            World.SetSoftBodyContactListener(Enabled ? &SoftContacts : nullptr);
        }
        std::uint64_t SoftRigidContactVertices(JPH::BodyID Soft, PhysicsBodyId Rigid) const {
            const auto *RigidSlot = Find(Rigid);
            return RigidSlot && !Soft.IsInvalid()
                ? SoftContacts.VertexCount(Soft, RigidSlot->Native) : 0;
        }
#ifdef PHYSICS_GANTRIA_ATTACHMENTS
        PhysicsOperationResult ConnectAttachment(SoftBodyId SoftId, JPH::BodyID SoftNative,
                                                 std::uint32_t Vertex, PhysicsBodyId RigidId,
                                                 glm::vec3 RigidLocalPoint) {
            auto *RigidSlot = Find(RigidId);
            if (!RigidSlot || SoftNative.IsInvalid())
                return {PhysicsOperationStatus::InvalidId, "Invalid Jolt attachment endpoint"};
            if (!Finite(RigidLocalPoint))
                return {PhysicsOperationStatus::InvalidDescription, "Nonfinite Jolt attachment point"};
            if (std::ranges::any_of(Attachments, [&](const auto &Record) {
                return Record.Soft == SoftId || Record.Rigid == RigidId;
            })) return Unsupported("Jolt attachment endpoint already has a live connection");
            auto Native = std::make_unique<JPH::SoftBodyRigidAttachment>();
            JPH::SoftBodyRigidAttachment::Settings Settings;
            if (!Native->Connect(World, SoftNative, Vertex, RigidSlot->Native,
                ToJolt(RigidLocalPoint), Settings))
                return Unsupported("Jolt dynamic attachment rejected native endpoint or settings");
            Attachments.push_back({SoftId, RigidId, Vertex, std::move(Native)});
            return {};
        }
        PhysicsOperationResult DisconnectAttachment(SoftBodyId SoftId, PhysicsBodyId RigidId) {
            const auto It = std::find_if(Attachments.begin(), Attachments.end(),
                [&](const auto &Record) { return Record.Soft == SoftId && Record.Rigid == RigidId; });
            if (It == Attachments.end())
                return {PhysicsOperationStatus::InvalidId, "Invalid Jolt attachment identity"};
            It->Native->Disconnect();
            Attachments.erase(It);
            return {};
        }
        std::optional<JoltRigidSoftAttachmentState> AttachmentState(
            SoftBodyId SoftId, PhysicsBodyId RigidId) const {
            const auto It = std::find_if(Attachments.begin(), Attachments.end(),
                [&](const auto &Record) { return Record.Soft == SoftId && Record.Rigid == RigidId; });
            if (It == Attachments.end()) return std::nullopt;
            return JoltRigidSoftAttachmentState{
                It->Native->GetStatus() == JPH::SoftBodyRigidAttachment::EStatus::Active,
                It->Native->GetAppliedStepCount(),
                FromJolt(It->Native->GetLastImpulseOnSoftBody())};
        }
        void DisconnectAttachmentsForRigid(PhysicsBodyId Id) {
            std::erase_if(Attachments, [&](auto &Record) {
                if (Record.Rigid != Id) return false;
                Record.Native->Disconnect();
                return true;
            });
        }
        void DisconnectAttachmentsForSoft(SoftBodyId Id) {
            std::erase_if(Attachments, [&](auto &Record) {
                if (Record.Soft != Id) return false;
                Record.Native->Disconnect();
                return true;
            });
        }
        bool HasAttachmentOnRigid(PhysicsBodyId Id) const {
            return std::ranges::any_of(Attachments,
                [&](const auto &Record) { return Record.Rigid == Id; });
        }
        bool IsAttachedSoftVertex(SoftBodyId Id, std::uint32_t Vertex) const {
            return std::ranges::any_of(Attachments, [&](const auto &Record) {
                return Record.Soft == Id && Record.Vertex == Vertex;
            });
        }
#endif
        bool IsBodyValid(PhysicsBodyId Id) const override { return Find(Id) != nullptr; }
        bool IsConstraintValid(PhysicsConstraintId Id) const override { return Find(Id) != nullptr; }

        PhysicsBodyId CreateBody(const PhysicsBodyDesc &Description) override {
            if (!Valid || !ValidBody(Description)) return {};
            const auto Layer = Layers.GetLayer(Description);
            if (Layer == JPH::cObjectLayerInvalid) return {};
            auto Shape = MakeShape(Description.Shape);
            if (!Shape) return {};
            if (!ValidMassProperties(Description, Shape)) return {};
            if (!CheckMeshAdmission(Description, Shape, nullptr).Succeeded()) return {};
            std::uint32_t Index = 1;
            for (; Index < BodySlots.size() && (BodySlots[Index].Live || BodySlots[Index].Retired); ++Index) {}
            if (Index == BodySlots.size()) BodySlots.emplace_back();
            PhysicsBodyId Id{Index, BodySlots[Index].Generation};
            NativeMap.reserve(NativeMap.size() + 1);
            std::unordered_map<std::uint32_t, PhysicsBodyId> StagedMap;
            StagedMap.emplace(0, Id);
            auto NativeNode = StagedMap.extract(0);
            const bool PriorTouchGuardUncertain = TouchGuardUncertain;
            if (Description.CanTouch) TouchGuardUncertain = true;
            const auto Native = AddNativeBody(Description, Layer, Shape);
            if (Native.IsInvalid()) return {};
            auto &Slot = BodySlots[Index];
            Slot.Live = true;
            Slot.Native = Native;
            Slot.Shape = Shape;
            Slot.Description = Description;
            Slot.PendingForce = {};
            Slot.PendingTorque = {};
            if (Description.CanTouch) ++TouchBodies;
            AdmitMesh(Slot);
            NativeNode.key() = Native.GetIndexAndSequenceNumber();
            NativeMap.insert(std::move(NativeNode));
            if (Description.Anchored && (Description.CanCollide || Description.CanTouch))
                WakeBodiesInNewBounds(Native);
            if (Description.CanTouch) TouchGuardUncertain = PriorTouchGuardUncertain;
            return Id;
        }

        PhysicsOperationResult UpdateBody(PhysicsBodyId Id, const PhysicsBodyDesc &Description) override {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            if (!ValidBody(Description)) return {PhysicsOperationStatus::InvalidDescription, "Invalid Jolt body description"};
            const bool ShapeChanged = Description.Shape.Kind != Slot->Description.Shape.Kind ||
                Description.Shape.Size.x != Slot->Description.Shape.Size.x ||
                Description.Shape.Size.y != Slot->Description.Shape.Size.y ||
                Description.Shape.Size.z != Slot->Description.Shape.Size.z ||
                !SameMesh(Description.Shape.Mesh, Slot->Description.Shape.Mesh);
            const bool MeshTransition =
                (Description.Shape.Kind == PhysicsShapeKind::StaticTriangleMesh) !=
                (Slot->Description.Shape.Kind == PhysicsShapeKind::StaticTriangleMesh);
            JPH::ShapeRefC NewShape;
            if (ShapeChanged) {
                NewShape = MakeShape(Description.Shape);
                if (!NewShape) return {PhysicsOperationStatus::InvalidDescription, "Invalid replacement shape"};
                const auto Admission = CheckMeshAdmission(Description, NewShape, Slot);
                if (!Admission.Succeeded()) return Admission;
            }
            if (!ValidMassProperties(Description, ShapeChanged ? NewShape : Slot->Shape))
                return {PhysicsOperationStatus::InvalidDescription,
                    "Jolt body mass or inertia exceeds native range"};
            const auto Layer = Layers.GetLayer(Description);
            if (Layer == JPH::cObjectLayerInvalid) return {PhysicsOperationStatus::ResourceLimit, "Jolt layer limit"};
            const bool IntroducesTouch = Description.CanTouch && !Slot->Description.CanTouch;
            const bool PriorTouchGuardUncertain = TouchGuardUncertain;
            auto &Interface = World.GetBodyInterface();
            const auto Motion = Description.Anchored ? JPH::EMotionType::Static
                : Description.Kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic;
            const bool MotionChanged = Description.Anchored != Slot->Description.Anchored ||
                Description.Kinematic != Slot->Description.Kinematic;
            const bool ContactGeometryChanged = ShapeChanged || MotionChanged ||
                !SameTransform(Description.Transform, Slot->Description.Transform) ||
                Description.CollisionGroup != Slot->Description.CollisionGroup ||
                Description.CollisionMask != Slot->Description.CollisionMask ||
                Description.CanCollide != Slot->Description.CanCollide ||
                Description.CanTouch != Slot->Description.CanTouch;
#ifdef PHYSICS_GANTRIA_ATTACHMENTS
            if ((MotionChanged || MeshTransition) && HasAttachmentOnRigid(Id))
                return Unsupported("Jolt attached rigid motion or mesh transition is unsupported");
#endif
            if (MeshTransition) {
                for (const auto &Constraint : ConstraintSlots)
                    if (Constraint.Live && (Constraint.Description.BodyA == Id ||
                        Constraint.Description.BodyB == Id))
                        return Unsupported("Jolt mesh transition with attached constraint is unsupported");
                NativeMap.reserve(NativeMap.size() + 1);
                std::unordered_map<std::uint32_t, PhysicsBodyId> StagedMap;
                StagedMap.emplace(0, Id);
                auto NativeNode = StagedMap.extract(0);
                if (IntroducesTouch) TouchGuardUncertain = true;
                const auto Replacement = AddNativeBody(Description, Layer, NewShape);
                if (Replacement.IsInvalid())
                    return {PhysicsOperationStatus::BackendFailure, "Jolt mesh transition body admission failed"};
                WakeContactNeighbors(Slot->Native);
                NativeNode.key() = Replacement.GetIndexAndSequenceNumber();
                NativeMap.insert(std::move(NativeNode));
                const auto OldNative = Slot->Native.GetIndexAndSequenceNumber();
                Interface.RemoveBody(Slot->Native);
                Interface.DestroyBody(Slot->Native);
                NativeMap.erase(OldNative);
                ForgetContactInvalidations(OldNative);
                Contacts.ForgetBody(OldNative);
                SoftContacts.ForgetBody(Slot->Native);
                for (auto It = PreviousContacts.begin(); It != PreviousContacts.end();) {
                    if (It->first == OldNative || It->second == OldNative) It = PreviousContacts.erase(It);
                    else ++It;
                }
                Slot->Native = Replacement;
                ReleaseMesh(*Slot);
                Slot->Shape = NewShape;
                if (Slot->Description.CanTouch) --TouchBodies;
                if (Description.CanTouch) ++TouchBodies;
                Slot->Description = Description;
                AdmitMesh(*Slot);
                if (Description.Anchored && (Description.CanCollide || Description.CanTouch))
                    WakeBodiesInNewBounds(Replacement);
                if (IntroducesTouch) TouchGuardUncertain = PriorTouchGuardUncertain;
                return {};
            }
            if (IntroducesTouch) TouchGuardUncertain = true;
            if (ContactGeometryChanged) {
                ContactInvalidatedBodies.insert(Slot->Native.GetIndexAndSequenceNumber());
                WakeContactNeighbors(Slot->Native);
            }
            if (ShapeChanged)
                Interface.SetShape(Slot->Native, NewShape.GetPtr(), true, JPH::EActivation::Activate);
            if (MotionChanged)
                Interface.SetMotionType(Slot->Native, Motion, JPH::EActivation::Activate);
            if (!SameTransform(Description.Transform, Slot->Description.Transform) &&
                Description.Kinematic && std::getenv("GANTRIA_PROBE_MOVE"))
                Interface.MoveKinematic(Slot->Native, ToRVec(Description.Transform.Position),
                    ToJolt(Description.Transform), SoftBodyStepInterval / PhysicsEvaluation::ContactProbeParts());
            else if (!SameTransform(Description.Transform, Slot->Description.Transform))
                Interface.SetPositionAndRotation(Slot->Native, ToRVec(Description.Transform.Position),
                    ToJolt(Description.Transform), JPH::EActivation::Activate);
            if (Description.CollisionGroup != Slot->Description.CollisionGroup ||
                Description.CollisionMask != Slot->Description.CollisionMask ||
                Description.Anchored != Slot->Description.Anchored ||
                Description.CanCollide != Slot->Description.CanCollide ||
                Description.CanTouch != Slot->Description.CanTouch)
                Interface.SetObjectLayer(Slot->Native, Layer);
            if (Description.CanCollide != Slot->Description.CanCollide ||
                Description.CanTouch != Slot->Description.CanTouch)
                Interface.SetIsSensor(Slot->Native, !Description.CanCollide && Description.CanTouch);
            if (IntroducesTouch && FailTouchUpdateAfterNativeMutationOnce) {
                FailTouchUpdateAfterNativeMutationOnce = false;
                return {PhysicsOperationStatus::BackendFailure,
                    "Injected Jolt touch update failure after native mutation"};
            }
            if (Description.MovingBodyCCD != Slot->Description.MovingBodyCCD)
                Interface.SetMotionQuality(Slot->Native,
                    Description.MovingBodyCCD ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete);
            if (Motion != JPH::EMotionType::Static && Description.GravityScale != Slot->Description.GravityScale)
                Interface.SetGravityFactor(Slot->Native, Description.GravityScale);
            if (Description.Density != Slot->Description.Density || ShapeChanged || MotionChanged ||
                Description.LinearDamping != Slot->Description.LinearDamping ||
                Description.AngularDamping != Slot->Description.AngularDamping ||
                Description.SleepEnabled != Slot->Description.SleepEnabled) {
                JPH::BodyLockWrite Lock(World.GetBodyLockInterface(), Slot->Native);
                if (!Lock.Succeeded()) return {PhysicsOperationStatus::BackendFailure, "Jolt update body lock failed"};
                auto &Body = Lock.GetBody();
                if (Motion != JPH::EMotionType::Static)
                    Body.SetAllowSleeping(Configuration.EnableSleep && Description.SleepEnabled);
                if (auto *Properties = Body.GetMotionProperties()) {
                    Properties->SetLinearDamping(Description.LinearDamping);
                    Properties->SetAngularDamping(Description.AngularDamping);
                    if (Motion == JPH::EMotionType::Dynamic &&
                        (Description.Density != Slot->Description.Density || ShapeChanged || MotionChanged)) {
                        auto Mass = Body.GetShape()->GetMassProperties();
                        Mass.ScaleToMass(BodyMass(Description));
                        Properties->SetMassProperties(JPH::EAllowedDOFs::All, Mass);
                    }
                }
            }
            if (ShapeChanged) {
                ReleaseMesh(*Slot);
                Slot->Shape = NewShape;
            }
            if (Slot->Description.CanTouch) --TouchBodies;
            if (Description.CanTouch) ++TouchBodies;
            Slot->Description = Description;
            if (ShapeChanged) AdmitMesh(*Slot);
            if (ContactGeometryChanged && Description.Anchored &&
                (Description.CanCollide || Description.CanTouch))
                WakeBodiesInNewBounds(Slot->Native);
            if (IntroducesTouch) TouchGuardUncertain = PriorTouchGuardUncertain;
            return {};
        }

        PhysicsOperationResult DestroyBody(PhysicsBodyId Id) override {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            WakeContactNeighbors(Slot->Native);
#ifdef PHYSICS_GANTRIA_ATTACHMENTS
            DisconnectAttachmentsForRigid(Id);
#endif
            for (std::uint32_t Index = 1; Index < ConstraintSlots.size(); ++Index) {
                const auto &Constraint = ConstraintSlots[Index];
                if (Constraint.Live && (Constraint.Description.BodyA == Id || Constraint.Description.BodyB == Id))
                    (void)DestroyConstraint({Index, Constraint.Generation});
            }
            auto &Interface = World.GetBodyInterface();
            Interface.RemoveBody(Slot->Native);
            Interface.DestroyBody(Slot->Native);
            NativeMap.erase(Slot->Native.GetIndexAndSequenceNumber());
            ForgetContactInvalidations(Slot->Native.GetIndexAndSequenceNumber());
            Contacts.ForgetBody(Slot->Native.GetIndexAndSequenceNumber());
            SoftContacts.ForgetBody(Slot->Native);
            ReleaseMesh(*Slot);
            for (auto It = PreviousContacts.begin(); It != PreviousContacts.end();) {
                if (It->first == Slot->Native.GetIndexAndSequenceNumber() ||
                    It->second == Slot->Native.GetIndexAndSequenceNumber()) It = PreviousContacts.erase(It);
                else ++It;
            }
            for (auto It = PreviousNeutralContacts.begin(); It != PreviousNeutralContacts.end();) {
                if (It->first == Id || It->second == Id) It = PreviousNeutralContacts.erase(It);
                else ++It;
            }
            Slot->Shape = nullptr;
            Slot->PendingForce = {};
            Slot->PendingTorque = {};
            if (Slot->Description.CanTouch) --TouchBodies;
            Slot->Live = false;
            if (Slot->Generation == std::numeric_limits<std::uint32_t>::max()) Slot->Retired = true;
            else ++Slot->Generation;
            return {};
        }

        PhysicsConstraintId CreateConstraint(const PhysicsConstraintDesc &Description) override {
            return TryCreateConstraint(Description).Constraint;
        }
        PhysicsConstraintCreateResult TryCreateConstraint(const PhysicsConstraintDesc &Description) override {
            auto *A = Find(Description.BodyA);
            auto *B = Find(Description.BodyB);
            if (!A || !B || Description.BodyA == Description.BodyB)
                return {{}, {PhysicsOperationStatus::InvalidId, "Invalid Jolt joint endpoints"}};
            if (ActiveConstraints >= std::min(Configuration.ConstraintLimit, MaximumPhysicsConstraints))
                return {{}, {PhysicsOperationStatus::ResourceLimit, "Jolt joint limit"}};
            if (!ValidConstraint(Description))
                return {{}, {PhysicsOperationStatus::InvalidDescription, "Invalid Jolt joint frame or limit"}};
            const auto PointA = ToRVec(Description.LocalFrameA.Position);
            const auto PointB = ToRVec(Description.LocalFrameB.Position);
            const auto AxisA = ToJolt(Description.LocalFrameA.Rotation[Description.Kind == PhysicsConstraintKind::Slider ? 0 : 2]);
            const auto AxisB = ToJolt(Description.LocalFrameB.Rotation[Description.Kind == PhysicsConstraintKind::Slider ? 0 : 2]);
            const JPH::BodyID NativeIds[2]{A->Native, B->Native};
            JPH::BodyLockMultiWrite Lock(World.GetBodyLockInterface(), NativeIds, 2);
            auto *BodyA = Lock.GetBody(0);
            auto *BodyB = Lock.GetBody(1);
            if (!BodyA || !BodyB)
                return {{}, {PhysicsOperationStatus::BackendFailure, "Jolt joint body lock failed"}};
            JPH::Ref<JPH::Constraint> Native;
            if (Description.Kind == PhysicsConstraintKind::Weld) {
                JPH::FixedConstraintSettings Settings;
                Settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
                Settings.mPoint1 = PointA; Settings.mPoint2 = PointB;
                Settings.mAxisX1 = ToJolt(Description.LocalFrameA.Rotation[0]);
                Settings.mAxisY1 = ToJolt(Description.LocalFrameA.Rotation[1]);
                Settings.mAxisX2 = ToJolt(Description.LocalFrameB.Rotation[0]);
                Settings.mAxisY2 = ToJolt(Description.LocalFrameB.Rotation[1]);
                Native = Settings.Create(*BodyA, *BodyB);
            } else if (Description.Kind == PhysicsConstraintKind::BallSocket) {
                JPH::PointConstraintSettings Settings;
                Settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
                Settings.mPoint1 = PointA;
                Settings.mPoint2 = PointB;
                Native = Settings.Create(*BodyA, *BodyB);
            } else if (Description.Kind == PhysicsConstraintKind::Hinge) {
                JPH::HingeConstraintSettings Settings;
                Settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
                Settings.mPoint1 = PointA; Settings.mPoint2 = PointB;
                Settings.mHingeAxis1 = AxisA; Settings.mHingeAxis2 = AxisB;
                Settings.mNormalAxis1 = ToJolt(Description.LocalFrameA.Rotation[0]);
                Settings.mNormalAxis2 = ToJolt(Description.LocalFrameB.Rotation[0]);
                if (Description.EnableLimit) {
                    Settings.mLimitsMin = Description.LowerLimit;
                    Settings.mLimitsMax = Description.UpperLimit;
                }
                if (Description.EnableMotor) Settings.mMotorSettings.SetTorqueLimit(Description.MaximumMotorForce);
                auto *Joint = static_cast<JPH::HingeConstraint *>(Settings.Create(*BodyA, *BodyB));
                if (Description.EnableMotor) {
                    Joint->SetMotorState(JPH::EMotorState::Velocity);
                    Joint->SetTargetAngularVelocity(Description.MotorSpeed);
                }
                Native = Joint;
            } else if (Description.Kind == PhysicsConstraintKind::Slider) {
                JPH::SliderConstraintSettings Settings;
                Settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
                Settings.mPoint1 = PointA; Settings.mPoint2 = PointB;
                Settings.mSliderAxis1 = AxisA; Settings.mSliderAxis2 = AxisB;
                if (Description.EnableLimit) {
                    Settings.mLimitsMin = Description.LowerLimit;
                    Settings.mLimitsMax = Description.UpperLimit;
                }
                if (Description.EnableMotor) Settings.mMotorSettings.SetForceLimit(Description.MaximumMotorForce);
                auto *Joint = static_cast<JPH::SliderConstraint *>(Settings.Create(*BodyA, *BodyB));
                if (Description.EnableMotor) {
                    Joint->SetMotorState(JPH::EMotorState::Velocity);
                    Joint->SetTargetVelocity(Description.MotorSpeed);
                }
                Native = Joint;
            } else if (Description.Kind == PhysicsConstraintKind::Distance) {
                JPH::DistanceConstraintSettings Settings;
                Settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
                Settings.mPoint1 = PointA; Settings.mPoint2 = PointB;
                Settings.mMinDistance = Description.EnableLimit ? Description.LowerLimit : Description.RestLength;
                Settings.mMaxDistance = Description.EnableLimit ? Description.UpperLimit : Description.RestLength;
                if (Description.SpringFrequency > 0)
                    Settings.mLimitsSpringSettings = {JPH::ESpringMode::FrequencyAndDamping,
                        Description.SpringFrequency, Description.SpringDamping};
                Native = Settings.Create(*BodyA, *BodyB);
            } else return {{}, Unsupported("Jolt constraint kind is unsupported")};
            Lock.ReleaseLocks();
            if (!Native) return {{}, Unsupported("Jolt constraint creation failed")};
            const bool PriorJointGuardUncertain = JointGuardUncertain;
            JointGuardUncertain = true;
            World.AddConstraint(Native.GetPtr());
            if (!Description.CollideConnected) {
                Contacts.SuppressPair(A->Native, B->Native);
                ContactInvalidatedPairs.insert(ContactPairOf(A->Native, B->Native));
                auto &Interface = World.GetBodyInterface();
                Interface.InvalidateContactCache(A->Native);
                Interface.InvalidateContactCache(B->Native);
                if (!A->Description.Anchored) Interface.ActivateBody(A->Native);
                if (!B->Description.Anchored) Interface.ActivateBody(B->Native);
            }
            std::uint32_t Index = 1;
            for (; Index < ConstraintSlots.size() &&
                (ConstraintSlots[Index].Live || ConstraintSlots[Index].Retired); ++Index) {}
            if (Index == ConstraintSlots.size()) ConstraintSlots.emplace_back();
            auto &Slot = ConstraintSlots[Index];
            Slot.Live = true; Slot.Native = Native; Slot.Description = Description;
            ++ActiveConstraints;
            JointGuardUncertain = PriorJointGuardUncertain;
            return {{Index, Slot.Generation}, {}};
        }
        PhysicsOperationResult DestroyConstraint(PhysicsConstraintId Id) override {
            auto *Slot = Find(Id);
            if (!Slot) return {PhysicsOperationStatus::InvalidId, "Invalid Jolt joint ID"};
            if (!Slot->Description.CollideConnected) {
                const auto *A = Find(Slot->Description.BodyA);
                const auto *B = Find(Slot->Description.BodyB);
                if (A && B) {
                    Contacts.RestorePair(A->Native, B->Native);
                    ContactInvalidatedPairs.insert(ContactPairOf(A->Native, B->Native));
                    auto &Interface = World.GetBodyInterface();
                    Interface.InvalidateContactCache(A->Native);
                    Interface.InvalidateContactCache(B->Native);
                    if (!A->Description.Anchored) Interface.ActivateBody(A->Native);
                    if (!B->Description.Anchored) Interface.ActivateBody(B->Native);
                }
            }
            World.RemoveConstraint(Slot->Native.GetPtr());
            Slot->Native = nullptr;
            Slot->Live = false;
            if (Slot->Generation == std::numeric_limits<std::uint32_t>::max()) Slot->Retired = true;
            else ++Slot->Generation;
            --ActiveConstraints;
            return {};
        }

        PhysicsOperationResult ApplyLinearImpulse(PhysicsBodyId Id, glm::vec3 Impulse) override {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            if (!Finite(Impulse)) return {PhysicsOperationStatus::InvalidDescription, "Nonfinite impulse"};
            if (Slot->Description.Anchored) return {};
            if (Slot->Description.Kinematic) return Unsupported("Impulse requires a dynamic body");
            World.GetBodyInterface().AddImpulse(Slot->Native, ToJolt(Impulse));
            return {};
        }
        PhysicsOperationResult ApplyAngularImpulse(PhysicsBodyId Id, glm::vec3 Impulse) override {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            if (!Finite(Impulse) || Slot->Description.Kinematic)
                return {PhysicsOperationStatus::InvalidDescription, "Angular impulse requires a finite dynamic body"};
            if (!Slot->Description.Anchored) World.GetBodyInterface().AddAngularImpulse(Slot->Native, ToJolt(Impulse));
            return {};
        }
        PhysicsOperationResult ApplyImpulseAtPosition(PhysicsBodyId Id, glm::vec3 Impulse,
                                                     glm::vec3 Position) override {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            if (!Finite(Impulse) || !Finite(Position) || Slot->Description.Kinematic)
                return {PhysicsOperationStatus::InvalidDescription, "Point impulse requires a finite dynamic body"};
            if (!Slot->Description.Anchored)
                World.GetBodyInterface().AddImpulse(Slot->Native, ToJolt(Impulse), ToRVec(Position));
            return {};
        }
        PhysicsOperationResult ApplyForce(PhysicsBodyId Id, glm::vec3 Force) override {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            if (!Finite(Force) || Slot->Description.Anchored || Slot->Description.Kinematic)
                return {PhysicsOperationStatus::InvalidDescription, "Force requires finite dynamic body"};
            const auto Combined = Slot->PendingForce + Force;
            if (!Finite(Combined))
                return {PhysicsOperationStatus::InvalidDescription, "Rigid force accumulation overflow"};
            Slot->PendingForce = Combined;
            return {};
        }
        PhysicsOperationResult ApplyTorque(PhysicsBodyId Id, glm::vec3 Torque) override {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            if (!Finite(Torque) || Slot->Description.Anchored || Slot->Description.Kinematic)
                return {PhysicsOperationStatus::InvalidDescription, "Torque requires a finite dynamic body"};
            const auto Combined = Slot->PendingTorque + Torque;
            if (!Finite(Combined))
                return {PhysicsOperationStatus::InvalidDescription, "Rigid torque accumulation overflow"};
            Slot->PendingTorque = Combined;
            return {};
        }
        PhysicsOperationResult SetLinearVelocity(PhysicsBodyId Id, glm::vec3 Velocity) override {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            if (!Finite(Velocity) || Slot->Description.Anchored)
                return {PhysicsOperationStatus::InvalidDescription, "Velocity requires finite moving body"};
            World.GetBodyInterface().SetLinearVelocity(Slot->Native, ToJolt(Velocity));
            return {};
        }
        PhysicsOperationResult SetAngularVelocity(PhysicsBodyId Id, glm::vec3 Velocity) override {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            if (!Finite(Velocity) || Slot->Description.Anchored)
                return {PhysicsOperationStatus::InvalidDescription, "Angular velocity requires finite moving body"};
            World.GetBodyInterface().SetAngularVelocity(Slot->Native, ToJolt(Velocity));
            return {};
        }
        PhysicsOperationResult SetAwake(PhysicsBodyId Id, bool Awake) override {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            if (Slot->Description.Anchored) return {PhysicsOperationStatus::InvalidDescription, "Static body cannot sleep"};
            if (Awake) World.GetBodyInterface().ActivateBody(Slot->Native);
            else World.GetBodyInterface().DeactivateBody(Slot->Native);
            return {};
        }
        PhysicsOperationResult SetGravity(glm::vec3 Gravity) override {
            if (!Finite(Gravity)) return {PhysicsOperationStatus::InvalidDescription, "Nonfinite gravity"};
            World.SetGravity(ToJolt(Gravity));
            return {};
        }
        std::optional<PhysicsBodyState> GetBodyState(PhysicsBodyId Id) const override {
            const auto *Slot = Find(Id);
            if (!Slot) return std::nullopt;
            const auto &Interface = World.GetBodyInterface();
            JPH::RVec3 Position; JPH::Quat Rotation;
            Interface.GetPositionAndRotation(Slot->Native, Position, Rotation);
            PhysicsBodyState State;
            State.Transform = FromJolt(Position, Rotation);
            if (!Slot->Description.Anchored) {
                State.LinearVelocity = FromJolt(Interface.GetLinearVelocity(Slot->Native));
                State.AngularVelocity = FromJolt(Interface.GetAngularVelocity(Slot->Native));
            }
            State.Awake = Interface.IsActive(Slot->Native);
            State.Description = Slot->Description;
            if (!Slot->Description.Anchored && !Slot->Description.Kinematic) {
                JPH::BodyLockRead Lock(World.GetBodyLockInterface(), Slot->Native);
                if (Lock.Succeeded()) {
                    const auto *Motion = Lock.GetBody().GetMotionProperties();
                    const float InvMass = Motion->GetInverseMass();
                    State.Mass = InvMass > 0 ? 1.0f / InvMass : 0.0f;
                    const auto Inverse = Motion->GetInverseInertiaDiagonal();
                    State.LocalInertia = {Inverse.GetX() > 0 ? 1.0f / Inverse.GetX() : 0.0f,
                        Inverse.GetY() > 0 ? 1.0f / Inverse.GetY() : 0.0f,
                        Inverse.GetZ() > 0 ? 1.0f / Inverse.GetZ() : 0.0f};
                }
            }
            return State;
        }
        PhysicsCollisionResidency GetCollisionResidency() const override { return Residency; }
        PhysicsKinematicMotionResult MoveKinematicCapsule(const PhysicsKinematicMotionRequest &Request) const override {
            PhysicsKinematicMotionResult Result{.Position = Request.Position, .Velocity = Request.Velocity};
            if (!Finite(Request.Position) || !Finite(Request.Translation) || !Finite(Request.Velocity) ||
                !std::isfinite(Request.Radius) || !std::isfinite(Request.Height) ||
                Request.Radius <= 0 || Request.Height < 2.0f * Request.Radius ||
                Request.Radius > MaximumPhysicsQueryExtent || Request.Height > MaximumPhysicsQueryExtent)
                return {.Status = PhysicsOperationStatus::InvalidDescription,
                    .Message = "Invalid Jolt kinematic capsule request"};
            const auto Cast = [&](glm::vec3 Position, glm::vec3 Direction) {
                PhysicsShapeCastRequest Query;
                Query.Shape.Kind = PhysicsQueryShapeKind::Capsule;
                Query.Shape.Radius = Request.Radius;
                Query.Shape.Height = Request.Height;
                Query.Shape.Transform.Position = Position;
                Query.Direction = Direction;
                return ShapeCast(Query);
            };
            glm::vec3 Remaining = Request.Translation;
            for (int Iteration = 0; Iteration < 5 && glm::length(Remaining) > 1.0e-4f; ++Iteration) {
                const auto Hits = Cast(Result.Position, Remaining);
                if (!Hits.Succeeded()) {
                    Result.Status = Hits.Status;
                    Result.Message = Hits.Message;
                    return Result;
                }
                if (Hits.Candidates.empty()) { Result.Position += Remaining; break; }
                const auto &Hit = Hits.Candidates.front();
                const float Distance = glm::length(Remaining);
                const float Fraction = std::clamp((Hit.Distance - 0.001f) / Distance, 0.0f, 1.0f);
                Result.Position += Remaining * Fraction;
                const auto Normal = Hit.Normal;
                Result.Collided = true;
                Result.ContactNormal = Normal;
                if (Normal.y > Result.FloorNormal.y) Result.FloorNormal = Normal;
                Remaining *= 1.0f - Fraction;
                Remaining -= Normal * std::min(0.0f, glm::dot(Remaining, Normal));
                Result.Velocity -= Normal * std::min(0.0f, glm::dot(Result.Velocity, Normal));
                if (Fraction <= 0 && glm::length(Remaining) >= Distance - 1.0e-5f) break;
            }
            Result.AppliedTranslation = Result.Position - Request.Position;
            const auto Floor = Cast(Result.Position, {0.0f, -0.05f, 0.0f});
            if (Floor.Succeeded() && !Floor.Candidates.empty() &&
                Floor.Candidates.front().Normal.y > Result.FloorNormal.y)
                Result.FloorNormal = Floor.Candidates.front().Normal;
            Result.HasFloor = Result.FloorNormal.y > 0.0f;
            return Result;
        }
        PhysicsRaycastResult Raycast(const PhysicsRaycastRequest &Request) const override {
            PhysicsRaycastResult Result;
            if (!Finite(Request.Origin) || !Finite(Request.Direction) ||
                glm::length(Request.Direction) < MinimumRaycastDistance ||
                glm::length(Request.Direction) > MaximumRaycastDistance ||
                Request.Filter.Bodies.size() > MaximumRaycastFilterBodies)
                return {.Status = PhysicsOperationStatus::InvalidDescription, .Message = "Invalid ray request"};
            QueryBodyFilter Filter(NativeMap, BodySlots, Request.Filter);
            JPH::AllHitCollisionCollector<JPH::CastRayCollector> Collector;
            const JPH::RRayCast Ray(ToRVec(Request.Origin), ToJolt(Request.Direction));
            JPH::RayCastSettings Settings;
            Settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
            World.GetNarrowPhaseQuery().CastRay(Ray, Settings, Collector, {}, {}, Filter);
            for (const auto &Hit : Collector.mHits) {
                if (Hit.mFraction < 0 || Hit.mFraction > 1) continue;
                const auto Found = NativeMap.find(Hit.mBodyID.GetIndexAndSequenceNumber());
                if (Found == NativeMap.end()) continue;
                const auto Point = Ray.GetPointOnRay(Hit.mFraction);
                JPH::BodyLockRead Lock(World.GetBodyLockInterface(), Hit.mBodyID);
                if (!Lock.Succeeded()) continue;
                const auto Normal = Lock.GetBody().GetWorldSpaceSurfaceNormal(Hit.mSubShapeID2, Point);
                Result.Candidates.push_back({Found->second, FromRVec(Point), FromJolt(Normal),
                    glm::length(Request.Direction) * Hit.mFraction});
            }
            return NormalizeHits(std::move(Result));
        }
        PhysicsRaycastResult ShapeCast(const PhysicsShapeCastRequest &Request) const override {
            PhysicsRaycastResult Result;
            if (!Finite(Request.Direction) || !Finite(Request.Shape.Transform) ||
                glm::length(Request.Direction) < MinimumRaycastDistance ||
                glm::length(Request.Direction) > MaximumRaycastDistance ||
                Request.Filter.Bodies.size() > MaximumRaycastFilterBodies)
                return {.Status = PhysicsOperationStatus::InvalidDescription, .Message = "Invalid shape cast"};
            auto Shape = GetQueryShape(Request.Shape);
            if (!Shape) return {.Status = PhysicsOperationStatus::InvalidDescription, .Message = "Invalid query shape"};
            QueryBodyFilter Filter(NativeMap, BodySlots, Request.Filter);
            JPH::AllHitCollisionCollector<JPH::CastShapeCollector> Collector;
            const auto Transform = JPH::RMat44::sRotationTranslation(
                ToJolt(Request.Shape.Transform), ToRVec(Request.Shape.Transform.Position));
            const auto Cast = JPH::RShapeCast::sFromWorldTransform(Shape.GetPtr(),
                JPH::Vec3::sReplicate(1.0f), Transform, ToJolt(Request.Direction));
            World.GetNarrowPhaseQuery().CastShape(Cast, {}, JPH::RVec3::sZero(), Collector, {}, {}, Filter);
            for (const auto &Hit : Collector.mHits) {
                if (Hit.mFraction <= 0 || Hit.mFraction > 1) continue;
                const auto Found = NativeMap.find(Hit.mBodyID2.GetIndexAndSequenceNumber());
                if (Found == NativeMap.end()) continue;
                const auto Axis = -Hit.mPenetrationAxis;
                Result.Candidates.push_back({Found->second, FromJolt(Hit.mContactPointOn2),
                    FromJolt(Axis), glm::length(Request.Direction) * Hit.mFraction});
            }
            return NormalizeHits(std::move(Result));
        }
        PhysicsOverlapResult Overlap(const PhysicsOverlapRequest &Request) const override {
            PhysicsOverlapResult Result;
            if (!Finite(Request.Shape.Transform) ||
                Request.Filter.Bodies.size() > MaximumRaycastFilterBodies)
                return {.Status = PhysicsOperationStatus::InvalidDescription, .Message = "Invalid overlap transform"};
            auto Shape = GetQueryShape(Request.Shape);
            if (!Shape) return {.Status = PhysicsOperationStatus::InvalidDescription, .Message = "Invalid overlap shape"};
            QueryBodyFilter Filter(NativeMap, BodySlots, Request.Filter);
            JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> Collector;
            const auto Transform = JPH::RMat44::sRotationTranslation(
                ToJolt(Request.Shape.Transform), ToRVec(Request.Shape.Transform.Position));
            World.GetNarrowPhaseQuery().CollideShape(Shape.GetPtr(), JPH::Vec3::sReplicate(1.0f),
                Transform, {}, JPH::RVec3::sZero(), Collector, {}, {}, Filter);
            std::set<PhysicsBodyId> UniqueBodies;
            for (const auto &Hit : Collector.mHits) {
                const auto Found = NativeMap.find(Hit.mBodyID2.GetIndexAndSequenceNumber());
                if (Found != NativeMap.end()) UniqueBodies.insert(Found->second);
                if (UniqueBodies.size() > MaximumRaycastCandidates)
                    return {.Status = PhysicsOperationStatus::BackendFailure,
                        .Message = "Jolt overlap unique-body candidate limit exceeded"};
            }
            Result.Bodies.assign(UniqueBodies.begin(), UniqueBodies.end());
            if (Result.Bodies.size() > MaximumPhysicsOverlapResults) {
                Result.Bodies.resize(MaximumPhysicsOverlapResults);
                Result.Truncated = true;
            }
            return Result;
        }
        PhysicsStepResult Step(const PhysicsStepConfig &Config) override {
            PhysicsStepResult Result;
            if (!std::isfinite(Config.DeltaTime) || Config.DeltaTime <= 0 ||
                Config.SubStepCount < 1 || Config.SubStepCount > 16)
                throw std::runtime_error("[Physics:JoltRigid] invalid experimental step configuration");
            for (const auto &Slot : BodySlots)
                if (Slot.Live && (!Finite(Slot.PendingForce) ||
                                  !Finite(Slot.PendingTorque)))
                    throw std::runtime_error("[Physics:JoltRigid] pending force or torque exceeds native range");
            if (UnifiedMode && BeforeUnifiedStep && !BeforeUnifiedStep(Config.DeltaTime))
                throw std::runtime_error("[Physics:JoltProvider] pending soft force application failed");
            const auto UpdateWorld = [&](float DeltaTime, int CollisionSteps) {
                for (auto &Slot : BodySlots) {
                    if (!Slot.Live || Slot.Description.Anchored || Slot.Description.Kinematic) continue;
                    if (Slot.PendingForce != glm::vec3(0.0f))
                        World.GetBodyInterface().AddForce(Slot.Native, ToJolt(Slot.PendingForce));
                    if (Slot.PendingTorque != glm::vec3(0.0f))
                        World.GetBodyInterface().AddTorque(Slot.Native, ToJolt(Slot.PendingTorque));
                }
                const auto Update = World.Update(DeltaTime, CollisionSteps, &Temp, Jobs.get());
                if (Update != JPH::EPhysicsUpdateError::None)
                    throw std::runtime_error("[Physics:JoltRigid] Jolt Update failed with code " +
                        std::to_string(static_cast<unsigned>(Update)));
            };
            const auto Emit = [&](const auto &Pair, PhysicsContactPhase Phase) {
                if (Result.Contacts.size() == MaximumPhysicsEventsPerStep) {
                    Result.EventsTruncated = true;
                    return;
                }
                Result.Contacts.push_back({Pair.first, Pair.second, Phase});
            };
            const auto CollectContacts = [&] {
                auto Current = Contacts.CurrentPairs();
                // Jolt retires the native manifold when an island sleeps even
                // though its physical contact remains. Retain that neutral pair
                // until an endpoint wakes or an authored geometry edit changes it.
                const auto &Interface = World.GetBodyInterface();
                for (const auto &Pair : PreviousContacts) {
                    if (Current.contains(Pair) ||
                        !NativeMap.contains(Pair.first) || !NativeMap.contains(Pair.second) ||
                        ContactInvalidatedBodies.contains(Pair.first) ||
                        ContactInvalidatedBodies.contains(Pair.second) ||
                        ContactInvalidatedPairs.contains(Pair)) continue;
                    if (!Interface.IsActive(JPH::BodyID(Pair.first)) &&
                        !Interface.IsActive(JPH::BodyID(Pair.second)))
                        Current.insert(Pair);
                }
                std::set<std::pair<PhysicsBodyId, PhysicsBodyId>> CurrentNeutral;
                for (const auto &Pair : Current) {
                    const auto A = NativeMap.find(Pair.first);
                    const auto B = NativeMap.find(Pair.second);
                    if (A == NativeMap.end() || B == NativeMap.end() || A->second == B->second)
                        continue;
                    CurrentNeutral.emplace(std::min(A->second, B->second),
                        std::max(A->second, B->second));
                }
                for (const auto &Pair : CurrentNeutral)
                    if (!PreviousNeutralContacts.contains(Pair))
                        Emit(Pair, PhysicsContactPhase::Began);
                for (const auto &Pair : PreviousNeutralContacts)
                    if (!CurrentNeutral.contains(Pair))
                        Emit(Pair, PhysicsContactPhase::Ended);
                PreviousContacts = Current;
                PreviousNeutralContacts = std::move(CurrentNeutral);
            };
            if (UnifiedMode) {
                bool Coupled = !RigidFastScheduleEnabled || Configuration.EnableSleep ||
                    EnabledSoftBodies != 0 ||
                    TouchBodies != 0 || TouchGuardUncertain || ActiveConstraints != 0 ||
                    JointGuardUncertain;
#ifdef PHYSICS_GANTRIA_ATTACHMENTS
                Coupled |= !Attachments.empty();
#endif
                if (Coupled) {
                    UpdateWorld(Config.DeltaTime * 0.5f, 8 / PhysicsEvaluation::ContactProbeParts());
                    CollectContacts();
                    UpdateWorld(Config.DeltaTime * 0.5f, 8 / PhysicsEvaluation::ContactProbeParts());
                    CollectContacts();
                } else {
                    UpdateWorld(Config.DeltaTime, std::max(4, Config.SubStepCount));
                    CollectContacts();
                }
                LastStepSchedule = JoltStepSchedule{
                    Coupled ? 2u : 1u, Coupled ? 8u : std::uint32_t(std::max(4, Config.SubStepCount)),
                    EnabledSoftBodies};
            } else {
                UpdateWorld(Config.DeltaTime, Config.SubStepCount);
                CollectContacts();
            }
            for (auto &Slot : BodySlots) {
                Slot.PendingForce = {};
                Slot.PendingTorque = {};
            }
            ContactInvalidatedBodies.clear();
            ContactInvalidatedPairs.clear();
            ++StepSequence;
            for (std::uint32_t Index = 1; Index < BodySlots.size(); ++Index) {
                const auto &Slot = BodySlots[Index];
                if (!Slot.Live || Slot.Description.Anchored) continue;
                JPH::RVec3 Position; JPH::Quat Rotation;
                World.GetBodyInterface().GetPositionAndRotation(Slot.Native, Position, Rotation);
                Result.Motions.push_back({{Index, Slot.Generation}, FromJolt(Position, Rotation)});
            }
            std::ranges::sort(Result.Contacts, [](const auto &A, const auto &B) {
                return std::tie(A.BodyA, A.BodyB, A.Phase) < std::tie(B.BodyA, B.BodyB, B.Phase);
            });
            return Result;
        }

    private:
        static std::pair<std::uint32_t, std::uint32_t> ContactPairOf(
            JPH::BodyID A, JPH::BodyID B) {
            const auto First = A.GetIndexAndSequenceNumber();
            const auto Second = B.GetIndexAndSequenceNumber();
            return {std::min(First, Second), std::max(First, Second)};
        }
        void ForgetContactInvalidations(std::uint32_t Native) {
            ContactInvalidatedBodies.erase(Native);
            for (auto It = ContactInvalidatedPairs.begin();
                 It != ContactInvalidatedPairs.end();) {
                if (It->first == Native || It->second == Native)
                    It = ContactInvalidatedPairs.erase(It);
                else ++It;
            }
        }
        void WakeContactNeighbors(JPH::BodyID Native) {
            const auto Packed = Native.GetIndexAndSequenceNumber();
            auto &Interface = World.GetBodyInterface();
            for (const auto &Pair : PreviousContacts) {
                if (Pair.first != Packed && Pair.second != Packed) continue;
                const auto Neighbor = Pair.first == Packed ? Pair.second : Pair.first;
                const auto Found = NativeMap.find(Neighbor);
                if (Found == NativeMap.end()) continue;
                const auto *Slot = Find(Found->second);
                if (Slot && !Slot->Description.Anchored && !Slot->Description.Kinematic)
                    Interface.ActivateBody(Slot->Native);
            }
        }
        void WakeBodiesInNewBounds(JPH::BodyID Native) {
            std::optional<JPH::AABox> Bounds;
            JPH::ObjectLayer Layer = JPH::cObjectLayerInvalid;
            {
                JPH::BodyLockRead Lock(World.GetBodyLockInterface(), Native);
                if (Lock.Succeeded()) {
                    Bounds = Lock.GetBody().GetWorldSpaceBounds();
                    Layer = Lock.GetBody().GetObjectLayer();
                }
            }
            if (!Bounds || Layer == JPH::cObjectLayerInvalid) return;
            JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> Candidates;
            World.GetBroadPhaseQuery().CollideAABox(*Bounds, Candidates,
                World.GetDefaultBroadPhaseLayerFilter(Layer), World.GetDefaultLayerFilter(Layer));
            std::vector<JPH::BodyID> Dormant;
            for (const auto Candidate : Candidates.mHits) {
                JPH::BodyLockRead Lock(World.GetBodyLockInterface(), Candidate);
                if (Lock.Succeeded() && !Lock.GetBody().IsStatic() && !Lock.GetBody().IsActive())
                    Dormant.push_back(Candidate);
            }
            if (!Dormant.empty())
                World.GetBodyInterface().ActivateBodies(Dormant.data(), static_cast<int>(Dormant.size()));
        }
        using MeshKey = std::pair<std::string, std::uint64_t>;
        struct MeshResource {
            const void *Positions = nullptr;
            const void *Indices = nullptr;
            std::size_t CanonicalBytes = 0;
            std::size_t Consumers = 0;
        };
        static MeshKey KeyOf(const PhysicsTriangleMesh &Mesh) {
            return {Mesh.ResourceId, Mesh.ContentRevision};
        }
        static std::size_t CanonicalBytesOf(const PhysicsTriangleMesh &Mesh) {
            return Mesh.Positions->size() * sizeof(glm::vec3) +
                Mesh.Indices->size() * sizeof(std::uint32_t);
        }
        PhysicsOperationResult CheckMeshAdmission(const PhysicsBodyDesc &Description,
                                                   const JPH::ShapeRefC &Shape,
                                                   const BodySlot *Old) {
            if (Description.Shape.Kind != PhysicsShapeKind::StaticTriangleMesh) return {};
            const auto &Mesh = *Description.Shape.Mesh;
            const auto Key = KeyOf(Mesh);
            const auto Found = MeshResources.find(Key);
            const auto OldMesh = Old && Old->Description.Shape.Kind == PhysicsShapeKind::StaticTriangleMesh;
            const auto OldKey = OldMesh ? KeyOf(*Old->Description.Shape.Mesh) : MeshKey{};
            const bool OldResourceRetires = OldMesh && OldKey != Key &&
                MeshResources.at(OldKey).Consumers == 1;
            const bool NewResource = Found == MeshResources.end();
            const auto NativeBytes = Shape->GetStats().mSizeBytes;
            Residency.PeakBuildNativeBytes = std::max(Residency.PeakBuildNativeBytes,
                Residency.NativeMeshBytes + NativeBytes);
            const auto CanonicalBytes = CanonicalBytesOf(Mesh);
            const auto ResourceCount = MeshResources.size() + std::size_t(NewResource) -
                std::size_t(OldResourceRetires);
            const auto CanonicalCharge = Residency.CanonicalBytes +
                (NewResource ? CanonicalBytes : 0) -
                (OldResourceRetires ? MeshResources.at(OldKey).CanonicalBytes : 0);
            const auto NativeCharge = Residency.NativeMeshBytes + NativeBytes -
                (OldMesh ? Old->NativeMeshBytes : 0);
            const auto ConsumerCount = Residency.ActiveConsumers + 1 - std::size_t(OldMesh);
            std::string Failure;
            if (Found != MeshResources.end() &&
                (Found->second.Positions != Mesh.Positions.get() ||
                 Found->second.Indices != Mesh.Indices.get()))
                Failure = "Jolt mesh resource revision has conflicting canonical buffers";
            else if (ResourceCount > Residency.ResourceLimit ||
                ConsumerCount > Residency.ConsumerLimit ||
                ConsumerCount > Residency.MeshShapeLimit ||
                CanonicalCharge > Residency.CanonicalByteLimit ||
                NativeCharge > Residency.NativeByteLimit)
                Failure = "Jolt canonical mesh residency limit exceeded";
            if (Failure.empty()) return {};
            ++Residency.FailedAdmissions;
            Residency.LastAdmissionFailure = Failure;
            return {PhysicsOperationStatus::ResourceLimit, Failure};
        }
        void AdmitMesh(BodySlot &Slot) {
            if (Slot.Description.Shape.Kind != PhysicsShapeKind::StaticTriangleMesh) return;
            const auto &Mesh = *Slot.Description.Shape.Mesh;
            const auto Key = KeyOf(Mesh);
            auto [It, Inserted] = MeshResources.try_emplace(Key);
            if (Inserted) {
                It->second.Positions = Mesh.Positions.get();
                It->second.Indices = Mesh.Indices.get();
                It->second.CanonicalBytes = CanonicalBytesOf(Mesh);
                Residency.CanonicalBytes += It->second.CanonicalBytes;
                ++Residency.CreatedResources;
                ++Residency.ActiveResources;
            }
            ++It->second.Consumers;
            Slot.NativeMeshBytes = Slot.Shape->GetStats().mSizeBytes;
            Residency.NativeMeshBytes += Slot.NativeMeshBytes;
            Residency.PeakBuildNativeBytes = std::max(Residency.PeakBuildNativeBytes,
                Residency.NativeMeshBytes);
            ++Residency.ActiveConsumers;
            ++Residency.CreatedConsumers;
            ++Residency.ActiveMeshShapes;
            ++Residency.CreatedMeshShapes;
        }
        void ReleaseMesh(BodySlot &Slot) {
            if (Slot.Description.Shape.Kind != PhysicsShapeKind::StaticTriangleMesh) return;
            auto It = MeshResources.find(KeyOf(*Slot.Description.Shape.Mesh));
            if (It == MeshResources.end()) return;
            Residency.NativeMeshBytes -= Slot.NativeMeshBytes;
            Slot.NativeMeshBytes = 0;
            --Residency.ActiveConsumers;
            ++Residency.RetiredConsumers;
            --Residency.ActiveMeshShapes;
            ++Residency.RetiredMeshShapes;
            if (--It->second.Consumers == 0) {
                Residency.CanonicalBytes -= It->second.CanonicalBytes;
                --Residency.ActiveResources;
                ++Residency.RetiredResources;
                MeshResources.erase(It);
            }
        }
        JPH::BodyID AddNativeBody(const PhysicsBodyDesc &Description, JPH::ObjectLayer Layer,
                                  const JPH::ShapeRefC &Shape) {
            const auto Motion = Description.Anchored ? JPH::EMotionType::Static
                : Description.Kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic;
            JPH::BodyCreationSettings Settings(Shape.GetPtr(), ToRVec(Description.Transform.Position),
                ToJolt(Description.Transform), Motion, Layer);
            Settings.mAllowDynamicOrKinematic =
                Description.Shape.Kind != PhysicsShapeKind::StaticTriangleMesh;
            Settings.mIsSensor = !Description.CanCollide && Description.CanTouch;
            Settings.mMotionQuality = Description.MovingBodyCCD ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
            Settings.mAllowSleeping = Configuration.EnableSleep && Description.SleepEnabled;
            Settings.mLinearDamping = Description.LinearDamping;
            Settings.mAngularDamping = Description.AngularDamping;
            Settings.mGravityFactor = Description.GravityScale;
            if (Motion == JPH::EMotionType::Dynamic) {
                Settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
                Settings.mMassPropertiesOverride = Shape->GetMassProperties();
                Settings.mMassPropertiesOverride.ScaleToMass(BodyMass(Description));
            }
            return World.GetBodyInterface().CreateAndAddBody(Settings,
                Motion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        }
        bool ValidBody(const PhysicsBodyDesc &Value) const {
            if (!Finite(Value.Transform) || !Finite(Value.Shape.Size) ||
                Value.Shape.Size.x <= 0 || Value.Shape.Size.y <= 0 || Value.Shape.Size.z <= 0 ||
                Value.Shape.Size.x > MaximumPhysicsQueryExtent ||
                Value.Shape.Size.y > MaximumPhysicsQueryExtent ||
                Value.Shape.Size.z > MaximumPhysicsQueryExtent ||
                !std::isfinite(Value.Density) || Value.Density < 0 ||
                !std::isfinite(Value.GravityScale) || Value.GravityScale < 0 ||
                !std::isfinite(Value.LinearDamping) || Value.LinearDamping < 0 ||
                !std::isfinite(Value.AngularDamping) || Value.AngularDamping < 0 ||
                Value.CollisionGroup > MaximumPhysicsCollisionGroup ||
                (Value.Anchored && Value.Kinematic) ||
                (Value.MovingBodyCCD && (Value.Anchored || Value.Kinematic || !Value.CanCollide))) return false;
            const auto HalfSize = Value.Shape.Size * 0.5f;
            if (Value.Shape.Kind != PhysicsShapeKind::StaticTriangleMesh &&
                (HalfSize.x <= 0 || HalfSize.y <= 0 || HalfSize.z <= 0)) return false;
            if (Value.Shape.Kind == PhysicsShapeKind::Cylinder &&
                std::min(Value.Shape.Size.x * 0.5f, Value.Shape.Size.z * 0.25f) <= 0)
                return false;
            if (!Value.Anchored && !Value.Kinematic && !std::isfinite(BodyMass(Value))) return false;
            if (Value.Shape.Kind == PhysicsShapeKind::StaticTriangleMesh)
                return Value.Anchored && Value.Shape.Mesh && Value.Shape.Mesh->Positions &&
                    Value.Shape.Mesh->Indices && !Value.Shape.Mesh->ResourceId.empty() &&
                    Value.Shape.Mesh->ResourceId.size() <= MaximumPhysicsCollisionResourceIdBytes;
            return Value.Shape.Kind == PhysicsShapeKind::Box || Value.Shape.Kind == PhysicsShapeKind::Ball ||
                Value.Shape.Kind == PhysicsShapeKind::Cylinder || Value.Shape.Kind == PhysicsShapeKind::Wedge ||
                Value.Shape.Kind == PhysicsShapeKind::CornerWedge;
        }
        JPH::ShapeRefC MakeShape(const PhysicsShapeDesc &Description) const {
            if (Description.Kind == PhysicsShapeKind::Box)
                return new JPH::BoxShape(ToJolt(Description.Size * 0.5f), 0.0f);
            if (Description.Kind == PhysicsShapeKind::Ball)
                return new JPH::SphereShape(std::min({Description.Size.x, Description.Size.y, Description.Size.z}) * 0.5f);
            if (Description.Kind == PhysicsShapeKind::Cylinder)
                return new JPH::CylinderShape(Description.Size.y * 0.5f,
                    std::min(Description.Size.x * 0.5f, Description.Size.z * 0.25f), 0.0f);
            if (Description.Kind == PhysicsShapeKind::Wedge ||
                Description.Kind == PhysicsShapeKind::CornerWedge) {
                const auto H = Description.Size * 0.5f;
                JPH::Vec3 Points[6] = {
                    {-H.x, -H.y, -H.z}, {H.x, -H.y, -H.z},
                    {H.x, -H.y, H.z}, {-H.x, -H.y, H.z},
                    {H.x, H.y, -H.z}, {-H.x, H.y, -H.z},
                };
                if (Description.Kind == PhysicsShapeKind::CornerWedge)
                    Points[4] = {-H.x, H.y, -H.z};
                auto Result = JPH::ConvexHullShapeSettings(Points,
                    Description.Kind == PhysicsShapeKind::Wedge ? 6 : 5, 0.0f).Create();
                return Result.HasError() ? nullptr : Result.Get();
            }
            if (Description.Kind != PhysicsShapeKind::StaticTriangleMesh || !Description.Mesh ||
                !Description.Mesh->Positions || !Description.Mesh->Indices) return nullptr;
            const auto &Positions = *Description.Mesh->Positions;
            const auto &Indices = *Description.Mesh->Indices;
            if (Positions.size() < 3 || Positions.size() > 262144 || Indices.empty() ||
                Indices.size() % 3 != 0 || Indices.size() > 1048575) return nullptr;
            JPH::VertexList Vertices;
            JPH::IndexedTriangleList Triangles;
            Vertices.reserve(Positions.size()); Triangles.reserve(Indices.size() / 3);
            for (const auto &Position : Positions) {
                const auto Scaled = Position * Description.Size;
                if (!Finite(Scaled)) return nullptr;
                Vertices.emplace_back(Scaled.x, Scaled.y, Scaled.z);
            }
            for (std::size_t Index = 0; Index < Indices.size(); Index += 3) {
                if (Indices[Index] >= Positions.size() || Indices[Index + 1] >= Positions.size() ||
                    Indices[Index + 2] >= Positions.size()) return nullptr;
                Triangles.emplace_back(Indices[Index], Indices[Index + 1], Indices[Index + 2]);
            }
            auto Created = JPH::MeshShapeSettings(std::move(Vertices), std::move(Triangles)).Create();
            return Created.HasError() ? nullptr : Created.Get();
        }
        JPH::ShapeRefC MakeQueryShape(const PhysicsQueryShape &Query) const {
            if (Query.Kind == PhysicsQueryShapeKind::Sphere && std::isfinite(Query.Radius) && Query.Radius > 0)
                return new JPH::SphereShape(Query.Radius);
            if (Query.Kind == PhysicsQueryShapeKind::Box && Finite(Query.Size) &&
                Query.Size.x > 0 && Query.Size.y > 0 && Query.Size.z > 0) {
                const auto HalfExtent = Query.Size * 0.5f;
                if (!Finite(HalfExtent) || HalfExtent.x <= 0 ||
                    HalfExtent.y <= 0 || HalfExtent.z <= 0) return nullptr;
                return new JPH::BoxShape(ToJolt(HalfExtent), 0.0f);
            }
            if (Query.Kind == PhysicsQueryShapeKind::Capsule && std::isfinite(Query.Radius) &&
                std::isfinite(Query.Height) && Query.Radius > 0 && Query.Height >= 2 * Query.Radius)
                return JPH::CapsuleShapeSettings(0.5f * Query.Height - Query.Radius, Query.Radius).Create().Get();
            return nullptr;
        }
        JPH::ShapeRefC GetQueryShape(const PhysicsQueryShape &Query) const {
            if (!std::isfinite(Query.Radius) || !std::isfinite(Query.Height) ||
                !Finite(Query.Size) || Query.Radius > MaximumPhysicsQueryExtent ||
                Query.Height > MaximumPhysicsQueryExtent ||
                Query.Size.x > MaximumPhysicsQueryExtent || Query.Size.y > MaximumPhysicsQueryExtent ||
                Query.Size.z > MaximumPhysicsQueryExtent) return nullptr;
            const auto Key = std::make_tuple(static_cast<int>(Query.Kind), Query.Radius,
                Query.Height, Query.Size.x, Query.Size.y, Query.Size.z);
            const auto Found = QueryShapes.find(Key);
            if (Found != QueryShapes.end()) return Found->second;
            auto Shape = MakeQueryShape(Query);
            if (Shape && QueryShapes.size() < 256) QueryShapes.emplace(Key, Shape);
            return Shape;
        }
        BodySlot *Find(PhysicsBodyId Id) {
            return Id.IsValid() && Id.Slot < BodySlots.size() && BodySlots[Id.Slot].Live &&
                BodySlots[Id.Slot].Generation == Id.Generation ? &BodySlots[Id.Slot] : nullptr;
        }
        const BodySlot *Find(PhysicsBodyId Id) const {
            return Id.IsValid() && Id.Slot < BodySlots.size() && BodySlots[Id.Slot].Live &&
                BodySlots[Id.Slot].Generation == Id.Generation ? &BodySlots[Id.Slot] : nullptr;
        }
        ConstraintSlot *Find(PhysicsConstraintId Id) {
            return Id.IsValid() && Id.Slot < ConstraintSlots.size() && ConstraintSlots[Id.Slot].Live &&
                ConstraintSlots[Id.Slot].Generation == Id.Generation ? &ConstraintSlots[Id.Slot] : nullptr;
        }
        const ConstraintSlot *Find(PhysicsConstraintId Id) const {
            return Id.IsValid() && Id.Slot < ConstraintSlots.size() && ConstraintSlots[Id.Slot].Live &&
                ConstraintSlots[Id.Slot].Generation == Id.Generation ? &ConstraintSlots[Id.Slot] : nullptr;
        }
        PhysicsWorldConfig Configuration;
        LayerTable Layers;
        ContactCapture Contacts;
        SoftContactCapture SoftContacts;
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
        AdmissionStart TempStart;
#endif
        JPH::TempAllocatorImpl Temp;
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
        AdmissionStop TempStop;
#endif
        std::unique_ptr<JPH::JobSystem> Jobs;
        // Destruct World before Jobs, Temp and Layers, whose storage it may use.
        JPH::PhysicsSystem World;
#ifdef PHYSICS_GANTRIA_ATTACHMENTS
        std::vector<RigidSoftAttachmentRecord> Attachments;
#endif
        std::vector<BodySlot> BodySlots;
        std::vector<ConstraintSlot> ConstraintSlots;
        std::unordered_map<std::uint32_t, PhysicsBodyId> NativeMap;
        std::map<MeshKey, MeshResource> MeshResources;
        PhysicsCollisionResidency Residency;
        std::set<std::pair<std::uint32_t, std::uint32_t>> PreviousContacts;
        std::set<std::pair<PhysicsBodyId, PhysicsBodyId>> PreviousNeutralContacts;
        std::set<std::uint32_t> ContactInvalidatedBodies;
        std::set<std::pair<std::uint32_t, std::uint32_t>> ContactInvalidatedPairs;
        mutable std::map<std::tuple<int, float, float, float, float, float>, JPH::ShapeRefC> QueryShapes;
        std::size_t ActiveConstraints = 0;
        // A failed admission after native mutation must leave the cadence conservative.
        bool JointGuardUncertain = false;
        std::size_t TouchBodies = 0;
        std::size_t EnabledSoftBodies = 0;
        std::uint64_t StepSequence = 0;
        bool Valid = false;
        bool UnifiedMode = false;
        bool RigidFastScheduleEnabled = true;
        bool TouchGuardUncertain = false;
        bool FailTouchUpdateAfterNativeMutationOnce = false;
        std::function<bool(float)> BeforeUnifiedStep;
        std::optional<JoltStepSchedule> LastStepSchedule;
    };

    struct SoftConstruction {
        JPH::Ref<JPH::SoftBodySharedSettings> Shared;
        std::shared_ptr<std::vector<std::uint32_t>> Indices;
    };

    std::optional<SoftConstruction> BuildSoft(const SoftBodyDefinition &Definition,
                                               const SoftBodyWorldLimits &Limits) {
        const auto &Material = Definition.Material;
        const auto FiniteNonnegative = [](float Value) { return std::isfinite(Value) && Value >= 0.0f; };
        if (!Finite(Definition.Position) || !Finite(Definition.Size) ||
            Definition.Size.x <= 0 || Definition.Size.y <= 0 || Definition.Size.z <= 0 ||
            Definition.Size.x > MaximumPhysicsQueryExtent ||
            Definition.Size.y > MaximumPhysicsQueryExtent ||
            Definition.Size.z > MaximumPhysicsQueryExtent ||
            !std::isfinite(Material.ParticleMass) || Material.ParticleMass <= 0 ||
            !FiniteNonnegative(Material.Damping) || Material.Damping > 1.0f ||
            !FiniteNonnegative(Material.StretchCompliance) ||
            !FiniteNonnegative(Material.BendCompliance) || !FiniteNonnegative(Material.ShapeCompliance) ||
            !FiniteNonnegative(Material.VolumeCompliance) || !FiniteNonnegative(Material.Friction) ||
            !FiniteNonnegative(Material.Thickness) || Material.Friction > 1.0f ||
            Material.Thickness > 10.0f || Definition.ResolutionX < 2 ||
            Definition.ResolutionY < 2 || Definition.Attachments.size() > Limits.MaximumAttachments)
            return std::nullopt;
        if (Material.StretchCompliance <= 0 ||
            (Definition.Kind == SoftBodyKind::Cloth && Material.BendCompliance <= 0) ||
            (Definition.Kind == SoftBodyKind::Rubber &&
                (Material.ShapeCompliance <= 0 || Material.VolumeCompliance <= 0)))
            return std::nullopt;
        const std::uint64_t X = Definition.ResolutionX, Y = Definition.ResolutionY;
        const std::uint64_t Z = Definition.Kind == SoftBodyKind::Cloth ? 1 : Definition.ResolutionZ;
        const auto VertexLimit = std::min(Limits.MaximumVerticesPerBody, MaximumSoftBodyVertices);
        if (Z == 0 || (Definition.Kind == SoftBodyKind::Rubber && Z < 2) ||
            X > VertexLimit / Y || X * Y > VertexLimit / Z) return std::nullopt;
        const double InverseMassDouble = 1.0 / double(Material.ParticleMass);
        const double MaximumSize = std::max({double(Definition.Size.x),
            double(Definition.Size.y), double(Definition.Size.z)});
        const double MaximumMassMoment = double(Material.ParticleMass) *
            double(X * Y * Z) * (1.0 + MaximumSize * MaximumSize);
        if (!std::isfinite(InverseMassDouble) || InverseMassDouble <= 0 ||
            InverseMassDouble > double(std::numeric_limits<float>::max()) ||
            !std::isfinite(MaximumMassMoment) ||
            MaximumMassMoment > double(std::numeric_limits<float>::max()) / 16.0)
            return std::nullopt;
        const float InverseMass = static_cast<float>(InverseMassDouble);
        SoftConstruction Result;
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
        {
            AdmissionMeasure Measure(AdmissionProfile.Topology);
#endif
        Result = {new JPH::SoftBodySharedSettings,
            std::make_shared<std::vector<std::uint32_t>>()};
        const auto Index = [X, Y](std::uint32_t I, std::uint32_t J, std::uint32_t K = 0) {
            return static_cast<std::uint32_t>((K * Y + J) * X + I);
        };
        for (std::uint32_t K = 0; K < Z; ++K)
            for (std::uint32_t J = 0; J < Y; ++J)
                for (std::uint32_t I = 0; I < X; ++I) {
                    const float U = float(I) / float(X - 1) - 0.5f;
                    const float V = float(J) / float(Y - 1) - 0.5f;
                    const float W = Z > 1 ? float(K) / float(Z - 1) - 0.5f : 0.0f;
                    const glm::vec3 Position = Definition.Kind == SoftBodyKind::Cloth
                        ? glm::vec3(U * Definition.Size.x, -V * Definition.Size.y, 0.0f)
                        : glm::vec3(U * Definition.Size.x, V * Definition.Size.y, W * Definition.Size.z);
                    Result.Shared->mVertices.emplace_back(JPH::Float3(Position.x, Position.y, Position.z),
                        JPH::Float3(0, 0, 0), InverseMass);
                }
        std::set<std::uint32_t> Pinned;
        for (const auto &Attachment : Definition.Attachments) {
            const glm::dvec3 Local = glm::dvec3(Attachment.Position) -
                glm::dvec3(Definition.Position);
            if (Attachment.Vertex >= X * Y * Z || !Finite(Attachment.Position) ||
                std::abs(Local.x) > MaximumPhysicsQueryExtent ||
                std::abs(Local.y) > MaximumPhysicsQueryExtent ||
                std::abs(Local.z) > MaximumPhysicsQueryExtent ||
                !Pinned.insert(Attachment.Vertex).second) return std::nullopt;
        }
        const auto Edge = [&](std::uint32_t A, std::uint32_t B, float Compliance) {
            Result.Shared->mEdgeConstraints.emplace_back(A, B, Compliance);
        };
        const auto Face = [&](std::uint32_t A, std::uint32_t B, std::uint32_t C) {
            Result.Shared->mFaces.emplace_back(A, B, C);
            Result.Indices->insert(Result.Indices->end(), {A, B, C});
        };
        if (Definition.Kind == SoftBodyKind::Cloth) {
            for (std::uint32_t J = 0; J < Y; ++J) for (std::uint32_t I = 0; I < X; ++I) {
                const auto A = Index(I, J);
                if (I + 1 < X) Edge(A, Index(I + 1, J), Material.StretchCompliance);
                if (J + 1 < Y) Edge(A, Index(I, J + 1), Material.StretchCompliance);
                if (I + 1 < X && J + 1 < Y) {
                    Edge(A, Index(I + 1, J + 1), Material.StretchCompliance);
                    Edge(Index(I + 1, J), Index(I, J + 1), Material.StretchCompliance);
                    Face(A, Index(I, J + 1), Index(I + 1, J + 1));
                    Face(A, Index(I + 1, J + 1), Index(I + 1, J));
                }
                if (I + 2 < X) Edge(A, Index(I + 2, J), Material.BendCompliance);
                if (J + 2 < Y) Edge(A, Index(I, J + 2), Material.BendCompliance);
            }
        } else {
            for (std::uint32_t K = 0; K < Z; ++K) for (std::uint32_t J = 0; J < Y; ++J)
                for (std::uint32_t I = 0; I < X; ++I) {
                    const auto A = Index(I, J, K);
                    if (I + 1 < X) Edge(A, Index(I + 1, J, K), Material.StretchCompliance);
                    if (J + 1 < Y) Edge(A, Index(I, J + 1, K), Material.StretchCompliance);
                    if (K + 1 < Z) Edge(A, Index(I, J, K + 1), Material.StretchCompliance);
                }
            for (std::uint32_t K = 0; K + 1 < Z; ++K) for (std::uint32_t J = 0; J + 1 < Y; ++J)
                for (std::uint32_t I = 0; I + 1 < X; ++I) {
                    const auto A = Index(I, J, K), B = Index(I + 1, J, K);
                    const auto C = Index(I, J + 1, K), D = Index(I + 1, J + 1, K);
                    const auto E = Index(I, J, K + 1), F = Index(I + 1, J, K + 1);
                    const auto G = Index(I, J + 1, K + 1), H = Index(I + 1, J + 1, K + 1);
                    Result.Shared->mVolumeConstraints.emplace_back(A, B, C, E, Material.VolumeCompliance);
                    Result.Shared->mVolumeConstraints.emplace_back(B, D, C, H, Material.VolumeCompliance);
                    Result.Shared->mVolumeConstraints.emplace_back(B, C, E, H, Material.VolumeCompliance);
                    Result.Shared->mVolumeConstraints.emplace_back(B, E, F, H, Material.VolumeCompliance);
                    Result.Shared->mVolumeConstraints.emplace_back(C, G, E, H, Material.VolumeCompliance);
                    Edge(A, H, Material.ShapeCompliance);
                    Edge(B, G, Material.ShapeCompliance);
                    Edge(C, F, Material.ShapeCompliance);
                    Edge(D, E, Material.ShapeCompliance);
                    if (K == 0) { Face(A, B, D); Face(A, D, C); }
                    if (K + 2 == Z) { Face(E, G, H); Face(E, H, F); }
                    if (J == 0) { Face(A, E, F); Face(A, F, B); }
                    if (J + 2 == Y) { Face(C, D, H); Face(C, H, G); }
                    if (I == 0) { Face(A, C, G); Face(A, G, E); }
                    if (I + 2 == X) { Face(B, F, H); Face(B, H, D); }
                }
        }
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
        }
        {
            AdmissionMeasure Measure(AdmissionProfile.Precompute);
#endif
        const auto ConstraintCount = Result.Shared->mEdgeConstraints.size() +
            Result.Shared->mVolumeConstraints.size();
        if (ConstraintCount > Limits.MaximumConstraints) return std::nullopt;
        for (const auto &EdgeValue : Result.Shared->mEdgeConstraints) {
            const auto Difference = JPH::Vec3(Result.Shared->mVertices[EdgeValue.mVertex[1]].mPosition) -
                JPH::Vec3(Result.Shared->mVertices[EdgeValue.mVertex[0]].mPosition);
            const float LengthSquared = Difference.LengthSq();
            if (!(LengthSquared > 0) || !std::isfinite(LengthSquared)) return std::nullopt;
        }
        Result.Shared->CalculateEdgeLengths();
        Result.Shared->CalculateVolumeConstraintVolumes();
        for (const auto &EdgeValue : Result.Shared->mEdgeConstraints)
            if (!(EdgeValue.mRestLength > 0) || !std::isfinite(EdgeValue.mRestLength))
                return std::nullopt;
        for (const auto &VolumeValue : Result.Shared->mVolumeConstraints)
            if (!(VolumeValue.mSixRestVolume > 0) ||
                !std::isfinite(VolumeValue.mSixRestVolume)) return std::nullopt;
        for (const auto &Attachment : Definition.Attachments) {
            const auto Local = Attachment.Position - Definition.Position;
            Result.Shared->mVertices[Attachment.Vertex].mPosition = JPH::Float3(Local.x, Local.y, Local.z);
            Result.Shared->mVertices[Attachment.Vertex].mInvMass = 0.0f;
        }
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
        }
        {
            AdmissionMeasure Measure(AdmissionProfile.Optimize);
#endif
#ifdef PHYSICS_GANTRIA_GROUPING
        if (Definition.Kind == SoftBodyKind::Cloth && Result.Shared->mVertices.size() >= 16384) {
            JPH::SoftBodySharedSettings::OptimizationResults Remap;
            if (!Result.Shared->Optimize(Remap, PHYSICS_GANTRIA_LARGE_CLOTH_GROUP_BATCH))
                return std::nullopt;
        } else
#endif
        Result.Shared->Optimize();
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
        }
#endif
        return Result;
    }

    class JoltSoftBackend final : public ISoftBodyBackend {
    public:
        JoltSoftBackend(RigidJoltBackend &Owner, SoftBodyWorldLimits Limits)
            : Owner(Owner), Limits(Limits) { Slots.emplace_back(); }
        ~JoltSoftBackend() override { Shutdown(); }
        bool IsValid() const override { return !Stopped && Owner.IsValid(); }
        bool IsBodyValid(SoftBodyId Id) const override { return Find(Id) != nullptr; }
        RigidJoltBackend &SharedWorld() const { return Owner; }
        JPH::BodyID NativeBody(SoftBodyId Id) const {
            const auto *Slot = Find(Id);
            return Slot ? Slot->Native : JPH::BodyID();
        }
        SoftBodyId CreateBody(const SoftBodyDefinition &Definition) override {
            if (!IsValid() || LiveBodies >= Limits.MaximumBodies) return {};
            auto Prepared = PrepareConstruction(Definition);
            if (!Prepared) return {};
            auto &Construction = Prepared->Construction;
            const auto NewVertices = Construction.Shared->mVertices.size();
            const auto NewConstraints = Construction.Shared->mEdgeConstraints.size() +
                Construction.Shared->mVolumeConstraints.size();
            if (NewVertices > Limits.MaximumWorldVertices - LiveVertices ||
                NewConstraints > Limits.MaximumConstraints - LiveConstraints ||
                NewVertices > Limits.MaximumQueuedResultBytes / sizeof(glm::vec3))
                return {};
            const auto Layer = SoftLayer(Definition);
            if (Layer == JPH::cObjectLayerInvalid) return {};
            auto PreparedDefinition = Definition;
            std::uint32_t Index = 1;
            for (; Index < Slots.size() && (Slots[Index].Live || Slots[Index].Retired); ++Index) {}
            if (Index == Slots.size()) Slots.emplace_back();
            JPH::SoftBodyCreationSettings Settings(Construction.Shared,
                ToRVec(Definition.Position), JPH::Quat::sIdentity(), Layer);
            Settings.mNumIterations = SoftIterationsPerCollisionStep;
#ifdef PHYSICS_GANTRIA_JOLT
            Settings.mVelocityReconstruction = JPH::ESoftBodyVelocityReconstruction::ConstraintDelta;
#endif
#ifdef PHYSICS_GANTRIA_CONVERGENCE
            Settings.mNumConstraintIterations = PhysicsEvaluation::ContactProbeSweeps(ConstraintSweepsPerIteration);
            if (!Settings.IsConstraintConvergenceSupported()) return {};
#endif
            Settings.mAllowSleeping = false;
            Settings.mUpdatePosition = false;
            Settings.mFacesDoubleSided = true;
            Settings.mLinearDamping = SoftDampingRate(Definition.Material.Damping,
                SoftBodyStepInterval);
            Settings.mFriction = Definition.Material.Friction;
            Settings.mVertexRadius = Definition.Material.Thickness;
            JPH::BodyID Native;
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
            {
                AdmissionMeasure Measure(AdmissionProfile.NativeCreate);
#endif
            Native = Owner.World.GetBodyInterface().CreateAndAddSoftBody(Settings,
                Definition.Enabled ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
            }
#endif
            if (Native.IsInvalid()) return {};
            SoftBodyId CreatedId;
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
            {
                AdmissionMeasure Measure(AdmissionProfile.SlotCommit);
#endif
            auto &Slot = Slots[Index];
            Slot.Live = true;
            Slot.Native = Native;
            Slot.Definition = std::move(PreparedDefinition);
            Slot.Shared = std::move(Construction.Shared);
            Slot.Indices = std::move(Construction.Indices);
            Slot.PendingForce = {};
            Slot.ConstraintCount = NewConstraints;
            Slot.TopologyRevision = std::max<std::uint64_t>(1, Slot.TopologyRevision + 1);
            Slot.VertexRevision = std::max<std::uint64_t>(1, Slot.VertexRevision + 1);
            Slot.ExtractedSequence = Owner.StepSequence;
            Slot.EverSimulated = false;
            ++LiveBodies;
            if (Definition.Enabled) ++Owner.EnabledSoftBodies;
            LiveVertices += NewVertices;
            LiveConstraints += NewConstraints;
            if (!Prepared->CacheHit) Cached = std::move(Prepared->Candidate);
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
            else ++AdmissionProfile.CacheHits;
#endif
            CreatedId = {Index, Slot.Generation};
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
            }
#endif
            return CreatedId;
        }
        PhysicsOperationResult UpdateBody(SoftBodyId Id, const SoftBodyDefinition &Definition) override {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            if (SameExceptAttachments(Slot->Definition, Definition)) {
                std::set<std::uint32_t> Attached;
                if (Definition.Attachments.size() > Limits.MaximumAttachments)
                    return {PhysicsOperationStatus::InvalidDescription, "Jolt soft attachment limit"};
                for (const auto &Attachment : Definition.Attachments)
                    if (Attachment.Vertex >= Slot->Shared->mVertices.size() ||
                        !Finite(Attachment.Position) || !Attached.insert(Attachment.Vertex).second)
                        return {PhysicsOperationStatus::InvalidDescription, "Invalid Jolt soft attachment"};
                auto PreparedAttachments = Definition.Attachments;
#ifdef PHYSICS_GANTRIA_ATTACHMENTS
                for (const auto &Attachment : Definition.Attachments)
                    if (Owner.IsAttachedSoftVertex(Id, Attachment.Vertex))
                        return Unsupported("Jolt dynamic attachment vertex cannot become a fixed pin");
#endif
                JPH::BodyLockWrite Lock(Owner.World.GetBodyLockInterface(), Slot->Native);
                if (!Lock.Succeeded())
                    return {PhysicsOperationStatus::BackendFailure, "Jolt soft attachment lock failed"};
                auto *Motion = static_cast<JPH::SoftBodyMotionProperties *>(Lock.GetBody().GetMotionProperties());
                auto &Vertices = Motion->GetVertices();
                for (auto &Vertex : Vertices) Vertex.mInvMass = 1.0f / Definition.Material.ParticleMass;
                const auto Origin = FromRVec(Lock.GetBody().GetPosition());
                for (const auto &Attachment : Definition.Attachments) {
                    auto &Vertex = Vertices[Attachment.Vertex];
                    Vertex.mPosition = ToJolt(Attachment.Position - Origin);
                    Vertex.mVelocity = JPH::Vec3::sZero();
                    Vertex.mInvMass = 0.0f;
                }
                Motion->CalculateMassAndInertia();
                Slot->Definition.Attachments = std::move(PreparedAttachments);
                ++Slot->VertexRevision;
                return {};
            }
            auto Prepared = PrepareConstruction(Definition);
            if (!Prepared) return {PhysicsOperationStatus::InvalidDescription, "Invalid Jolt soft definition"};
            auto &Replacement = Prepared->Construction;
            const auto NewVertices = Replacement.Shared->mVertices.size();
            const auto NewConstraints = Replacement.Shared->mEdgeConstraints.size() +
                Replacement.Shared->mVolumeConstraints.size();
            if (NewVertices > Limits.MaximumWorldVertices - (LiveVertices - Slot->Shared->mVertices.size()) ||
                NewConstraints > Limits.MaximumConstraints - (LiveConstraints - Slot->ConstraintCount) ||
                NewVertices > Limits.MaximumQueuedResultBytes / sizeof(glm::vec3))
                return {PhysicsOperationStatus::InvalidDescription, "Invalid or oversized Jolt soft definition"};
            const auto Layer = SoftLayer(Definition);
            if (Layer == JPH::cObjectLayerInvalid)
                return {PhysicsOperationStatus::ResourceLimit, "Jolt soft layer limit"};
            auto PreparedDefinition = Definition;
            JPH::SoftBodyCreationSettings Settings(Replacement.Shared,
                ToRVec(Definition.Position), JPH::Quat::sIdentity(), Layer);
            Settings.mNumIterations = SoftIterationsPerCollisionStep;
#ifdef PHYSICS_GANTRIA_JOLT
            Settings.mVelocityReconstruction = JPH::ESoftBodyVelocityReconstruction::ConstraintDelta;
#endif
#ifdef PHYSICS_GANTRIA_CONVERGENCE
            Settings.mNumConstraintIterations = PhysicsEvaluation::ContactProbeSweeps(ConstraintSweepsPerIteration);
            if (!Settings.IsConstraintConvergenceSupported())
                return {PhysicsOperationStatus::InvalidDescription,
                    "Jolt repeated-sweep profile does not support this constraint graph"};
#endif
            Settings.mAllowSleeping = false;
            Settings.mUpdatePosition = false;
            Settings.mFacesDoubleSided = true;
            Settings.mLinearDamping = SoftDampingRate(Definition.Material.Damping,
                SoftBodyStepInterval);
            Settings.mFriction = Definition.Material.Friction;
            Settings.mVertexRadius = Definition.Material.Thickness;
            const auto Native = Owner.World.GetBodyInterface().CreateAndAddSoftBody(Settings,
                Definition.Enabled ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
            if (Native.IsInvalid()) return {PhysicsOperationStatus::BackendFailure, "Jolt soft replacement failed"};
            auto &Interface = Owner.World.GetBodyInterface();
#ifdef PHYSICS_GANTRIA_ATTACHMENTS
            Owner.DisconnectAttachmentsForSoft(Id);
#endif
            Interface.RemoveBody(Slot->Native);
            Interface.DestroyBody(Slot->Native);
            Owner.SoftContacts.ForgetBody(Slot->Native);
            LiveVertices = LiveVertices - Slot->Shared->mVertices.size() + Replacement.Shared->mVertices.size();
            LiveConstraints = LiveConstraints - Slot->ConstraintCount + NewConstraints;
            Slot->Native = Native;
            if (Slot->Definition.Enabled != Definition.Enabled) {
                if (Definition.Enabled) ++Owner.EnabledSoftBodies;
                else --Owner.EnabledSoftBodies;
            }
            Slot->Definition = std::move(PreparedDefinition);
            Slot->Shared = std::move(Replacement.Shared);
            Slot->Indices = std::move(Replacement.Indices);
            Slot->ConstraintCount = NewConstraints;
            ++Slot->TopologyRevision;
            ++Slot->VertexRevision;
            Slot->ExtractedSequence = Owner.StepSequence;
            Slot->EverSimulated = false;
            if (!Prepared->CacheHit) Cached = std::move(Prepared->Candidate);
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
            else ++AdmissionProfile.CacheHits;
#endif
            return {};
        }
        PhysicsOperationResult DestroyBody(SoftBodyId Id) override {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
#ifdef PHYSICS_GANTRIA_ATTACHMENTS
            Owner.DisconnectAttachmentsForSoft(Id);
#endif
            auto &Interface = Owner.World.GetBodyInterface();
            Interface.RemoveBody(Slot->Native);
            Interface.DestroyBody(Slot->Native);
            Owner.SoftContacts.ForgetBody(Slot->Native);
            LiveVertices -= Slot->Shared->mVertices.size();
            LiveConstraints -= Slot->ConstraintCount;
            --LiveBodies;
            if (Slot->Definition.Enabled) --Owner.EnabledSoftBodies;
            Slot->Live = false;
            Slot->Shared = nullptr;
            Slot->Indices.reset();
            Slot->PendingForce = {};
            if (Slot->Generation == std::numeric_limits<std::uint32_t>::max()) Slot->Retired = true;
            else ++Slot->Generation;
            return {};
        }
        PhysicsOperationResult ApplyForce(SoftBodyId Id, glm::vec3 Force) override {
            if (!Finite(Force)) return {PhysicsOperationStatus::InvalidDescription, "Nonfinite soft force"};
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            const auto Combined = Slot->PendingForce + Force;
            if (!Finite(Combined))
                return {PhysicsOperationStatus::InvalidDescription, "Soft force accumulation overflow"};
            Slot->PendingForce = Combined;
            return {};
        }
        bool FlushForces(float DeltaTime) {
            struct StagedForce {
                Slot *Target = nullptr;
                float BeforeDamping = 0;
                float AfterDamping = 0;
                std::vector<glm::vec3> Before;
                std::vector<glm::vec3> After;
            };
            std::vector<StagedForce> Pending;
            for (std::uint32_t Index = 1; Index < Slots.size(); ++Index) {
                auto &Slot = Slots[Index];
                if (!Slot.Live || !Slot.Definition.Enabled) continue;
                const auto VelocityImpulse = Slot.PendingForce * DeltaTime;
                if (!Finite(VelocityImpulse)) return false;
                JPH::BodyLockRead Lock(Owner.World.GetBodyLockInterface(), Slot.Native);
                if (!Lock.Succeeded()) return false;
                const auto *Motion = static_cast<const JPH::SoftBodyMotionProperties *>(
                    Lock.GetBody().GetMotionProperties());
                const auto Rate = SoftDampingRate(Slot.Definition.Material.Damping, DeltaTime);
                if (!std::isfinite(Rate) || Rate < 0) return false;
                const auto &Vertices = Motion->GetVertices();
                std::size_t Movable = 0;
                for (const auto &Vertex : Vertices) if (Vertex.mInvMass > 0) ++Movable;
                StagedForce Stage;
                Stage.Target = &Slot;
                Stage.BeforeDamping = Motion->GetLinearDamping();
                Stage.AfterDamping = Rate;
                if (Slot.PendingForce == glm::vec3(0.0f)) {
                    Pending.push_back(std::move(Stage));
                    continue;
                }
                Stage.Before.reserve(Vertices.size());
                Stage.After.reserve(Vertices.size());
                for (const auto &Vertex : Vertices) {
                    const auto Before = FromJolt(Vertex.mVelocity);
                    const auto After = Vertex.mInvMass > 0 && Movable > 0
                        ? Before + VelocityImpulse * (Vertex.mInvMass / float(Movable)) : Before;
                    if (!Finite(Before) || !Finite(After)) return false;
                    Stage.Before.push_back(Before);
                    Stage.After.push_back(After);
                }
                Pending.push_back(std::move(Stage));
            }
            std::size_t Applied = 0;
            for (; Applied < Pending.size(); ++Applied) {
                auto &Stage = Pending[Applied];
                JPH::BodyLockWrite Lock(Owner.World.GetBodyLockInterface(), Stage.Target->Native);
                if (!Lock.Succeeded()) break;
                auto *Motion = static_cast<JPH::SoftBodyMotionProperties *>(
                    Lock.GetBody().GetMotionProperties());
                auto &Vertices = Motion->GetVertices();
                if (!Stage.After.empty() && Vertices.size() != Stage.After.size()) break;
                Motion->SetLinearDamping(Stage.AfterDamping);
                for (std::size_t Index = 0; Index < Stage.After.size(); ++Index)
                    Vertices[Index].mVelocity = ToJolt(Stage.After[Index]);
            }
            if (Applied != Pending.size()) {
                for (std::size_t Prior = 0; Prior < Applied; ++Prior) {
                    auto &Stage = Pending[Prior];
                    JPH::BodyLockWrite Lock(Owner.World.GetBodyLockInterface(), Stage.Target->Native);
                    if (!Lock.Succeeded()) return false;
                    auto *Motion = static_cast<JPH::SoftBodyMotionProperties *>(
                        Lock.GetBody().GetMotionProperties());
                    Motion->SetLinearDamping(Stage.BeforeDamping);
                    auto &Vertices = Motion->GetVertices();
                    for (std::size_t Index = 0; Index < Stage.Before.size(); ++Index)
                        Vertices[Index].mVelocity = ToJolt(Stage.Before[Index]);
                }
                return false;
            }
            for (auto &Stage : Pending) Stage.Target->PendingForce = {};
            return true;
        }
        PhysicsOperationResult ApplyImpulse(SoftBodyId Id, glm::vec3 Impulse) override {
            if (!Finite(Impulse)) return {PhysicsOperationStatus::InvalidDescription, "Nonfinite soft impulse"};
            return AddVelocity(Id, Impulse, std::nullopt);
        }
        PhysicsOperationResult ApplyImpulseAtPosition(SoftBodyId Id, glm::vec3 Impulse,
                                                     glm::vec3 Position) override {
            if (!Finite(Impulse) || !Finite(Position))
                return {PhysicsOperationStatus::InvalidDescription, "Nonfinite soft point impulse"};
            return AddVelocity(Id, Impulse, Position);
        }
        std::optional<SoftBodyState> GetBodyState(SoftBodyId Id) const override {
            const auto *Slot = Find(Id);
            if (!Slot) return std::nullopt;
            return Extract(Id, *Slot, false);
        }
        std::optional<SoftBodyInspection> GetBodyInspection(SoftBodyId Id) const override {
            const auto *Slot = Find(Id);
            if (!Slot) return std::nullopt;
            return SoftBodyInspection{Slot->Shared->mVertices.size(),
                Slot->Shared->mEdgeConstraints.size() + Slot->Shared->mVolumeConstraints.size(),
                Slot->Definition.Attachments.size(), Slot->TopologyRevision, Slot->VertexRevision,
                Slot->Definition.Enabled, Slot->Definition.Enabled && Slot->EverSimulated};
        }
        SoftBodyStepResult Step(const SoftBodyStepConfig &Config) override {
            SoftBodyStepResult Result;
            if (!IsValid() || !std::isfinite(Config.DeltaTime) || Config.DeltaTime <= 0 ||
                !Finite(Config.Gravity)) return Result;
            // WorldRoot calls rigid Step first. The shared Jolt world has already
            // advanced soft and rigid contacts together; this only publishes states.
            for (std::uint32_t Index = 1; Index < Slots.size(); ++Index) {
                auto &Slot = Slots[Index];
                if (!Slot.Live) continue;
                const bool Simulated = Slot.Definition.Enabled && Owner.StepSequence > Slot.ExtractedSequence;
                if (Simulated) ++Slot.VertexRevision;
                if (Simulated) Slot.EverSimulated = true;
                auto State = Extract({Index, Slot.Generation}, Slot, Simulated);
                if (State) Result.States.push_back(std::move(*State));
                Slot.ExtractedSequence = Owner.StepSequence;
                if (Simulated) {
                    ++Result.Profile.SimulatedBodies;
                    Result.Profile.SimulatedVertices += Slot.Shared->mVertices.size();
                }
            }
            return Result;
        }
        bool HasInFlightStep() const override { return false; }
        void Shutdown() override {
            if (Stopped) return;
            Owner.SetBeforeUnifiedStep({});
            for (std::uint32_t Index = 1; Index < Slots.size(); ++Index)
                if (Slots[Index].Live) (void)DestroyBody({Index, Slots[Index].Generation});
            Cached.reset();
            Stopped = true;
        }
        const SoftBodyWorldLimits &GetLimits() const override { return Limits; }
    private:
        struct CachedConstruction {
            SoftBodyDefinition Definition;
            SoftConstruction Construction;
        };
        struct PreparedConstruction {
            SoftConstruction Construction;
            bool CacheHit = false;
            std::unique_ptr<CachedConstruction> Candidate;
        };
        static bool SameCachedDefinition(const SoftBodyDefinition &A,
                                         const SoftBodyDefinition &B) {
            const auto SameFloat = [](float X, float Y) {
                return std::bit_cast<std::uint32_t>(X) == std::bit_cast<std::uint32_t>(Y);
            };
            const auto SameVector = [&](glm::vec3 X, glm::vec3 Y) {
                return SameFloat(X.x, Y.x) && SameFloat(X.y, Y.y) && SameFloat(X.z, Y.z);
            };
            const auto &M = A.Material, &N = B.Material;
            if (A.Kind != B.Kind || !SameVector(A.Position, B.Position) ||
                !SameVector(A.Size, B.Size) || A.ResolutionX != B.ResolutionX ||
                A.ResolutionY != B.ResolutionY || A.ResolutionZ != B.ResolutionZ ||
                A.Quality != B.Quality || A.CollisionMode != B.CollisionMode ||
                A.Enabled != B.Enabled || !SameFloat(M.ParticleMass, N.ParticleMass) ||
                !SameFloat(M.Damping, N.Damping) ||
                !SameFloat(M.StretchCompliance, N.StretchCompliance) ||
                !SameFloat(M.BendCompliance, N.BendCompliance) ||
                !SameFloat(M.ShapeCompliance, N.ShapeCompliance) ||
                !SameFloat(M.VolumeCompliance, N.VolumeCompliance) ||
                !SameFloat(M.Friction, N.Friction) ||
                !SameFloat(M.Thickness, N.Thickness) ||
                A.Attachments.size() != B.Attachments.size()) return false;
            for (std::size_t Index = 0; Index < A.Attachments.size(); ++Index)
                if (A.Attachments[Index].Vertex != B.Attachments[Index].Vertex ||
                    !SameVector(A.Attachments[Index].Position, B.Attachments[Index].Position))
                    return false;
            return true;
        }
        static bool CacheEligible(const SoftBodyDefinition &Definition,
                                  const SoftConstruction &Construction) {
            const auto *Shared = Construction.Shared.GetPtr();
            return Shared->mVertices.size() <= 16'384 &&
                Shared->mEdgeConstraints.size() + Shared->mVolumeConstraints.size() <= 100'000 &&
                Shared->mFaces.size() <= 50'000 && Construction.Indices->size() <= 150'000 &&
                Definition.Attachments.size() <= 256;
        }
#if defined(PHYSICS_EVALUATION_ADMISSION_PROFILE) && defined(PHYSICS_GANTRIA_GROUPING)
        static void RecordGroupStats(const SoftConstruction &Construction) {
            const auto *Shared = Construction.Shared.GetPtr();
            AdmissionProfile.GroupStatsAvailable = true;
            AdmissionProfile.UpdateGroups = Shared->GetNumUpdateGroups();
            AdmissionProfile.NonParallelEdges = Shared->GetNumNonParallelEdgeConstraints();
            AdmissionProfile.NonParallelVolumes = Shared->GetNumNonParallelVolumeConstraints();
            AdmissionProfile.ParallelEdges = Shared->mEdgeConstraints.size() -
                AdmissionProfile.NonParallelEdges;
            AdmissionProfile.ParallelVolumes = Shared->mVolumeConstraints.size() -
                AdmissionProfile.NonParallelVolumes;
        }
#endif
        std::optional<PreparedConstruction> PrepareConstruction(const SoftBodyDefinition &Definition) {
            if (Cached && SameCachedDefinition(Cached->Definition, Definition)) {
#if defined(PHYSICS_EVALUATION_ADMISSION_PROFILE) && defined(PHYSICS_GANTRIA_GROUPING)
                RecordGroupStats(Cached->Construction);
#endif
                return PreparedConstruction{Cached->Construction, true, nullptr};
            }
            auto Built = BuildSoft(Definition, Limits);
            if (!Built) return std::nullopt;
#if defined(PHYSICS_EVALUATION_ADMISSION_PROFILE) && defined(PHYSICS_GANTRIA_GROUPING)
            RecordGroupStats(*Built);
#endif
            PreparedConstruction Prepared{std::move(*Built), false, nullptr};
            if (CacheEligible(Definition, Prepared.Construction))
                Prepared.Candidate = std::make_unique<CachedConstruction>(
                    CachedConstruction{Definition, Prepared.Construction});
            return Prepared;
        }
        static bool SameExceptAttachments(const SoftBodyDefinition &A, const SoftBodyDefinition &B) {
            const auto SameVector = [](glm::vec3 X, glm::vec3 Y) {
                return X.x == Y.x && X.y == Y.y && X.z == Y.z;
            };
            const auto &M = A.Material, &N = B.Material;
            return A.Kind == B.Kind && SameVector(A.Position, B.Position) &&
                SameVector(A.Size, B.Size) && A.ResolutionX == B.ResolutionX &&
                A.ResolutionY == B.ResolutionY && A.ResolutionZ == B.ResolutionZ &&
                A.Quality == B.Quality && A.CollisionMode == B.CollisionMode && A.Enabled == B.Enabled &&
                M.ParticleMass == N.ParticleMass && M.Damping == N.Damping &&
                M.StretchCompliance == N.StretchCompliance && M.BendCompliance == N.BendCompliance &&
                M.ShapeCompliance == N.ShapeCompliance && M.VolumeCompliance == N.VolumeCompliance &&
                M.Friction == N.Friction && M.Thickness == N.Thickness;
        }
        struct Slot {
            std::uint32_t Generation = 1;
            bool Live = false;
            bool Retired = false;
            JPH::BodyID Native;
            SoftBodyDefinition Definition;
            JPH::Ref<JPH::SoftBodySharedSettings> Shared;
            std::shared_ptr<std::vector<std::uint32_t>> Indices;
            std::uint64_t TopologyRevision = 0;
            std::uint64_t VertexRevision = 0;
            std::uint64_t ExtractedSequence = 0;
            bool EverSimulated = false;
            std::size_t ConstraintCount = 0;
            glm::vec3 PendingForce{0.0f};
        };
        Slot *Find(SoftBodyId Id) {
            return Id.IsValid() && Id.Slot < Slots.size() && Slots[Id.Slot].Live &&
                Slots[Id.Slot].Generation == Id.Generation ? &Slots[Id.Slot] : nullptr;
        }
        const Slot *Find(SoftBodyId Id) const {
            return Id.IsValid() && Id.Slot < Slots.size() && Slots[Id.Slot].Live &&
                Slots[Id.Slot].Generation == Id.Generation ? &Slots[Id.Slot] : nullptr;
        }
        JPH::ObjectLayer SoftLayer(const SoftBodyDefinition &Definition) {
            PhysicsBodyDesc Description;
            Description.CanCollide = Definition.CollisionMode == SoftBodyCollisionMode::RigidPrimitives &&
                Definition.Enabled;
            Description.CanTouch = false;
            return Owner.Layers.GetLayer(Description, true);
        }
        std::optional<SoftBodyState> Extract(SoftBodyId Id, const Slot &Record, bool Simulated) const {
            JPH::BodyLockRead Lock(Owner.World.GetBodyLockInterface(), Record.Native);
            if (!Lock.Succeeded()) return std::nullopt;
            const auto *Motion = static_cast<const JPH::SoftBodyMotionProperties *>(
                Lock.GetBody().GetMotionProperties());
            if (!Motion) return std::nullopt;
            const auto Origin = FromRVec(Lock.GetBody().GetPosition());
            auto Positions = std::make_shared<std::vector<glm::vec3>>();
            Positions->reserve(Motion->GetVertices().size());
            for (const auto &Vertex : Motion->GetVertices())
                Positions->push_back(Origin + FromJolt(Vertex.mPosition));
            return SoftBodyState{Id, Record.TopologyRevision, Record.VertexRevision,
                Simulated, std::move(Positions), Record.Indices};
        }
        PhysicsOperationResult AddVelocity(SoftBodyId Id, glm::vec3 Impulse,
                                           std::optional<glm::vec3> Point) {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            JPH::BodyLockWrite Lock(Owner.World.GetBodyLockInterface(), Slot->Native);
            if (!Lock.Succeeded()) return {PhysicsOperationStatus::BackendFailure, "Jolt soft body lock failed"};
            auto *Motion = static_cast<JPH::SoftBodyMotionProperties *>(Lock.GetBody().GetMotionProperties());
            auto &Vertices = Motion->GetVertices();
            std::size_t Count = 0;
            std::array<std::size_t, 4> Closest{};
            std::array<double, 4> Distances;
            Distances.fill(std::numeric_limits<double>::infinity());
            const auto Origin = Lock.GetBody().GetPosition();
            for (std::size_t Index = 0; Index < Vertices.size(); ++Index) {
                if (Vertices[Index].mInvMass <= 0) continue;
                ++Count;
                if (Point) {
                    const auto Position = Vertices[Index].mPosition;
                    const double X = double(Point->x) - (Origin.GetX() + Position.GetX());
                    const double Y = double(Point->y) - (Origin.GetY() + Position.GetY());
                    const double Z = double(Point->z) - (Origin.GetZ() + Position.GetZ());
                    const double Distance = X * X + Y * Y + Z * Z;
                    for (std::size_t Candidate = 0; Candidate < Closest.size(); ++Candidate) {
                        if (Distance >= Distances[Candidate]) continue;
                        for (std::size_t Shift = Closest.size() - 1; Shift > Candidate; --Shift) {
                            Distances[Shift] = Distances[Shift - 1];
                            Closest[Shift] = Closest[Shift - 1];
                        }
                        Distances[Candidate] = Distance;
                        Closest[Candidate] = Index;
                        break;
                    }
                }
            }
            if (Count == 0) return {};
            std::array<double, 4> Weights{};
            double TotalWeight = 0;
            if (Point) {
                for (std::size_t Index = 0; Index < Closest.size(); ++Index) {
                    if (!std::isfinite(Distances[Index])) continue;
                    Weights[Index] = 1.0 / std::max(std::sqrt(Distances[Index]), 0.001);
                    TotalWeight += Weights[Index];
                }
                if (!std::isfinite(TotalWeight) || TotalWeight <= 0)
                    return {PhysicsOperationStatus::InvalidDescription,
                        "Soft point impulse has invalid finite weights"};
            }
            std::vector<std::pair<std::size_t, glm::vec3>> Pending;
            Pending.reserve(Point ? std::min<std::size_t>(4, Count) : Count);
            for (std::size_t Index = 0; Index < Vertices.size(); ++Index) {
                auto &Vertex = Vertices[Index];
                if (Vertex.mInvMass <= 0) continue;
                float Share = 1.0f / float(Count);
                if (Point) {
                    const auto Found = std::find(Closest.begin(), Closest.end(), Index);
                    if (Found == Closest.end()) continue;
                    const auto Position = std::size_t(Found - Closest.begin());
                    if (Weights[Position] == 0) continue;
                    Share = float(Weights[Position] / TotalWeight);
                }
                const auto Velocity = FromJolt(Vertex.mVelocity) +
                    Impulse * (Vertex.mInvMass * Share);
                if (!Finite(Velocity))
                    return {PhysicsOperationStatus::InvalidDescription, "Soft impulse exceeds finite velocity range"};
                Pending.emplace_back(Index, Velocity);
            }
            for (const auto &[Index, Velocity] : Pending)
                Vertices[Index].mVelocity = ToJolt(Velocity);
            return {};
        }
        RigidJoltBackend &Owner;
        SoftBodyWorldLimits Limits;
        std::vector<Slot> Slots;
        std::size_t LiveBodies = 0;
        std::size_t LiveVertices = 0;
        std::size_t LiveConstraints = 0;
        // A single successfully admitted, exact-definition topology survives the
        // last body until replacement or Shutdown. It never owns native body state.
        std::unique_ptr<CachedConstruction> Cached;
        bool Stopped = false;
    };
}
// The admission executable resets this before each isolated body construction.
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
void ResetJoltAdmissionProfile() { AdmissionProfile = {}; }
JoltAdmissionProfile GetJoltAdmissionProfile() { return AdmissionProfile; }
void ResetJoltWorldAdmissionProfile() { WorldAdmissionProfile = {}; }
JoltWorldAdmissionProfile GetJoltWorldAdmissionProfile() { return WorldAdmissionProfile; }
#endif

std::unique_ptr<gargantuan::IPhysicsBackend> CreateRigidJoltBackend(
    const gargantuan::PhysicsWorldConfig &Config, int WorkerBudget) {
    return std::make_unique<RigidJoltBackend>(Config, WorkerBudget);
}

UnifiedJoltProvider CreateUnifiedJoltProvider(const gargantuan::PhysicsWorldConfig &Config,
                                               gargantuan::SoftBodyWorldLimits Limits,
                                               int WorkerBudget) {
#ifndef PHYSICS_GANTRIA_JOLT
    (void)Config;
    (void)Limits;
    (void)WorkerBudget;
    return {};
#else
    auto Rigid = std::make_unique<RigidJoltBackend>(Config, WorkerBudget, true);
    if (!Rigid->IsValid()) return {};
    auto Soft = std::make_unique<JoltSoftBackend>(*Rigid, Limits);
    auto *SoftPointer = Soft.get();
    Rigid->SetBeforeUnifiedStep([SoftPointer](float DeltaTime) {
        return SoftPointer->FlushForces(DeltaTime);
    });
    return {std::move(Rigid), std::move(Soft)};
#endif
}
JoltSoftSolverProfile GetJoltSoftSolverProfile() {
    return {.WorldUpdates = 2, .CollisionStepsPerUpdate = 8u / unsigned(PhysicsEvaluation::ContactProbeParts()),
        .SoftIterationsPerCollisionStep = SoftIterationsPerCollisionStep,
        .ConstraintSweepsPerIteration = PhysicsEvaluation::ContactProbeSweeps(ConstraintSweepsPerIteration),
        .SoftIntegrationsPerFrame = SoftIntegrationsPerFrame / unsigned(PhysicsEvaluation::ContactProbeParts()),
#ifdef PHYSICS_GANTRIA_SLEEP_FIX
        .FullUpdateSleepClock = true,
#else
        .FullUpdateSleepClock = false,
#endif
#ifdef PHYSICS_GANTRIA_GROUPING
        .LargeClothGroupBatch = PHYSICS_GANTRIA_LARGE_CLOTH_GROUP_BATCH};
#else
        .LargeClothGroupBatch = 256};
#endif
}
std::optional<JoltStepSchedule> GetJoltStepSchedule(
    const gargantuan::IPhysicsBackend &Rigid) {
    const auto *Jolt = dynamic_cast<const RigidJoltBackend *>(&Rigid);
    return Jolt ? Jolt->GetStepSchedule() : std::nullopt;
}
bool SetJoltRigidFastScheduleEnabled(gargantuan::IPhysicsBackend &Rigid, bool Enabled) {
    auto *Jolt = dynamic_cast<RigidJoltBackend *>(&Rigid);
    if (!Jolt) return false;
    Jolt->SetRigidFastScheduleEnabled(Enabled);
    return true;
}
std::optional<std::size_t> GetJoltNativeRigidContactPairCount(
    const gargantuan::IPhysicsBackend &Rigid) {
    const auto *Jolt = dynamic_cast<const RigidJoltBackend *>(&Rigid);
    return Jolt ? std::optional<std::size_t>(Jolt->NativeRigidContactPairCount()) : std::nullopt;
}
bool FailNextJoltTouchUpdateAfterNativeMutation(gargantuan::IPhysicsBackend &Rigid) {
    auto *Jolt = dynamic_cast<RigidJoltBackend *>(&Rigid);
    if (!Jolt) return false;
    Jolt->FailNextTouchUpdateAfterNativeMutation();
    return true;
}
std::uint64_t GetJoltSoftRigidContactVertexCount(
    const gargantuan::IPhysicsBackend &Rigid, const gargantuan::ISoftBodyBackend &Soft,
    gargantuan::SoftBodyId SoftId, gargantuan::PhysicsBodyId RigidId) {
    const auto *RigidValue = dynamic_cast<const RigidJoltBackend *>(&Rigid);
    const auto *SoftValue = dynamic_cast<const JoltSoftBackend *>(&Soft);
    if (!RigidValue || !SoftValue || &SoftValue->SharedWorld() != RigidValue) return 0;
    return RigidValue->SoftRigidContactVertices(SoftValue->NativeBody(SoftId), RigidId);
}
bool SetJoltSoftRigidContactCapture(gargantuan::IPhysicsBackend &Rigid, bool Enabled) {
    auto *RigidValue = dynamic_cast<RigidJoltBackend *>(&Rigid);
    if (!RigidValue) return false;
    RigidValue->SetSoftContactCapture(Enabled);
    return true;
}
#ifdef PHYSICS_GANTRIA_ATTACHMENTS
gargantuan::PhysicsOperationResult ConnectJoltRigidSoftAttachment(
    gargantuan::IPhysicsBackend &Rigid, gargantuan::ISoftBodyBackend &Soft,
    gargantuan::SoftBodyId SoftId, std::uint32_t Vertex,
    gargantuan::PhysicsBodyId RigidId, glm::vec3 RigidLocalPoint) {
    auto *RigidValue = dynamic_cast<RigidJoltBackend *>(&Rigid);
    auto *SoftValue = dynamic_cast<JoltSoftBackend *>(&Soft);
    if (!RigidValue || !SoftValue || &SoftValue->SharedWorld() != RigidValue)
        return {gargantuan::PhysicsOperationStatus::BackendFailure,
            "Jolt attachment endpoints do not share one provider world"};
    return RigidValue->ConnectAttachment(SoftId, SoftValue->NativeBody(SoftId),
        Vertex, RigidId, RigidLocalPoint);
}
gargantuan::PhysicsOperationResult DisconnectJoltRigidSoftAttachment(
    gargantuan::IPhysicsBackend &Rigid, gargantuan::SoftBodyId SoftId,
    gargantuan::PhysicsBodyId RigidId) {
    auto *RigidValue = dynamic_cast<RigidJoltBackend *>(&Rigid);
    if (!RigidValue)
        return {gargantuan::PhysicsOperationStatus::BackendFailure,
            "Rigid endpoint is not the Jolt provider"};
    return RigidValue->DisconnectAttachment(SoftId, RigidId);
}
std::optional<JoltRigidSoftAttachmentState> GetJoltRigidSoftAttachmentState(
    const gargantuan::IPhysicsBackend &Rigid, gargantuan::SoftBodyId SoftId,
    gargantuan::PhysicsBodyId RigidId) {
    const auto *RigidValue = dynamic_cast<const RigidJoltBackend *>(&Rigid);
    return RigidValue ? RigidValue->AttachmentState(SoftId, RigidId) : std::nullopt;
}
#endif
}
