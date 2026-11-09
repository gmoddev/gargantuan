#pragma once
#include <cstdlib>
#include <stdexcept>
#include <string>
namespace PhysicsEvaluation {
inline int ContactProbeParts() {
    const char *Value = std::getenv("GANTRIA_PROBE_PARTS");
    const int Parts = Value ? std::atoi(Value) : 1;
    if (Parts != 1 && Parts != 2 && Parts != 4 && Parts != 8) throw std::runtime_error("Invalid contact probe parts");
    return Parts;
}
inline unsigned ContactProbeSweeps(unsigned Default) {
    const char *Value = std::getenv("GANTRIA_PROBE_SWEEPS");
    const unsigned Sweeps = Value ? unsigned(std::atoi(Value)) : Default;
    if (Sweeps != 2 && Sweeps != 8 && Sweeps != 32) throw std::runtime_error("Invalid contact probe sweeps");
    return Sweeps;
}
inline float ContactProbeFloat(const char *Name, float Default) {
    const char *Value = std::getenv(Name);
    return Value ? std::stof(Value) : Default;
}
}
