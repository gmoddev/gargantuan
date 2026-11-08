#pragma once
#include <stdexcept>
#include <string_view>
#ifdef PHYSICS_GANTRIA_JOLT
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#endif

namespace PhysicsJoltEvaluation {
#if defined(PHYSICS_DELTA_VELOCITY) || defined(PHYSICS_GANTRIA_JOLT)
inline constexpr bool NumericalReconstructionPatch = true;
#else
inline constexpr bool NumericalReconstructionPatch = false;
#endif
inline int ParseVelocityReconstruction(std::string_view Value) {
    if (Value == "difference") return 0;
    if (Value == "delta") return 1;
    if (Value == "applieddelta") return 2;
    throw std::runtime_error("[Physics:Numerics] Unknown velocity reconstruction");
}
inline const char *VelocityReconstructionName(int Mode) {
    return Mode == 1 ? "delta" : Mode == 2 ? "applieddelta" : "difference";
}
inline void ValidateConstraintSweeps(int Sweeps, int Mode) {
    if (Sweeps != 1 && Sweeps != 2 && Sweeps != 4 && Sweeps != 8)
        throw std::runtime_error("[Physics:Numerics] Constraint sweeps must be 1, 2, 4, or 8");
    if (Sweeps > 1 && Mode != 1)
        throw std::runtime_error("[Physics:Numerics] Multiple constraint sweeps require delta reconstruction");
#ifndef PHYSICS_GANTRIA_CONVERGENCE
    if (Sweeps > 1)
        throw std::runtime_error("[Physics:Numerics] Multiple constraint sweeps require isolated convergence Jolt");
#endif
}
#if defined(PHYSICS_DELTA_VELOCITY) || defined(PHYSICS_GANTRIA_JOLT)
// Set before creating the worker pool; a fixture retires its pool before
// another fixture changes this process-local diagnostic setting.
inline int ActiveVelocityReconstruction = 0;
#endif
inline void ConfigureVelocityReconstruction(int Mode) {
#ifdef PHYSICS_GANTRIA_JOLT
    if (Mode != 0 && Mode != 1)
        throw std::runtime_error("[Physics:Numerics] Native Gantria patch supports difference or delta");
    ActiveVelocityReconstruction = Mode;
#elif defined(PHYSICS_DELTA_VELOCITY)
    ActiveVelocityReconstruction = Mode;
#else
    if (Mode != 0)
        throw std::runtime_error("[Physics:Numerics] Alternative reconstruction requires isolated patched Jolt");
#endif
}
template<class Settings> void ValidateVelocityConstraints(const Settings &Shared, int Mode) {
#ifndef PHYSICS_GANTRIA_JOLT
    if (Mode != 0 && (!Shared.mDihedralBendConstraints.empty() || !Shared.mSkinnedConstraints.empty() ||
        !Shared.mLRAConstraints.empty() || !Shared.mRodStretchShearConstraints.empty() ||
        !Shared.mRodBendTwistConstraints.empty()))
        throw std::runtime_error("[Physics:Numerics] Numerical counterfactual supports edge and volume constraints only");
#endif
}
template<class Settings> void ApplyVelocityReconstruction(Settings &Creation) {
#ifdef PHYSICS_GANTRIA_JOLT
    Creation.mVelocityReconstruction = ActiveVelocityReconstruction == 1 ?
        JPH::ESoftBodyVelocityReconstruction::ConstraintDelta :
        JPH::ESoftBodyVelocityReconstruction::PositionDifference;
#endif
}
template<class Settings> void ApplyConstraintSweeps(Settings &Creation, int Sweeps) {
#ifdef PHYSICS_GANTRIA_CONVERGENCE
    Creation.mNumConstraintIterations = static_cast<JPH::uint32>(Sweeps);
#else
    if (Sweeps != 1)
        throw std::runtime_error("[Physics:Numerics] Multiple constraint sweeps require isolated convergence Jolt");
#endif
}
}

#ifdef PHYSICS_DELTA_VELOCITY
// Included by exactly one entrypoint translation unit per executable.
extern "C" int PhysicsJoltVelocityMode() {
    return PhysicsJoltEvaluation::ActiveVelocityReconstruction;
}
#endif
