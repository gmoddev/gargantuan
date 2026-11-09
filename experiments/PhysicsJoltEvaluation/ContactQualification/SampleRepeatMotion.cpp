// Isolated reconstruction of the x component of the authored repeat-3
// sphere's ContactQualification.cpp::Sphere::Target substep sampler.
// This isolates scalar platform math; it does not replay the cloth solver.
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>

int main() {
    constexpr float DeltaTime = 1.0f / 60.0f;
    constexpr float Start = 5.2f;
    constexpr float Velocity = -0.9f;
    constexpr float Frequency = 0.75f;
    constexpr int Steps = 32;
    for (int Frame = 0; Frame < 240; ++Frame) {
        for (int Substep = 0; Substep < Steps; ++Substep) {
            const float Time = (float(Frame) + float(Substep + 1) / Steps) * DeltaTime;
            const float Angle = 2.0f * 3.14159265358979323846f * Frequency * Time;
            const float Sine = std::sin(Angle);
            const float Position = Start + Velocity * (Sine / Frequency);
            std::cout << Frame << ',' << Substep << ',' << std::bit_cast<std::uint32_t>(Time)
                      << ',' << std::bit_cast<std::uint32_t>(Angle)
                      << ',' << std::bit_cast<std::uint32_t>(Sine)
                      << ',' << std::bit_cast<std::uint32_t>(Position) << '\n';
        }
    }
}
