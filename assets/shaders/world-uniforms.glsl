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
} world;
#endif
