#ifndef GARGANTUAN_WORLD_UNIFORMS
#define GARGANTUAN_WORLD_UNIFORMS
#ifndef WORLD_UNIFORM_SET
#define WORLD_UNIFORM_SET 1
#endif
layout(set = WORLD_UNIFORM_SET, binding = 0) uniform WorldUniforms {
    mat4 ViewMatrix;
    mat4 ProjectionMatrix;
    mat4 ShadowBiasMatrix;
    vec4 SunDirectionIntensity;
    vec4 AmbientExposure;
    vec4 SunColorFogEnabled;
    vec4 FogColorStart;
    vec4 CameraPositionFogEnd;
    vec4 EnvironmentLighting;
    vec4 EnvironmentRotationTone;
    vec4 LocalLighting;
    vec4 ShadowControls;
    mat4 CascadeShadowMatrices[4];
    vec4 CascadeShadowOrigins[4];
    vec4 CascadeShadowSplits[4];
    vec4 CascadeShadowMetrics[4];
    vec4 SpotShadowControls;
    mat4 SpotShadowMatrices[8];
    vec4 SpotShadowOrigins[8];
} world;
#endif
