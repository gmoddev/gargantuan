#version 450
#extension GL_GOOGLE_include_directive : require
#include "ibl_sampling.glsl"

layout(location = 0) out vec4 OutputColor;
layout(set = 2, binding = 0) uniform samplerCube SourceCube;
layout(set = 3, binding = 0) uniform ConvolutionParameters {
    vec4 Target; // Face, resolution, perceptual roughness, diffuse mode.
    vec4 Source; // Source mip 0 resolution and maximum LOD.
} Ibl;

const uint SampleCount = 64;

float SourceLod(float Pdf) {
    float TexelSolidAngle = (4.0 * IblPi) / (6.0 * Ibl.Source.x * Ibl.Source.x);
    float SampleSolidAngle = 1.0 / (float(SampleCount) * max(Pdf, 0.00001));
    return clamp(0.5 * log2(SampleSolidAngle / TexelSolidAngle), 0.0, Ibl.Source.y);
}

vec3 Diffuse(vec3 Normal) {
    vec3 Sum = vec3(0.0);
    for (uint Index = 0; Index < SampleCount; ++Index) {
        vec2 Xi = IblHammersley(Index, SampleCount);
        float Phi = 2.0 * IblPi * Xi.x;
        float Z = sqrt(1.0 - Xi.y);
        float Radius = sqrt(Xi.y);
        vec3 L = IblToWorld(vec3(cos(Phi) * Radius, sin(Phi) * Radius, Z), Normal);
        Sum += textureLod(SourceCube, L, SourceLod(Z / IblPi)).rgb;
    }
    // Cosine importance sampling stores irradiance divided by pi, including
    // the Lambert BRDF's normalization for the opaque shader.
    return Sum / float(SampleCount);
}

vec3 Specular(vec3 Normal, float Roughness) {
    if (Roughness == 0.0) return textureLod(SourceCube, Normal, 0.0).rgb;
    float Alpha = Roughness * Roughness;
    float AlphaSquared = Alpha * Alpha;
    vec3 Sum = vec3(0.0);
    float Weight = 0.0;
    // The split-sum approximation prefilters in the N = V = R frame.
    for (uint Index = 0; Index < SampleCount; ++Index) {
        vec3 LocalH = IblGgxHalfVector(IblHammersley(Index, SampleCount), AlphaSquared);
        vec3 H = IblToWorld(LocalH, Normal);
        vec3 L = 2.0 * LocalH.z * H - Normal;
        float NoL = dot(Normal, L);
        if (NoL > 0.0) {
            float Denominator = 1.0 + (AlphaSquared - 1.0) * LocalH.z * LocalH.z;
            float Pdf = 0.25 * AlphaSquared / (IblPi * Denominator * Denominator);
            Sum += textureLod(SourceCube, L, SourceLod(Pdf)).rgb * NoL;
            Weight += NoL;
        }
    }
    return Sum / max(Weight, 0.00001);
}

void main() {
    vec3 Normal = IblCubeDirection(int(Ibl.Target.x), gl_FragCoord.xy / Ibl.Target.y);
    vec3 Radiance = Ibl.Target.w > 0.5 ? Diffuse(Normal) : Specular(Normal, Ibl.Target.z);
    OutputColor = vec4(clamp(Radiance, vec3(0.0), vec3(65504.0)), 1.0);
}
