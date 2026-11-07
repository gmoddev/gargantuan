#include "RigidJoltBackend.hpp"

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
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
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
        JPH::BodyID Native;
        PhysicsBodyDesc Description;
        JPH::ShapeRefC Shape;
    };
    struct ConstraintSlot {
        std::uint32_t Generation = 1;
        bool Live = false;
        PhysicsConstraintDesc Description;
        JPH::Ref<JPH::Constraint> Native;
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
        RigidJoltBackend(const PhysicsWorldConfig &Config, int WorkerBudget)
            : Configuration(Config), Temp(32 * 1024 * 1024) {
            static const bool Registered = [] {
                JPH::Factory::sInstance = new JPH::Factory;
                JPH::RegisterTypes();
                return true;
            }();
            (void)Registered;
            if (!Finite(Config.Gravity) || WorkerBudget < 1 || WorkerBudget > 8) return;
            if (WorkerBudget == 1) Jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
            else Jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs,
                JPH::cMaxPhysicsBarriers, WorkerBudget - 1);
            World.Init(65536, 0, 65536, 65536, Layers, Layers, Layers);
            World.SetGravity(ToJolt(Config.Gravity));
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
            Settings.mIsSensor = !Description.CanCollide && Description.CanTouch;
            Settings.mMotionQuality = Description.MovingBodyCCD ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
            Settings.mAllowSleeping = Configuration.EnableSleep && Description.SleepEnabled;
            Settings.mLinearDamping = Description.LinearDamping;
            Settings.mAngularDamping = Description.AngularDamping;
            Settings.mGravityFactor = Description.GravityScale;
            if (Motion == JPH::EMotionType::Dynamic) {
                const auto Size = Description.Shape.Size;
                const float Volume = Description.Shape.Kind == PhysicsShapeKind::Ball
                    ? (4.0f / 3.0f) * 3.14159265358979323846f *
                        std::pow(std::min({Size.x, Size.y, Size.z}) * 0.5f, 3)
                    : Size.x * Size.y * Size.z;
                const float Mass = std::max(0.001f, Description.Density * Volume);
                Settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
                Settings.mMassPropertiesOverride = Shape->GetMassProperties();
                Settings.mMassPropertiesOverride.ScaleToMass(Mass);
            }
            const auto Native = World.GetBodyInterface().CreateAndAddBody(Settings,
                Motion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
            if (Native.IsInvalid()) return {};
            std::uint32_t Index = 1;
            for (; Index < BodySlots.size() && BodySlots[Index].Live; ++Index) {}
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
            if (Description.Shape.Kind != Slot->Description.Shape.Kind ||
                Description.Shape.Size.x != Slot->Description.Shape.Size.x ||
                Description.Shape.Size.y != Slot->Description.Shape.Size.y ||
                Description.Shape.Size.z != Slot->Description.Shape.Size.z ||
                Description.Shape.Mesh != Slot->Description.Shape.Mesh ||
                Description.Density != Slot->Description.Density ||
                Description.SleepEnabled != Slot->Description.SleepEnabled)
                return Unsupported("Experimental Jolt shape, density, and sleep replacement are unsupported");
            const auto Layer = Layers.GetLayer(Description);
            if (Layer == JPH::cObjectLayerInvalid) return {PhysicsOperationStatus::ResourceLimit, "Jolt layer limit"};
            auto &Interface = World.GetBodyInterface();
            const auto Motion = Description.Anchored ? JPH::EMotionType::Static
                : Description.Kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic;
            if (Description.Anchored != Slot->Description.Anchored ||
                Description.Kinematic != Slot->Description.Kinematic)
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
            Slot->Shape = nullptr;
            Slot->Live = false;
            ++Slot->Generation;
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
            for (; Index < ConstraintSlots.size() && ConstraintSlots[Index].Live; ++Index) {}
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
            ++Slot->Generation;
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
        PhysicsOperationResult ApplyForce(PhysicsBodyId Id, glm::vec3 Force) override {
            auto *Slot = Find(Id);
            if (!Slot) return InvalidBody();
            if (!Finite(Force) || Slot->Description.Anchored || Slot->Description.Kinematic)
                return {PhysicsOperationStatus::InvalidDescription, "Force requires finite dynamic body"};
            World.GetBodyInterface().AddForce(Slot->Native, ToJolt(Force));
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
        PhysicsKinematicMotionResult MoveKinematicCapsule(const PhysicsKinematicMotionRequest &) const override {
            return {.Status = PhysicsOperationStatus::BackendFailure,
                .Message = "Experimental Jolt kinematic capsule mover is unsupported"};
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
            const auto Update = World.Update(Config.DeltaTime, Config.SubStepCount, &Temp, Jobs.get());
            if (Update != JPH::EPhysicsUpdateError::None)
                throw std::runtime_error("[Physics:JoltRigid] Jolt Update failed with code " +
                    std::to_string(static_cast<unsigned>(Update)));
            for (std::uint32_t Index = 1; Index < BodySlots.size(); ++Index) {
                const auto &Slot = BodySlots[Index];
                if (!Slot.Live || Slot.Description.Anchored) continue;
                JPH::RVec3 Position; JPH::Quat Rotation;
                World.GetBodyInterface().GetPositionAndRotation(Slot.Native, Position, Rotation);
                Result.Motions.push_back({{Index, Slot.Generation}, FromJolt(Position, Rotation)});
            }
            return Result;
        }

    private:
        bool ValidBody(const PhysicsBodyDesc &Value) const {
            if (!Finite(Value.Transform) || !Finite(Value.Shape.Size) ||
                Value.Shape.Size.x <= 0 || Value.Shape.Size.y <= 0 || Value.Shape.Size.z <= 0 ||
                !std::isfinite(Value.Density) || Value.Density < 0 ||
                !std::isfinite(Value.GravityScale) || Value.GravityScale < 0 ||
                !std::isfinite(Value.LinearDamping) || Value.LinearDamping < 0 ||
                !std::isfinite(Value.AngularDamping) || Value.AngularDamping < 0 ||
                Value.CollisionGroup > MaximumPhysicsCollisionGroup ||
                (Value.Anchored && Value.Kinematic) ||
                (Value.MovingBodyCCD && (Value.Anchored || Value.Kinematic || !Value.CanCollide))) return false;
            if (Value.Shape.Kind == PhysicsShapeKind::StaticTriangleMesh)
                return Value.Anchored && Value.Shape.Mesh && Value.Shape.Mesh->Positions && Value.Shape.Mesh->Indices;
            return Value.Shape.Kind == PhysicsShapeKind::Box || Value.Shape.Kind == PhysicsShapeKind::Ball;
        }
        JPH::ShapeRefC MakeShape(const PhysicsShapeDesc &Description) const {
            if (Description.Kind == PhysicsShapeKind::Box)
                return new JPH::BoxShape(ToJolt(Description.Size * 0.5f), 0.0f);
            if (Description.Kind == PhysicsShapeKind::Ball)
                return new JPH::SphereShape(std::min({Description.Size.x, Description.Size.y, Description.Size.z}) * 0.5f);
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
            if (Shape) QueryShapes.emplace(Key, Shape);
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
        JPH::PhysicsSystem World;
        JPH::TempAllocatorImpl Temp;
        std::unique_ptr<JPH::JobSystem> Jobs;
        std::vector<BodySlot> BodySlots;
        std::vector<ConstraintSlot> ConstraintSlots;
        std::unordered_map<std::uint32_t, PhysicsBodyId> NativeMap;
        mutable std::map<std::tuple<int, float, float, float, float, float>, JPH::ShapeRefC> QueryShapes;
        std::size_t ActiveConstraints = 0;
        bool Valid = false;
    };
}

std::unique_ptr<gargantuan::IPhysicsBackend> CreateRigidJoltBackend(
    const gargantuan::PhysicsWorldConfig &Config, int WorkerBudget) {
    return std::make_unique<RigidJoltBackend>(Config, WorkerBudget);
}
}
