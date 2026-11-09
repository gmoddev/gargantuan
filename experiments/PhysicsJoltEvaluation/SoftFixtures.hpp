#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include "ExperimentalVelocityReconstruction.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace PhysicsJoltEvaluation {
    using Clock = std::chrono::steady_clock;

    inline double Milliseconds(Clock::time_point Start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - Start).count();
    }

    // One runtime per process. It outlives every world, native shape, and soft
    // settings object created by these probes.
    class JoltRuntime final {
      public:
        JoltRuntime() {
            JPH::Factory::sInstance = new JPH::Factory;
            JPH::RegisterTypes();
        }
        ~JoltRuntime() {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
        JoltRuntime(const JoltRuntime &) = delete;
        JoltRuntime &operator=(const JoltRuntime &) = delete;
    };

    struct Layers final : JPH::BroadPhaseLayerInterface {
        JPH::uint GetNumBroadPhaseLayers() const override { return 3; }
        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer Layer) const override {
            return JPH::BroadPhaseLayer(static_cast<JPH::uint8>(Layer));
        }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "Evaluation"; }
#endif
    };

    struct Pairs final : JPH::ObjectLayerPairFilter {
        bool EnableSoftRigidContact = true;
        explicit Pairs(bool Enabled = true) : EnableSoftRigidContact(Enabled) {}
        bool ShouldCollide(JPH::ObjectLayer A, JPH::ObjectLayer B) const override {
            if (!EnableSoftRigidContact && ((A == 1 && B == 2) || (A == 2 && B == 1))) return false;
            return A != 0 || B != 0;
        }
    };

    struct BroadPhasePairs final : JPH::ObjectVsBroadPhaseLayerFilter {
        bool EnableSoftRigidContact = true;
        explicit BroadPhasePairs(bool Enabled = true) : EnableSoftRigidContact(Enabled) {}
        bool ShouldCollide(JPH::ObjectLayer A, JPH::BroadPhaseLayer B) const override {
            return Pairs{EnableSoftRigidContact}.ShouldCollide(A, B.GetValue());
        }
    };

    class JoltWorld final {
      public:
        explicit JoltWorld(int WorkerCount, bool EnableSoftRigidContact = true)
            : PairsValue(EnableSoftRigidContact), BroadPhasePairsValue(EnableSoftRigidContact),
              Temp(32 * 1024 * 1024), Jobs(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, WorkerCount) {
            World.Init(8192, 0, 16384, 8192, LayersValue, BroadPhasePairsValue, PairsValue);
        }

        ~JoltWorld() {
            auto &Bodies = World.GetBodyInterface();
            for (auto Body : BodyIds) {
                Bodies.RemoveBody(Body);
                Bodies.DestroyBody(Body);
            }
        }

        JoltWorld(const JoltWorld &) = delete;
        JoltWorld &operator=(const JoltWorld &) = delete;

        JPH::BodyID AddRigid(JPH::RefConst<JPH::Shape> Shape, JPH::RVec3 Position,
                             JPH::EMotionType MotionType, JPH::ObjectLayer Layer, float Mass = 0.0f) {
            JPH::BodyCreationSettings Settings(Shape, Position, JPH::Quat::sIdentity(), MotionType, Layer);
            if (MotionType == JPH::EMotionType::Dynamic) {
                Settings.mAllowSleeping = false;
                Settings.mLinearDamping = 0.0f;
                Settings.mAngularDamping = 0.0f;
                if (Mass > 0.0f) {
                    Settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                    Settings.mMassPropertiesOverride.mMass = Mass;
                }
            }
            auto Id = World.GetBodyInterface().CreateAndAddBody(
                Settings, MotionType == JPH::EMotionType::Dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
            if (Id.IsInvalid()) throw std::runtime_error("[Physics:JoltSoft] Rigid body admission failed");
            BodyIds.push_back(Id);
            return Id;
        }

        JPH::BodyID AddSoft(const JPH::Ref<JPH::SoftBodySharedSettings> &Shared, JPH::RVec3 Position,
                            int Iterations = 16, bool UpdatePosition = false, float LinearDamping = 0.05f,
                            int ConstraintSweeps = 1) {
            JPH::SoftBodyCreationSettings Settings(Shared, Position, JPH::Quat::sIdentity(), 2);
            ApplyVelocityReconstruction(Settings);
            ApplyConstraintSweeps(Settings, ConstraintSweeps);
            Settings.mNumIterations = static_cast<JPH::uint32>(Iterations);
            Settings.mUpdatePosition = UpdatePosition;
            Settings.mAllowSleeping = false;
            Settings.mLinearDamping = LinearDamping;
            Settings.mVertexRadius = 0.05f;
            Settings.mFacesDoubleSided = true;
            auto Id = World.GetBodyInterface().CreateAndAddSoftBody(Settings, JPH::EActivation::Activate);
            if (Id.IsInvalid()) throw std::runtime_error("[Physics:JoltSoft] Soft body admission failed");
            BodyIds.push_back(Id);
            return Id;
        }

        void Remove(JPH::BodyID Id) {
            auto Found = std::find(BodyIds.begin(), BodyIds.end(), Id);
            if (Found == BodyIds.end()) throw std::runtime_error("[Physics:JoltSoft] Unknown body removal");
            World.GetBodyInterface().RemoveBody(Id);
            World.GetBodyInterface().DestroyBody(Id);
            BodyIds.erase(Found);
        }

        double Step(float DeltaTime = 1.0f / 60.0f, int CollisionSteps = 1) {
            const auto Start = Clock::now();
            if (World.Update(DeltaTime, CollisionSteps, &Temp, &Jobs) != JPH::EPhysicsUpdateError::None)
                throw std::runtime_error("[Physics:JoltSoft] PhysicsSystem::Update failed");
            return Milliseconds(Start);
        }

      private:
        Layers LayersValue;
        Pairs PairsValue;
        BroadPhasePairs BroadPhasePairsValue;
        JPH::TempAllocatorImpl Temp;
        JPH::JobSystemThreadPool Jobs;
        std::vector<JPH::BodyID> BodyIds;

      public:
        // Destroyed before Jobs, Temp, filters and layers.
        JPH::PhysicsSystem World;
    };

    inline JPH::Ref<JPH::SoftBodySharedSettings> Cloth(int Resolution, float Size,
                                                       bool Horizontal, bool PinBoundary,
                                                       float ParticleMass = 0.1f,
                                                       float StretchCompliance = 1.0e-6f) {
        JPH::Ref<JPH::SoftBodySharedSettings> Shared = new JPH::SoftBodySharedSettings;
        const auto Index = [Resolution](int X, int Y) { return Y * Resolution + X; };
        for (int Y = 0; Y < Resolution; ++Y) for (int X = 0; X < Resolution; ++X) {
            float A = (static_cast<float>(X) / (Resolution - 1) - 0.5f) * Size;
            float B = (static_cast<float>(Y) / (Resolution - 1) - 0.5f) * Size;
            const bool Pinned = PinBoundary ? (X == 0 || Y == 0 || X + 1 == Resolution || Y + 1 == Resolution) : Y == 0;
            JPH::SoftBodySharedSettings::Vertex Vertex;
            Vertex.mPosition = Horizontal ? JPH::Float3(A, 0.0f, B) : JPH::Float3(A, -B, 0.0f);
            Vertex.mInvMass = Pinned ? 0.0f : 1.0f / ParticleMass;
            Shared->mVertices.push_back(Vertex);
        }
        const auto Edge = [&](int A, int B, float Compliance) {
            Shared->mEdgeConstraints.emplace_back(A, B, Compliance);
        };
        for (int Y = 0; Y < Resolution; ++Y) for (int X = 0; X < Resolution; ++X) {
            const int I = Index(X, Y);
            if (X + 1 < Resolution) Edge(I, Index(X + 1, Y), StretchCompliance);
            if (Y + 1 < Resolution) Edge(I, Index(X, Y + 1), StretchCompliance);
            if (X + 1 < Resolution && Y + 1 < Resolution) {
                Edge(I, Index(X + 1, Y + 1), StretchCompliance);
                Edge(Index(X + 1, Y), Index(X, Y + 1), StretchCompliance);
                Shared->mFaces.emplace_back(I, Index(X, Y + 1), Index(X + 1, Y + 1));
                Shared->mFaces.emplace_back(I, Index(X + 1, Y + 1), Index(X + 1, Y));
            }
            if (X + 2 < Resolution) Edge(I, Index(X + 2, Y), 1.0e-4f);
            if (Y + 2 < Resolution) Edge(I, Index(X, Y + 2), 1.0e-4f);
        }
        Shared->CalculateEdgeLengths();
        Shared->Optimize();
        return Shared;
    }

    inline JPH::Ref<JPH::SoftBodySharedSettings> Rubber(int Resolution = 4, float Spacing = 0.5f) {
        auto Shared = JPH::SoftBodySharedSettings::sCreateCube(static_cast<JPH::uint>(Resolution), Spacing);
        for (auto &Vertex : Shared->mVertices) Vertex.mInvMass = 10.0f;
        for (auto &Edge : Shared->mEdgeConstraints) Edge.mCompliance = 1.0e-6f;
        for (auto &Volume : Shared->mVolumeConstraints) Volume.mCompliance = 1.0e-5f;
        return Shared;
    }

    inline double MaxEdgeStretch(const JPH::SoftBodySharedSettings &Shared,
                                 const JPH::SoftBodyMotionProperties &Motion) {
        const auto &Vertices = Motion.GetVertices();
        double Maximum = 0.0;
        for (const auto &Edge : Shared.mEdgeConstraints) {
            const auto Length = (Vertices[Edge.mVertex[1]].mPosition - Vertices[Edge.mVertex[0]].mPosition).Length();
            Maximum = std::max(Maximum, std::abs(static_cast<double>(Length / Edge.mRestLength) - 1.0));
        }
        return Maximum;
    }
}
