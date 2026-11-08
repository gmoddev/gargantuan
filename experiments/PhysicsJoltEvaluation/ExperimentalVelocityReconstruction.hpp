#pragma once
#include <stdexcept>
#include <string_view>

namespace PhysicsJoltEvaluation {
#ifdef PHYSICS_DELTA_VELOCITY
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
#ifdef PHYSICS_DELTA_VELOCITY
// Set before creating the worker pool; a fixture retires its pool before
// another fixture changes this process-local diagnostic setting.
inline int ActiveVelocityReconstruction = 0;
#endif
inline void ConfigureVelocityReconstruction(int Mode) {
#ifdef PHYSICS_DELTA_VELOCITY
    ActiveVelocityReconstruction = Mode;
#else
    if (Mode != 0)
        throw std::runtime_error("[Physics:Numerics] Alternative reconstruction requires isolated patched Jolt");
#endif
}
template<class Settings> void ValidateVelocityConstraints(const Settings &Shared, int Mode) {
    if (Mode != 0 && (!Shared.mDihedralBendConstraints.empty() || !Shared.mSkinnedConstraints.empty() ||
        !Shared.mLRAConstraints.empty() || !Shared.mRodStretchShearConstraints.empty() ||
        !Shared.mRodBendTwistConstraints.empty()))
        throw std::runtime_error("[Physics:Numerics] Numerical counterfactual supports edge and volume constraints only");
}
}

#ifdef PHYSICS_DELTA_VELOCITY
// Included by exactly one entrypoint translation unit per executable.
extern "C" int PhysicsJoltVelocityMode() {
    return PhysicsJoltEvaluation::ActiveVelocityReconstruction;
}
#endif
