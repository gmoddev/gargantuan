#version 450

layout(location = 0) in vec3 FragmentNormal;
layout(location = 1) in vec4 FragmentColor;
layout(location = 2) in vec4 WorldPosition;
layout(location = 3) in vec4 ShadowPosition;
layout(location = 4) in vec2 FragmentUV;
layout(location = 5) in vec4 FragmentMaterialValues;

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

float SHADOW_SPREAD = 2.0;
vec2 SHADOW_TEXEL_SIZE = vec2(1.0 / 2048.0);
vec2 POISSON_DISK[4] = vec2[](
        vec2(-0.94201624, -0.39906216),
        vec2(0.94558609, -0.76890725),
        vec2(-0.094184101, -0.92938870),
        vec2(0.34495938, 0.29387760)
    );

void main() {
    vec3 shadowCoordinate = ShadowPosition.xyz / ShadowPosition.w;

    vec3 n = normalize(FragmentNormal);
    vec3 l = normalize(world.SunDirectionIntensity.xyz);
    float nDotL = max(dot(n, l), 0.0);

    float bias = max(0.003 * (1.0 - nDotL), 0.0005);
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

    vec4 baseColor = FragmentColor * texture(BaseColorTexture, FragmentUV);
    if (FragmentMaterialValues.y == 1.0 && baseColor.a < FragmentMaterialValues.x) discard;
    if (FragmentMaterialValues.y == 0.0) baseColor.a = 1.0;
    vec3 direct = world.SunColorFogEnabled.rgb * world.SunDirectionIntensity.w * nDotL * shadowFactor;
    vec3 color = baseColor.rgb * (world.AmbientExposure.rgb + direct);
    if (world.SunColorFogEnabled.w > 0.5) {
        float distanceToCamera = length(WorldPosition.xyz - world.CameraPositionFogEnd.xyz);
        float fogRange = max(world.CameraPositionFogEnd.w - world.FogColorStart.w, 0.0001);
        float fogFactor = clamp((distanceToCamera - world.FogColorStart.w) / fogRange, 0.0, 1.0);
        color = mix(color, world.FogColorStart.rgb, fogFactor);
    }
    color *= world.AmbientExposure.w;
    color = clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);
    OutputColor = vec4(color, baseColor.a);
}
