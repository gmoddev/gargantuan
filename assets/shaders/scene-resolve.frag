#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene-color.glsl"

layout(set = 2, binding = 0) uniform sampler2D SceneColor;
layout(set = 3, binding = 0) uniform ResolveUniforms {
    vec4 ExposureAndEncoding;
};
layout(location = 0) out vec4 OutputColor;

void main() {
    vec3 Linear = texelFetch(SceneColor, ivec2(gl_FragCoord.xy), 0).rgb;
    vec3 Display = ToneMapScene(Linear, ExposureAndEncoding.x);
    if (ExposureAndEncoding.y > 0.5) Display = LinearToSrgb(Display);
    OutputColor = vec4(Display, 1.0);
}
