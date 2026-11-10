#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene-color.glsl"

layout(location = 0) in vec3 ViewRay;
layout(location = 0) out vec4 OutputColor;

layout(set = 3, binding = 0) uniform SkyUniforms {
    vec4 RightTanAspect;
    vec4 UpTan;
    vec4 LookExposure;
    vec4 EnvironmentColorHasSky;
    vec4 SceneFlags;
} sky;

layout(set = 2, binding = 0) uniform sampler2D PositiveX;
layout(set = 2, binding = 1) uniform sampler2D NegativeX;
layout(set = 2, binding = 2) uniform sampler2D PositiveY;
layout(set = 2, binding = 3) uniform sampler2D NegativeY;
layout(set = 2, binding = 4) uniform sampler2D PositiveZ;
layout(set = 2, binding = 5) uniform sampler2D NegativeZ;

vec3 ReadSky(sampler2D FaceTexture, vec2 UV) {
    if (sky.SceneFlags.x < 0.5) return texture(FaceTexture, UV).rgb;
    ivec2 MaxPixel = textureSize(FaceTexture, 0) - 1;
    vec2 Pixel = UV * vec2(MaxPixel + 1) - 0.5;
    ivec2 Corner = ivec2(floor(Pixel));
    vec2 Weight = fract(Pixel);
    vec3 A = SrgbToLinear(texelFetch(FaceTexture, clamp(Corner, ivec2(0), MaxPixel), 0).rgb);
    vec3 B = SrgbToLinear(texelFetch(FaceTexture, clamp(Corner + ivec2(1, 0), ivec2(0), MaxPixel), 0).rgb);
    vec3 C = SrgbToLinear(texelFetch(FaceTexture, clamp(Corner + ivec2(0, 1), ivec2(0), MaxPixel), 0).rgb);
    vec3 D = SrgbToLinear(texelFetch(FaceTexture, clamp(Corner + 1, ivec2(0), MaxPixel), 0).rgb);
    return mix(mix(A, B, Weight.x), mix(C, D, Weight.x), Weight.y);
}

vec3 sampleSky(vec3 direction) {
    vec3 absoluteDirection = abs(direction);
    vec2 face;
    float major;
    if (absoluteDirection.x >= absoluteDirection.y && absoluteDirection.x >= absoluteDirection.z) {
        major = absoluteDirection.x;
        if (direction.x >= 0.0) {
            face = vec2(-direction.z, -direction.y) / major;
            return ReadSky(PositiveX, face * 0.5 + 0.5);
        }
        face = vec2(direction.z, -direction.y) / major;
        return ReadSky(NegativeX, face * 0.5 + 0.5);
    }
    if (absoluteDirection.y >= absoluteDirection.z) {
        major = absoluteDirection.y;
        if (direction.y >= 0.0) {
            face = vec2(direction.x, direction.z) / major;
            return ReadSky(PositiveY, face * 0.5 + 0.5);
        }
        face = vec2(direction.x, -direction.z) / major;
        return ReadSky(NegativeY, face * 0.5 + 0.5);
    }
    major = absoluteDirection.z;
    if (direction.z >= 0.0) {
        face = vec2(direction.x, -direction.y) / major;
        return ReadSky(PositiveZ, face * 0.5 + 0.5);
    }
    face = vec2(-direction.x, -direction.y) / major;
    return ReadSky(NegativeZ, face * 0.5 + 0.5);
}

void main() {
    vec3 color = sky.EnvironmentColorHasSky.w > 0.5
        ? sampleSky(normalize(ViewRay)) : sky.EnvironmentColorHasSky.rgb;
    if (sky.SceneFlags.x < 0.5) color = ToneMapScene(color, sky.LookExposure.w);
    OutputColor = vec4(color, 1.0);
}
