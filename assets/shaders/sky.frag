#version 450

layout(location = 0) in vec3 ViewRay;
layout(location = 0) out vec4 OutputColor;

layout(set = 3, binding = 0) uniform SkyUniforms {
    vec4 RightTanAspect;
    vec4 UpTan;
    vec4 LookExposure;
    vec4 EnvironmentColorHasSky;
} sky;

layout(set = 2, binding = 0) uniform sampler2D PositiveX;
layout(set = 2, binding = 1) uniform sampler2D NegativeX;
layout(set = 2, binding = 2) uniform sampler2D PositiveY;
layout(set = 2, binding = 3) uniform sampler2D NegativeY;
layout(set = 2, binding = 4) uniform sampler2D PositiveZ;
layout(set = 2, binding = 5) uniform sampler2D NegativeZ;

vec3 sampleSky(vec3 direction) {
    vec3 absoluteDirection = abs(direction);
    vec2 face;
    float major;
    if (absoluteDirection.x >= absoluteDirection.y && absoluteDirection.x >= absoluteDirection.z) {
        major = absoluteDirection.x;
        if (direction.x >= 0.0) {
            face = vec2(-direction.z, -direction.y) / major;
            return texture(PositiveX, face * 0.5 + 0.5).rgb;
        }
        face = vec2(direction.z, -direction.y) / major;
        return texture(NegativeX, face * 0.5 + 0.5).rgb;
    }
    if (absoluteDirection.y >= absoluteDirection.z) {
        major = absoluteDirection.y;
        if (direction.y >= 0.0) {
            face = vec2(direction.x, direction.z) / major;
            return texture(PositiveY, face * 0.5 + 0.5).rgb;
        }
        face = vec2(direction.x, -direction.z) / major;
        return texture(NegativeY, face * 0.5 + 0.5).rgb;
    }
    major = absoluteDirection.z;
    if (direction.z >= 0.0) {
        face = vec2(direction.x, -direction.y) / major;
        return texture(PositiveZ, face * 0.5 + 0.5).rgb;
    }
    face = vec2(-direction.x, -direction.y) / major;
    return texture(NegativeZ, face * 0.5 + 0.5).rgb;
}

void main() {
    vec3 color = sky.EnvironmentColorHasSky.w > 0.5
        ? sampleSky(normalize(ViewRay)) : sky.EnvironmentColorHasSky.rgb;
    color *= sky.LookExposure.w;
    color = clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);
    OutputColor = vec4(color, 1.0);
}
