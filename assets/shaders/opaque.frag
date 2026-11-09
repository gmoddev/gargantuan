#version 450

layout(location = 0) in vec3 FragmentNormal;
layout(location = 1) in vec4 FragmentColor;
layout(location = 2) in vec4 WorldPosition;
layout(location = 3) in vec4 ShadowPosition;
layout(location = 4) in vec2 FragmentUV;
layout(location = 5) in vec4 FragmentMaterialValues;
layout(location = 6) in vec4 FragmentTangent;
layout(location = 7) flat in vec4 FragmentMaterialFlags;

layout(location = 0) out vec4 OutputColor;

layout(set = 3, binding = 0) uniform WorldUniforms {
    mat4 ViewMatrix;
    mat4 ProjectionMatrix;
    mat4 ShadowBiasMatrix;
    vec4 SunDirectionIntensity;
    vec4 AmbientExposure;
    vec4 SunColorFogEnabled;
    vec4 FogColorStart;
    vec4 CameraPositionFogEnd;
} world;

layout(set = 2, binding = 0) uniform sampler2DShadow ShadowMap;
layout(set = 2, binding = 1) uniform sampler2D BaseColorTexture;
layout(set = 2, binding = 2) uniform sampler2D NormalTexture;

float SHADOW_SPREAD = 2.0;
vec2 SHADOW_TEXEL_SIZE = vec2(1.0 / 2048.0);
vec2 POISSON_DISK[4] = vec2[](
        vec2(-0.94201624, -0.39906216),
        vec2(0.94558609, -0.76890725),
        vec2(-0.094184101, -0.92938870),
        vec2(0.34495938, 0.29387760)
    );

const float Pi = 3.141592653589793;

vec3 SrgbToLinear(vec3 Encoded) {
    return mix(pow((Encoded + 0.055) / 1.055, vec3(2.4)), Encoded / 12.92,
        lessThanEqual(Encoded, vec3(0.04045)));
}

vec3 LinearToSrgb(vec3 Linear) {
    return mix(1.055 * pow(Linear, vec3(1.0 / 2.4)) - 0.055, Linear * 12.92,
        lessThanEqual(Linear, vec3(0.0031308)));
}

vec4 ReadColorTexel(ivec2 Pixel, ivec2 Dimensions) {
    vec4 Texel = texelFetch(BaseColorTexture, (Pixel + Dimensions) % Dimensions, 0);
    return vec4(SrgbToLinear(Texel.rgb), Texel.a);
}

vec4 SampleLinearBaseColor(vec2 UV) {
    // One image can serve both color and data slots under the same residency.
    // Decode color texels before filtering and leave alpha in its linear range.
    ivec2 Dimensions = textureSize(BaseColorTexture, 0);
    if (all(equal(Dimensions, ivec2(1)))) return ReadColorTexel(ivec2(0), Dimensions);
    vec2 Pixel = fract(UV) * vec2(Dimensions) - 0.5;
    ivec2 Lower = ivec2(floor(Pixel));
    vec2 Weight = fract(Pixel);
    return mix(
        mix(ReadColorTexel(Lower, Dimensions), ReadColorTexel(Lower + ivec2(1, 0), Dimensions), Weight.x),
        mix(ReadColorTexel(Lower + ivec2(0, 1), Dimensions), ReadColorTexel(Lower + 1, Dimensions), Weight.x),
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
            Normal = SafeDirection(mat3(Tangent, Bitangent, Normal) * Sampled, Normal);
        }
    }
    return gl_FrontFacing ? Normal : -Normal;
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

float EvaluateShadow(float NormalDotLight) {
    if (world.SunDirectionIntensity.w <= 0.0) return 1.0;
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
        baseColor *= Physical ? SampleLinearBaseColor(FragmentUV) : texture(BaseColorTexture, FragmentUV);
    }
    if (FragmentMaterialValues.y == 1.0 && baseColor.a < FragmentMaterialValues.x) discard;
    if (FragmentMaterialValues.y == 0.0) baseColor.a = 1.0;
    vec3 GeometricNormal = SafeDirection(FragmentNormal, vec3(0.0, 1.0, 0.0));
    vec3 Light = SafeDirection(world.SunDirectionIntensity.xyz, vec3(0.0, 1.0, 0.0));
    float NormalDotLight = max(dot(GeometricNormal, Light), 0.0);
    vec3 Sun = world.SunColorFogEnabled.rgb * world.SunDirectionIntensity.w * EvaluateShadow(NormalDotLight);
    vec3 color;
    if (Physical) {
        vec3 Normal = MaterialNormal(GeometricNormal);
        vec3 View = SafeDirection(world.CameraPositionFogEnd.xyz - WorldPosition.xyz, Normal);
        float Metallic = FragmentMaterialValues.z;
        vec3 Base = clamp(baseColor.rgb, 0.0, 1.0);
        color = Base * (1.0 - Metallic) * world.AmbientExposure.rgb;
        color += Sun * EvaluateDirect(Base, Normal, Light, View, Metallic, FragmentMaterialValues.w);
    } else {
        color = baseColor.rgb * (world.AmbientExposure.rgb + Sun * NormalDotLight);
    }
    if (world.SunColorFogEnabled.w > 0.5) {
        float distanceToCamera = length(WorldPosition.xyz - world.CameraPositionFogEnd.xyz);
        float fogRange = max(world.CameraPositionFogEnd.w - world.FogColorStart.w, 0.0001);
        float fogFactor = clamp((distanceToCamera - world.FogColorStart.w) / fogRange, 0.0, 1.0);
        color = mix(color, world.FogColorStart.rgb, fogFactor);
    }
    color *= world.AmbientExposure.w;
    color = clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);
    if (Physical && FragmentMaterialFlags.w > 0.5) color = LinearToSrgb(color);
    OutputColor = vec4(color, baseColor.a);
}
