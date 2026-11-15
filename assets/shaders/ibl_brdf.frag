#version 450
#extension GL_GOOGLE_include_directive : require
#include "ibl_sampling.glsl"

layout(location = 0) out vec4 OutputColor;
layout(set = 3, binding = 0) uniform BrdfParameters {
    vec4 Dimensions;
} Brdf;

void main() {
    vec2 Uv = gl_FragCoord.xy / Brdf.Dimensions.x;
    float NoV = Uv.x;
    float Alpha = Uv.y * Uv.y;
    float AlphaSquared = Alpha * Alpha;
    vec3 V = vec3(sqrt(1.0 - NoV * NoV), 0.0, NoV);
    vec2 Sum = vec2(0.0);
    const uint SampleCount = 128;
    for (uint Index = 0; Index < SampleCount; ++Index) {
        vec3 H = IblGgxHalfVector(IblHammersley(Index, SampleCount), AlphaSquared);
        float VoH = max(dot(V, H), 0.0);
        vec3 L = 2.0 * VoH * H - V;
        float NoL = L.z;
        if (NoL > 0.0) {
            float SmithV = NoL * sqrt(NoV * NoV * (1.0 - AlphaSquared) + AlphaSquared);
            float SmithL = NoV * sqrt(NoL * NoL * (1.0 - AlphaSquared) + AlphaSquared);
            float Visibility = 0.5 / max(SmithV + SmithL, 0.00001);
            float Weight = 4.0 * NoL * Visibility * VoH / max(H.z, 0.00001);
            float Fresnel = pow(1.0 - VoH, 5.0);
            Sum += vec2(1.0 - Fresnel, Fresnel) * Weight;
        }
    }
    OutputColor = vec4(clamp(Sum / float(SampleCount), 0.0, 1.0), 0.0, 1.0);
}
