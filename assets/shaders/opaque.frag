#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene-color.glsl"

layout(location = 0) in vec3 FragmentNormal;
layout(location = 1) in vec4 FragmentColor;
layout(location = 2) in vec4 WorldPosition;
layout(location = 3) in vec4 ShadowPosition;
layout(location = 4) in vec2 FragmentUV;
layout(location = 5) in vec4 FragmentMaterialValues;
layout(location = 6) in vec4 FragmentTangent;
layout(location = 7) flat in vec4 FragmentMaterialFlags;
layout(location = 8) flat in vec4 FragmentEmissiveOcclusion;
layout(location = 9) flat in vec4 FragmentMaterialControls;
layout(location = 10) flat in vec4 FragmentColorSamplerModes;

layout(location = 0) out vec4 OutputColor;

#define WORLD_UNIFORM_SET 3
#include "world-uniforms.glsl"

layout(set = 2, binding = 0) uniform sampler2DShadow ShadowMap;
layout(set = 2, binding = 1) uniform sampler2D BaseColorTexture;
layout(set = 2, binding = 2) uniform sampler2D NormalTexture;
layout(set = 2, binding = 3) uniform sampler2D MetallicRoughnessTexture;
layout(set = 2, binding = 4) uniform sampler2D EmissiveTexture;
layout(set = 2, binding = 5) uniform sampler2D OcclusionTexture;
layout(set = 2, binding = 6) uniform samplerCube EnvironmentIrradiance;
layout(set = 2, binding = 7) uniform samplerCube EnvironmentReflections;
layout(set = 2, binding = 8) uniform sampler2D EnvironmentBrdf;

struct LocalLight {
    vec4 PositionRange;
    vec4 DirectionOuter;
    vec4 ColorIntensity;
    vec4 ShapeInner;
};
layout(std430, set = 2, binding = 13) readonly buffer LocalLightBuffer {
    LocalLight LocalLights[];
};
layout(std430, set = 2, binding = 14) readonly buffer LightTileBuffer {
    uint TileWords[];
};
const uint TileLightLimit = 24u;
const uint TileStride = TileLightLimit + 1u;

float SHADOW_SPREAD = 2.0;
vec2 SHADOW_TEXEL_SIZE = vec2(1.0 / 2048.0);
vec2 POISSON_DISK[4] = vec2[](
        vec2(-0.94201624, -0.39906216),
        vec2(0.94558609, -0.76890725),
        vec2(-0.094184101, -0.92938870),
        vec2(0.34495938, 0.29387760)
    );

#include "shadow-sampling.glsl"

const float Pi = 3.141592653589793;

int AddressTexel(int Pixel, int Size, int Wrap) {
    if (Wrap == 2) return clamp(Pixel, 0, Size - 1);
    if (Wrap == 1) {
        int Period = 2 * Size;
        int Address = (Pixel % Period + Period) % Period;
        return min(Address, Period - 1 - Address);
    }
    return (Pixel % Size + Size) % Size;
}

vec4 ReadColorTexel(sampler2D Map, ivec2 Pixel, ivec2 Dimensions, ivec2 Wrap) {
    ivec2 Address = ivec2(AddressTexel(Pixel.x, Dimensions.x, Wrap.x),
        AddressTexel(Pixel.y, Dimensions.y, Wrap.y));
    vec4 Texel = texelFetch(Map, Address, 0);
    return vec4(SrgbToLinear(Texel.rgb), Texel.a);
}

vec4 SampleLinearColor(sampler2D Map, vec2 UV, bool NativeSrgb, int WrapMode, int FilterMode) {
    if (NativeSrgb) return texture(Map, UV);
    // Budget or capability fallback decodes level-zero color before interpolation.
    ivec2 Dimensions = textureSize(Map, 0);
    ivec2 Wrap = ivec2(WrapMode % 3, WrapMode / 3);
    if (all(equal(Dimensions, ivec2(1)))) return ReadColorTexel(Map, ivec2(0), Dimensions, Wrap);
    vec2 FootprintX = dFdx(UV) * vec2(Dimensions);
    vec2 FootprintY = dFdy(UV) * vec2(Dimensions);
    bool Minified = max(dot(FootprintX, FootprintX), dot(FootprintY, FootprintY)) > 1.0;
    bool Nearest = (FilterMode & (Minified ? 1 : 2)) != 0;
    vec2 Address = mix(fract(UV), mod(UV, 2.0), equal(Wrap, ivec2(1)));
    Address = mix(Address, clamp(UV, 0.0, 1.0), equal(Wrap, ivec2(2)));
    vec2 Pixel = Address * vec2(Dimensions);
    if (Nearest) return ReadColorTexel(Map, ivec2(floor(Pixel)), Dimensions, Wrap);
    Pixel -= 0.5;
    ivec2 Lower = ivec2(floor(Pixel));
    vec2 Weight = fract(Pixel);
    return mix(
        mix(ReadColorTexel(Map, Lower, Dimensions, Wrap), ReadColorTexel(Map, Lower + ivec2(1, 0), Dimensions, Wrap), Weight.x),
        mix(ReadColorTexel(Map, Lower + ivec2(0, 1), Dimensions, Wrap), ReadColorTexel(Map, Lower + 1, Dimensions, Wrap), Weight.x),
        Weight.y);
}

vec3 SafeDirection(vec3 Value, vec3 Fallback) {
    float LengthSquared = dot(Value, Value);
    return LengthSquared > 1e-12 && !isinf(LengthSquared) ? Value * inversesqrt(LengthSquared) : Fallback;
}

vec3 MaterialNormal(vec3 Normal) {
    if (FragmentMaterialFlags.z > 0.5) {
        vec3 Tangent = FragmentTangent.xyz - Normal * dot(Normal, FragmentTangent.xyz);
        if (dot(Tangent, Tangent) > 1e-12 && !isinf(dot(Tangent, Tangent))) {
            Tangent = normalize(Tangent);
            vec3 Bitangent = cross(Normal, Tangent) * (FragmentTangent.w < 0.0 ? -1.0 : 1.0);
            vec3 Sampled = texture(NormalTexture, FragmentUV).xyz * 2.0 - 1.0;
            Sampled.xy *= FragmentMaterialControls.x;
            Normal = SafeDirection(mat3(Tangent, Bitangent, Normal) * Sampled, Normal);
        }
    }
    return gl_FrontFacing ? Normal : -Normal;
}

vec3 EvaluateEnvironment(vec3 Base, vec3 Normal, vec3 View, float Metallic, float Roughness) {
    vec3 Ambient = Base * (1.0 - Metallic) * world.AmbientExposure.rgb;
    if (world.EnvironmentLighting.x < 0.5 || world.EnvironmentLighting.z <= 0.0) return Ambient;
    float NoV = clamp(dot(Normal, View), 0.0, 1.0);
    vec3 F0 = mix(vec3(0.04), Base, Metallic);
    vec3 GrazingFresnel = max(vec3(1.0 - Roughness), F0);
    vec3 Fresnel = F0 + (GrazingFresnel - F0) * pow(1.0 - NoV, 5.0);
    vec3 Diffuse = texture(EnvironmentIrradiance, EnvironmentSampleDirection(Normal, world.EnvironmentRotationTone.xy)).rgb *
        Base * (1.0 - Fresnel) * (1.0 - Metallic);
    vec3 ReflectionRay = EnvironmentSampleDirection(reflect(-View, Normal), world.EnvironmentRotationTone.xy);
    vec3 Radiance = textureLod(EnvironmentReflections, ReflectionRay,
        Roughness * world.EnvironmentLighting.y).rgb;
    vec2 Brdf = texture(EnvironmentBrdf, vec2(NoV, Roughness)).rg;
    return Ambient + world.EnvironmentLighting.z * (Diffuse + Radiance * (F0 * Brdf.x + Brdf.y));
}

vec3 EvaluateDirect(vec3 Base, vec3 Normal, vec3 Light, vec3 View, float Metallic, float Roughness) {
    float NoL = max(dot(Normal, Light), 0.0);
    float NoV = max(dot(Normal, View), 0.0);
    if (NoL <= 0.0 || NoV <= 0.0) return vec3(0.0);
    vec3 Half = SafeDirection(Light + View, Normal);
    float NoH = clamp(dot(Normal, Half), 0.0, 1.0);
    float VoH = clamp(dot(View, Half), 0.0, 1.0);
    float Alpha = max(Roughness * Roughness, 0.002025);
    float AlphaSquared = Alpha * Alpha;
    float Denominator = 1.0 + NoH * NoH * (AlphaSquared - 1.0);
    float Distribution = AlphaSquared / max(Pi * Denominator * Denominator, 1e-12);
    float Visibility = 0.5 / max(NoL * sqrt(NoV * NoV * (1.0 - AlphaSquared) + AlphaSquared) +
        NoV * sqrt(NoL * NoL * (1.0 - AlphaSquared) + AlphaSquared), 1e-6);
    vec3 F0 = mix(vec3(0.04), Base, Metallic);
    vec3 Fresnel = F0 + (1.0 - F0) * pow(1.0 - VoH, 5.0);
    vec3 Diffuse = (1.0 - Fresnel) * (1.0 - Metallic) * Base / Pi;
    return (Diffuse + Distribution * Visibility * Fresnel) * NoL;
}

vec3 EvaluateLocalLights(vec3 Base, vec3 Normal, vec3 View, float Metallic, float Roughness, bool Physical) {
    if (world.LocalLighting.x < 0.5) return vec3(0.0);
    uvec2 Tile = uvec2(gl_FragCoord.xy) / uint(world.LocalLighting.z);
    uint Offset = (Tile.y * uint(world.LocalLighting.y) + Tile.x) * TileStride;
    uint Count = min(TileWords[Offset], TileLightLimit);
    vec3 Result = vec3(0.0);
    for (uint Index = 0u; Index < Count; ++Index) {
        LocalLight Source = LocalLights[TileWords[Offset + 1u + Index]];
        vec3 Delta = Source.PositionRange.xyz - WorldPosition.xyz;
        float DistanceSquared = dot(Delta, Delta);
        float RangeSquared = Source.PositionRange.w * Source.PositionRange.w;
        if (DistanceSquared >= RangeSquared) continue;
        vec3 Light = SafeDirection(Delta, Normal);
        float NormalDotLight = max(dot(Normal, Light), 0.0);
        if (NormalDotLight <= 0.0) continue;
        float Cone = 1.0;
        if (Source.ShapeInner.y > 0.5) {
            float Cosine = dot(Source.DirectionOuter.xyz, -Light);
            float Outer = Source.DirectionOuter.w;
            float Inner = Source.ShapeInner.x;
            Cone = Inner > Outer ? smoothstep(Outer, Inner, Cosine) : step(Outer, Cosine);
        }
        float RelativeSquared = DistanceSquared / RangeSquared;
        float Falloff = max(0.0, 1.0 - RelativeSquared * RelativeSquared);
        float Irradiance = Source.ColorIntensity.w * Cone * Falloff * Falloff / max(DistanceSquared, 0.01);
        Irradiance *= EvaluateSpotShadow(Source.ShapeInner.z, Normal, NormalDotLight);
        vec3 Response = Physical ? EvaluateDirect(Base, Normal, Light, View, Metallic, Roughness)
            : Base * NormalDotLight;
        Result += Source.ColorIntensity.rgb * Irradiance * Response;
    }
    return Result;
}

float EvaluateShadow(float NormalDotLight) {
    if (world.SunDirectionIntensity.w <= 0.0 || world.EnvironmentLighting.w < 0.5) return 1.0;
    if (world.ShadowControls.x > 0.5)
        return EvaluateDirectionalCascades(SafeDirection(FragmentNormal, vec3(0, 1, 0)), NormalDotLight);
    vec3 shadowCoordinate = ShadowPosition.xyz / ShadowPosition.w;
    float bias = max(0.003 * (1.0 - NormalDotLight), 0.0005);
    float currentDepth = shadowCoordinate.z - bias;

    float shadowFactor = 1.0;
    if (shadowCoordinate.x >= 0.0 && shadowCoordinate.x <= 1.0 &&
            shadowCoordinate.y >= 0.0 && shadowCoordinate.y <= 1.0 &&
            shadowCoordinate.z >= 0.0 && shadowCoordinate.z <= 1.0) {
        shadowFactor = 0.0;
        for (int i = 0; i < 4; i++) {
            vec2 sampleUV = shadowCoordinate.xy + (POISSON_DISK[i] * SHADOW_TEXEL_SIZE * SHADOW_SPREAD);
            shadowFactor += texture(ShadowMap, vec3(sampleUV, currentDepth));
        }
        shadowFactor /= 4;
    }

    return shadowFactor;
}

void main() {
    bool Physical = FragmentMaterialFlags.x > 0.5;
    vec4 baseColor = FragmentColor;
    if (FragmentMaterialFlags.y > 0.5) {
        baseColor *= Physical ? SampleLinearColor(BaseColorTexture, FragmentUV, FragmentMaterialControls.z > 0.5,
            int(FragmentColorSamplerModes.x), int(FragmentColorSamplerModes.z)) : texture(BaseColorTexture, FragmentUV);
    }
    if (FragmentMaterialValues.y == 1.0 && baseColor.a < FragmentMaterialValues.x) discard;
    if (FragmentMaterialValues.y != 2.0) baseColor.a = 1.0;
    vec3 GeometricNormal = SafeDirection(FragmentNormal, vec3(0.0, 1.0, 0.0));
    vec3 Light = SafeDirection(world.SunDirectionIntensity.xyz, vec3(0.0, 1.0, 0.0));
    float NormalDotLight = max(dot(GeometricNormal, Light), 0.0);
    vec3 Sun = world.SunColorFogEnabled.rgb * world.SunDirectionIntensity.w * EvaluateShadow(NormalDotLight);
    vec3 color;
    if (Physical) {
        vec3 Normal = MaterialNormal(GeometricNormal);
        vec3 View = SafeDirection(world.CameraPositionFogEnd.xyz - WorldPosition.xyz, Normal);
        float Metallic = FragmentMaterialValues.z;
        float Roughness = FragmentMaterialValues.w;
        if (FragmentMaterialFlags.x > 1.5) {
            vec2 RoughnessMetallic = texture(MetallicRoughnessTexture, FragmentUV).gb;
            Metallic *= RoughnessMetallic.y;
            Roughness *= RoughnessMetallic.x;
        }
        vec3 Base = clamp(baseColor.rgb, 0.0, 1.0);
        float Occlusion = FragmentEmissiveOcclusion.w > 0.0 ?
            mix(1.0, texture(OcclusionTexture, FragmentUV).r, FragmentEmissiveOcclusion.w) : 1.0;
        color = EvaluateEnvironment(Base, Normal, View, Metallic, Roughness) * Occlusion;
        color += Sun * EvaluateDirect(Base, Normal, Light, View, Metallic, Roughness);
        color += EvaluateLocalLights(Base, Normal, View, Metallic, Roughness, true);
        vec3 Emissive = FragmentEmissiveOcclusion.rgb;
        if (FragmentMaterialControls.y > 0.5) Emissive *= SampleLinearColor(EmissiveTexture, FragmentUV,
            FragmentMaterialControls.w > 0.5, int(FragmentColorSamplerModes.y), int(FragmentColorSamplerModes.w)).rgb;
        color += Emissive;
    } else {
        color = baseColor.rgb * (world.AmbientExposure.rgb + Sun * NormalDotLight);
        color += EvaluateLocalLights(baseColor.rgb, GeometricNormal, GeometricNormal, 0.0, 1.0, false);
    }
    if (world.SunColorFogEnabled.w > 0.5) {
        float distanceToCamera = length(WorldPosition.xyz - world.CameraPositionFogEnd.xyz);
        float fogRange = max(world.CameraPositionFogEnd.w - world.FogColorStart.w, 0.0001);
        float fogFactor = clamp((distanceToCamera - world.FogColorStart.w) / fogRange, 0.0, 1.0);
        color = mix(color, world.FogColorStart.rgb, fogFactor);
    }
    if (FragmentMaterialFlags.w > 1.5) {
        OutputColor = vec4(FiniteSceneColor(color), baseColor.a);
        return;
    }
    color = ToneMapScene(color, world.AmbientExposure.w, world.EnvironmentRotationTone.z > 0.5);
    if (Physical && FragmentMaterialFlags.w > 0.5) color = LinearToSrgb(color);
    OutputColor = vec4(color, baseColor.a);
}
