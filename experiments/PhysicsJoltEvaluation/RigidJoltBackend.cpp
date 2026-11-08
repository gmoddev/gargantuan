#include "RigidJoltBackend.hpp"
#include "Provider/UnifiedJoltProvider.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
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
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace PhysicsEvaluation {
namespace {
    using namespace gargantuan;

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
    class JoltRuntime final {
    public:
        JoltRuntime() : OwnedFactory(std::make_unique<JPH::Factory>()) {
            JPH::Factory::sInstance = OwnedFactory.get();
            JPH::RegisterTypes();
        }
        ~JoltRuntime() {
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
                PhysicsCanCollide(First.Group, First.Mask, Second.Group, Second.Mask);
        }
        bool ShouldCollide(JPH::ObjectLayer A, JPH::BroadPhaseLayer B) const override {
            return A < Policies.size() && Policies[A].Participates &&
                !(Policies[A].Static && B.GetValue() == 0);
        }
        JPH::ObjectLayer GetLayer(const PhysicsBodyDesc &Description) {
            const LayerPolicy Candidate{Description.CollisionGroup, Description.CollisionMask,
                Description.Anchored, Description.CanCollide || Description.CanTouch};
            for (std::size_t Index = 0; Index < Policies.size(); ++Index) {
                const auto &Existing = Policies[Index];
                if (Existing.Group == Candidate.Group && Existing.Mask == Candidate.Mask &&
                    Existing.Static == Candidate.Static && Existing.Participates == Candidate.Participates)
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
        void BeginStep() {
            std::scoped_lock Lock(Mutex);
            Observed.clear();
        }
        void OnContactAdded(const JPH::Body &A, const JPH::Body &B,
                            const JPH::ContactManifold &, JPH::ContactSettings &) override {
            Record(A, B);
        }
        void OnContactPersisted(const JPH::Body &A, const JPH::Body &B,
                                const JPH::ContactManifold &, JPH::ContactSettings &) override {
            Record(A, B);
        }
        std::set<std::pair<std::uint32_t, std::uint32_t>> TakeObserved() {
            std::scoped_lock Lock(Mutex);
            return std::move(Observed);
        }
    private:
        void Record(const JPH::Body &A, const JPH::Body &B) {
            const auto First = A.GetID().GetIndexAndSequenceNumber();
            const auto Second = B.GetID().GetIndexAndSequenceNumber();
            std::scoped_lock Lock(Mutex);
            Observed.emplace(std::min(First, Second), std::max(First, Second));
        }
        std::mutex Mutex;
        std::set<std::pair<std::uint32_t, std::uint32_t>> Observed;
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

    class RigidJoltBackend final : public IPhysicsBackend {
    public:
        friend class JoltSoftBackend;
        RigidJoltBackend(const PhysicsWorldConfig &Config, int WorkerBudget, bool Unified = false)
            : Configuration(Config), Temp(32 * 1024 * 1024), UnifiedMode(Unified) {
            static JoltRuntime Runtime;
            (void)Runtime;
            if (!Finite(Config.Gravity) || WorkerBudget < 1 || WorkerBudget > 8) return;
            if (WorkerBudget == 1) Jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
            else Jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs,
                JPH::cMaxPhysicsBarriers, WorkerBudget - 1);
            World.Init(65536, 0, 65536, 65536, Layers, Layers, Layers);
            World.SetGravity(ToJolt(Config.Gravity));
            World.SetContactListener(&Contacts);
            BodySlots.emplace_back();
            ConstraintSlots.emplace_back();
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
        bool IsBodyValid(PhysicsBodyId Id) const override { return Find(Id) != nullptr; }
        bool IsConstraintValid(PhysicsConstraintId Id) const override { return Find(Id) != nullptr; }

        PhysicsBodyId CreateBody(const PhysicsBodyDesc &Description) override {
            if (!Valid || !ValidBody(Description)) return {};
            const auto Layer = Layers.GetLayer(Description);
            if (Layer == JPH::cObjectLayerInvalid) return {};
            auto Shape = MakeShape(Description.Shape);
            if (!Shape) return {};
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
            const auto Native = World.GetBodyInterface().CreateAndAddBody(Settings,
                Motion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
            if (Native.IsInvalid()) return {};
            std::uint32_t Index = 1;
            for (; Index < BodySlots.size() && (BodySlots[Index].Live || BodySlots[Index].Retired); ++Index) {}
            if (Index == BodySlots.size()) BodySlots.emplace_back();
            auto &Slot = BodySlots[Index];
            Slot.Live = true;
            Slot.Native = Native;
            Slot.Shape = Shape;
            Slot.Description = Description;
            PhysicsBodyId Id{Index, Slot.Generation};
            NativeMap.emplace(Native.GetIndexAndSequenceNumber(), Id);
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
                Description.Shape.Mesh != Slot->Description.Shape.Mesh;
            JPH::ShapeRefC NewShape;
            if (ShapeChanged) {
                NewShape = MakeShape(Description.Shape);
                if (!NewShape) return {PhysicsOperationStatus::InvalidDescription, "Invalid replacement shape"};
            }
            const auto Layer = Layers.GetLayer(Description);
            if (Layer == JPH::cObjectLayerInvalid) return {PhysicsOperationStatus::ResourceLimit, "Jolt layer limit"};
            auto &Interface = World.GetBodyInterface();
            if (ShapeChanged)
                Interface.SetShape(Slot->Native, NewShape.GetPtr(), true, JPH::EActivation::Activate);
            const auto Motion = Description.Anchored ? JPH::EMotionType::Static
                : Description.Kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic;
            const bool MotionChanged = Description.Anchored != Slot->Description.Anchored ||
                Description.Kinematic != Slot->Description.Kinematic;
            if (MotionChanged)
                Interface.SetMotionType(Slot->Native, Motion, JPH::EActivation::Activate);
            if (!SameTransform(Description.Transform, Slot->Description.Transform))
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
            if (ShapeChanged) Slot->Shape = NewShape;
            Slot->Description = Description;
            return {};
        }

        PhysicsOperationResult DestroyBody(PhysicsBodyId Id) override {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            for (std::uint32_t Index = 1; Index < ConstraintSlots.size(); ++Index) {
                const auto &Constraint = ConstraintSlots[Index];
                if (Constraint.Live && (Constraint.Description.BodyA == Id || Constraint.Description.BodyB == Id))
                    (void)DestroyConstraint({Index, Constraint.Generation});
            }
            auto &Interface = World.GetBodyInterface();
            Interface.RemoveBody(Slot->Native);
            Interface.DestroyBody(Slot->Native);
            NativeMap.erase(Slot->Native.GetIndexAndSequenceNumber());
            for (auto It = PreviousContacts.begin(); It != PreviousContacts.end();) {
                if (It->first == Slot->Native.GetIndexAndSequenceNumber() ||
                    It->second == Slot->Native.GetIndexAndSequenceNumber()) It = PreviousContacts.erase(It);
                else ++It;
            }
            Slot->Shape = nullptr;
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
            if (!Finite(Description.LocalFrameA) || !Finite(Description.LocalFrameB) ||
                !std::isfinite(Description.LowerLimit) || !std::isfinite(Description.UpperLimit) ||
                Description.LowerLimit > Description.UpperLimit)
                return {{}, {PhysicsOperationStatus::InvalidDescription, "Invalid Jolt joint frame or limit"}};
            const auto PointA = ToRVec(Description.LocalFrameA.Position);
            const auto PointB = ToRVec(Description.LocalFrameB.Position);
            const auto AxisA = ToJolt(Description.LocalFrameA.Rotation[Description.Kind == PhysicsConstraintKind::Slider ? 0 : 2]);
            const auto AxisB = ToJolt(Description.LocalFrameB.Rotation[Description.Kind == PhysicsConstraintKind::Slider ? 0 : 2]);
            JPH::BodyLockWrite LockA(World.GetBodyLockInterface(), A->Native);
            JPH::BodyLockWrite LockB(World.GetBodyLockInterface(), B->Native);
            if (!LockA.Succeeded() || !LockB.Succeeded())
                return {{}, {PhysicsOperationStatus::BackendFailure, "Jolt joint body lock failed"}};
            JPH::Ref<JPH::Constraint> Native;
            if (Description.Kind == PhysicsConstraintKind::BallSocket) {
                JPH::PointConstraintSettings Settings;
                Settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
                Settings.mPoint1 = PointA;
                Settings.mPoint2 = PointB;
                Native = Settings.Create(LockA.GetBody(), LockB.GetBody());
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
                auto *Joint = static_cast<JPH::HingeConstraint *>(Settings.Create(LockA.GetBody(), LockB.GetBody()));
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
                auto *Joint = static_cast<JPH::SliderConstraint *>(Settings.Create(LockA.GetBody(), LockB.GetBody()));
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
                Native = Settings.Create(LockA.GetBody(), LockB.GetBody());
            } else return {{}, Unsupported("Experimental Jolt weld joint is unsupported")};
            if (!Native) return {{}, Unsupported("Jolt constraint creation failed")};
            World.AddConstraint(Native.GetPtr());
            std::uint32_t Index = 1;
            for (; Index < ConstraintSlots.size() &&
                (ConstraintSlots[Index].Live || ConstraintSlots[Index].Retired); ++Index) {}
            if (Index == ConstraintSlots.size()) ConstraintSlots.emplace_back();
            auto &Slot = ConstraintSlots[Index];
            Slot.Live = true; Slot.Native = Native; Slot.Description = Description;
            ++ActiveConstraints;
            return {{Index, Slot.Generation}, {}};
        }
        PhysicsOperationResult DestroyConstraint(PhysicsConstraintId Id) override {
            auto *Slot = Find(Id);
            if (!Slot) return {PhysicsOperationStatus::InvalidId, "Invalid Jolt joint ID"};
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
            World.GetBodyInterface().AddForce(Slot->Native, ToJolt(Force));
            return {};
        }
        PhysicsOperationResult ApplyTorque(PhysicsBodyId Id, glm::vec3 Torque) override {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            if (!Finite(Torque) || Slot->Description.Anchored || Slot->Description.Kinematic)
                return {PhysicsOperationStatus::InvalidDescription, "Torque requires a finite dynamic body"};
            World.GetBodyInterface().AddTorque(Slot->Native, ToJolt(Torque));
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
                glm::length(Request.Direction) > MaximumRaycastDistance)
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
            if (!Finite(Request.Shape.Transform))
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
            Contacts.BeginStep();
            const auto UpdateWorld = [&](float DeltaTime, int CollisionSteps) {
                const auto Update = World.Update(DeltaTime, CollisionSteps, &Temp, Jobs.get());
                if (Update != JPH::EPhysicsUpdateError::None)
                    throw std::runtime_error("[Physics:JoltRigid] Jolt Update failed with code " +
                        std::to_string(static_cast<unsigned>(Update)));
            };
            if (UnifiedMode) {
                UpdateWorld(Config.DeltaTime * 0.5f, 8);
                UpdateWorld(Config.DeltaTime * 0.5f, 8);
            } else UpdateWorld(Config.DeltaTime, Config.SubStepCount);
            ++StepSequence;
            for (std::uint32_t Index = 1; Index < BodySlots.size(); ++Index) {
                const auto &Slot = BodySlots[Index];
                if (!Slot.Live || Slot.Description.Anchored) continue;
                JPH::RVec3 Position; JPH::Quat Rotation;
                World.GetBodyInterface().GetPositionAndRotation(Slot.Native, Position, Rotation);
                Result.Motions.push_back({{Index, Slot.Generation}, FromJolt(Position, Rotation)});
            }
            const auto CurrentContacts = Contacts.TakeObserved();
            const auto Emit = [&](const auto &Pair, PhysicsContactPhase Phase) {
                const auto A = NativeMap.find(Pair.first);
                const auto B = NativeMap.find(Pair.second);
                if (A == NativeMap.end() || B == NativeMap.end()) return;
                if (Result.Contacts.size() == MaximumPhysicsEventsPerStep) {
                    Result.EventsTruncated = true;
                    return;
                }
                auto First = A->second;
                auto Second = B->second;
                if (Second < First) std::swap(First, Second);
                Result.Contacts.push_back({First, Second, Phase});
            };
            for (const auto &Pair : CurrentContacts)
                if (!PreviousContacts.contains(Pair)) Emit(Pair, PhysicsContactPhase::Began);
            for (const auto &Pair : PreviousContacts)
                if (!CurrentContacts.contains(Pair)) Emit(Pair, PhysicsContactPhase::Ended);
            PreviousContacts = CurrentContacts;
            std::ranges::sort(Result.Contacts, [](const auto &A, const auto &B) {
                return std::tie(A.BodyA, A.BodyB, A.Phase) < std::tie(B.BodyA, B.BodyB, B.Phase);
            });
            return Result;
        }

    private:
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
            if (!Value.Anchored && !Value.Kinematic && !std::isfinite(BodyMass(Value))) return false;
            if (Value.Shape.Kind == PhysicsShapeKind::StaticTriangleMesh)
                return Value.Anchored && Value.Shape.Mesh && Value.Shape.Mesh->Positions && Value.Shape.Mesh->Indices;
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
                Query.Size.x > 0 && Query.Size.y > 0 && Query.Size.z > 0)
                return new JPH::BoxShape(ToJolt(Query.Size * 0.5f), 0.0f);
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
        JPH::TempAllocatorImpl Temp;
        std::unique_ptr<JPH::JobSystem> Jobs;
        // Destruct World before Jobs, Temp and Layers, whose storage it may use.
        JPH::PhysicsSystem World;
        std::vector<BodySlot> BodySlots;
        std::vector<ConstraintSlot> ConstraintSlots;
        std::unordered_map<std::uint32_t, PhysicsBodyId> NativeMap;
        std::set<std::pair<std::uint32_t, std::uint32_t>> PreviousContacts;
        mutable std::map<std::tuple<int, float, float, float, float, float>, JPH::ShapeRefC> QueryShapes;
        std::size_t ActiveConstraints = 0;
        std::uint64_t StepSequence = 0;
        bool Valid = false;
        bool UnifiedMode = false;
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
            !std::isfinite(Material.ParticleMass) || Material.ParticleMass <= 0 ||
            !FiniteNonnegative(Material.Damping) || !FiniteNonnegative(Material.StretchCompliance) ||
            !FiniteNonnegative(Material.BendCompliance) || !FiniteNonnegative(Material.ShapeCompliance) ||
            !FiniteNonnegative(Material.VolumeCompliance) || !FiniteNonnegative(Material.Friction) ||
            !FiniteNonnegative(Material.Thickness) || Definition.ResolutionX < 2 ||
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
        SoftConstruction Result{new JPH::SoftBodySharedSettings,
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
                        JPH::Float3(0, 0, 0), 1.0f / Material.ParticleMass);
                }
        std::set<std::uint32_t> Pinned;
        for (const auto &Attachment : Definition.Attachments) {
            if (Attachment.Vertex >= X * Y * Z || !Finite(Attachment.Position) ||
                !Pinned.insert(Attachment.Vertex).second) return std::nullopt;
            const auto Local = Attachment.Position - Definition.Position;
            Result.Shared->mVertices[Attachment.Vertex].mPosition = JPH::Float3(Local.x, Local.y, Local.z);
            Result.Shared->mVertices[Attachment.Vertex].mInvMass = 0.0f;
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
        const auto ConstraintCount = Result.Shared->mEdgeConstraints.size() +
            Result.Shared->mVolumeConstraints.size();
        if (ConstraintCount > Limits.MaximumConstraints) return std::nullopt;
        Result.Shared->CalculateEdgeLengths();
        Result.Shared->CalculateVolumeConstraintVolumes();
        Result.Shared->Optimize();
        return Result;
    }

    class JoltSoftBackend final : public ISoftBodyBackend {
    public:
        JoltSoftBackend(RigidJoltBackend &Owner, SoftBodyWorldLimits Limits)
            : Owner(Owner), Limits(Limits) { Slots.emplace_back(); }
        ~JoltSoftBackend() override { Shutdown(); }
        bool IsValid() const override { return !Stopped && Owner.IsValid(); }
        bool IsBodyValid(SoftBodyId Id) const override { return Find(Id) != nullptr; }
        SoftBodyId CreateBody(const SoftBodyDefinition &Definition) override {
            if (!IsValid() || LiveBodies >= Limits.MaximumBodies) return {};
            auto Construction = BuildSoft(Definition, Limits);
            if (!Construction) return {};
            const auto NewVertices = Construction->Shared->mVertices.size();
            const auto NewConstraints = Construction->Shared->mEdgeConstraints.size() +
                Construction->Shared->mVolumeConstraints.size();
            if (NewVertices > Limits.MaximumWorldVertices - LiveVertices ||
                NewConstraints > Limits.MaximumConstraints - LiveConstraints ||
                NewVertices > Limits.MaximumQueuedResultBytes / sizeof(glm::vec3))
                return {};
            const auto Layer = SoftLayer(Definition);
            if (Layer == JPH::cObjectLayerInvalid) return {};
            JPH::SoftBodyCreationSettings Settings(Construction->Shared,
                ToRVec(Definition.Position), JPH::Quat::sIdentity(), Layer);
            Settings.mNumIterations = 12;
            Settings.mVelocityReconstruction = JPH::ESoftBodyVelocityReconstruction::ConstraintDelta;
            Settings.mAllowSleeping = false;
            Settings.mUpdatePosition = false;
            Settings.mFacesDoubleSided = true;
            Settings.mLinearDamping = Definition.Material.Damping;
            Settings.mFriction = Definition.Material.Friction;
            Settings.mVertexRadius = Definition.Material.Thickness;
            const auto Native = Owner.World.GetBodyInterface().CreateAndAddSoftBody(Settings,
                Definition.Enabled ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
            if (Native.IsInvalid()) return {};
            std::uint32_t Index = 1;
            for (; Index < Slots.size() && (Slots[Index].Live || Slots[Index].Retired); ++Index) {}
            if (Index == Slots.size()) Slots.emplace_back();
            auto &Slot = Slots[Index];
            Slot.Live = true;
            Slot.Native = Native;
            Slot.Definition = Definition;
            Slot.Shared = std::move(Construction->Shared);
            Slot.Indices = std::move(Construction->Indices);
            Slot.ConstraintCount = NewConstraints;
            Slot.TopologyRevision = std::max<std::uint64_t>(1, Slot.TopologyRevision + 1);
            Slot.VertexRevision = std::max<std::uint64_t>(1, Slot.VertexRevision + 1);
            ++LiveBodies;
            LiveVertices += NewVertices;
            LiveConstraints += NewConstraints;
            return {Index, Slot.Generation};
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
                Slot->Definition.Attachments = Definition.Attachments;
                ++Slot->VertexRevision;
                return {};
            }
            auto Replacement = BuildSoft(Definition, Limits);
            if (!Replacement) return {PhysicsOperationStatus::InvalidDescription, "Invalid Jolt soft definition"};
            const auto NewVertices = Replacement->Shared->mVertices.size();
            const auto NewConstraints = Replacement->Shared->mEdgeConstraints.size() +
                Replacement->Shared->mVolumeConstraints.size();
            if (NewVertices > Limits.MaximumWorldVertices - (LiveVertices - Slot->Shared->mVertices.size()) ||
                NewConstraints > Limits.MaximumConstraints - (LiveConstraints - Slot->ConstraintCount) ||
                NewVertices > Limits.MaximumQueuedResultBytes / sizeof(glm::vec3))
                return {PhysicsOperationStatus::InvalidDescription, "Invalid or oversized Jolt soft definition"};
            const auto Layer = SoftLayer(Definition);
            if (Layer == JPH::cObjectLayerInvalid)
                return {PhysicsOperationStatus::ResourceLimit, "Jolt soft layer limit"};
            JPH::SoftBodyCreationSettings Settings(Replacement->Shared,
                ToRVec(Definition.Position), JPH::Quat::sIdentity(), Layer);
            Settings.mNumIterations = 12;
            Settings.mVelocityReconstruction = JPH::ESoftBodyVelocityReconstruction::ConstraintDelta;
            Settings.mAllowSleeping = false;
            Settings.mUpdatePosition = false;
            Settings.mFacesDoubleSided = true;
            Settings.mLinearDamping = Definition.Material.Damping;
            Settings.mFriction = Definition.Material.Friction;
            Settings.mVertexRadius = Definition.Material.Thickness;
            const auto Native = Owner.World.GetBodyInterface().CreateAndAddSoftBody(Settings,
                Definition.Enabled ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
            if (Native.IsInvalid()) return {PhysicsOperationStatus::BackendFailure, "Jolt soft replacement failed"};
            auto &Interface = Owner.World.GetBodyInterface();
            Interface.RemoveBody(Slot->Native);
            Interface.DestroyBody(Slot->Native);
            LiveVertices = LiveVertices - Slot->Shared->mVertices.size() + Replacement->Shared->mVertices.size();
            LiveConstraints = LiveConstraints - Slot->ConstraintCount + NewConstraints;
            Slot->Native = Native;
            Slot->Definition = Definition;
            Slot->Shared = std::move(Replacement->Shared);
            Slot->Indices = std::move(Replacement->Indices);
            Slot->ConstraintCount = NewConstraints;
            ++Slot->TopologyRevision;
            ++Slot->VertexRevision;
            return {};
        }
        PhysicsOperationResult DestroyBody(SoftBodyId Id) override {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            auto &Interface = Owner.World.GetBodyInterface();
            Interface.RemoveBody(Slot->Native);
            Interface.DestroyBody(Slot->Native);
            LiveVertices -= Slot->Shared->mVertices.size();
            LiveConstraints -= Slot->ConstraintCount;
            --LiveBodies;
            Slot->Live = false;
            Slot->Shared = nullptr;
            Slot->Indices.reset();
            if (Slot->Generation == std::numeric_limits<std::uint32_t>::max()) Slot->Retired = true;
            else ++Slot->Generation;
            return {};
        }
        PhysicsOperationResult ApplyForce(SoftBodyId Id, glm::vec3 Force) override {
            if (!Finite(Force)) return {PhysicsOperationStatus::InvalidDescription, "Nonfinite soft force"};
            return AddVelocity(Id, Force * SoftBodyStepInterval, std::nullopt);
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
                Slot->Definition.Enabled, Slot->Definition.Enabled && Owner.StepSequence > 0};
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
            for (std::uint32_t Index = 1; Index < Slots.size(); ++Index)
                if (Slots[Index].Live) (void)DestroyBody({Index, Slots[Index].Generation});
            Stopped = true;
        }
        const SoftBodyWorldLimits &GetLimits() const override { return Limits; }
    private:
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
            std::size_t ConstraintCount = 0;
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
            return Owner.Layers.GetLayer(Description);
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
            std::size_t Count = 0, Nearest = 0;
            float BestDistance = std::numeric_limits<float>::infinity();
            for (std::size_t Index = 0; Index < Vertices.size(); ++Index) {
                if (Vertices[Index].mInvMass <= 0) continue;
                ++Count;
                if (Point) {
                    const float Distance = glm::length(*Point -
                        (FromRVec(Lock.GetBody().GetPosition()) + FromJolt(Vertices[Index].mPosition)));
                    if (Distance < BestDistance) { BestDistance = Distance; Nearest = Index; }
                }
            }
            if (Count == 0) return {};
            for (std::size_t Index = 0; Index < Vertices.size(); ++Index) {
                auto &Vertex = Vertices[Index];
                if (Vertex.mInvMass <= 0 || (Point && Index != Nearest)) continue;
                const float Share = Point ? 1.0f : 1.0f / float(Count);
                Vertex.mVelocity += ToJolt(Impulse * (Vertex.mInvMass * Share));
            }
            return {};
        }
        RigidJoltBackend &Owner;
        SoftBodyWorldLimits Limits;
        std::vector<Slot> Slots;
        std::size_t LiveBodies = 0;
        std::size_t LiveVertices = 0;
        std::size_t LiveConstraints = 0;
        bool Stopped = false;
    };
}

std::unique_ptr<gargantuan::IPhysicsBackend> CreateRigidJoltBackend(
    const gargantuan::PhysicsWorldConfig &Config, int WorkerBudget) {
    return std::make_unique<RigidJoltBackend>(Config, WorkerBudget);
}

UnifiedJoltProvider CreateUnifiedJoltProvider(const gargantuan::PhysicsWorldConfig &Config,
                                               gargantuan::SoftBodyWorldLimits Limits,
                                               int WorkerBudget) {
    auto Rigid = std::make_unique<RigidJoltBackend>(Config, WorkerBudget, true);
    if (!Rigid->IsValid()) return {};
    auto Soft = std::make_unique<JoltSoftBackend>(*Rigid, Limits);
    return {std::move(Rigid), std::move(Soft)};
}
}
