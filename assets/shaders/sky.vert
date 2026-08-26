#version 450

layout(set = 1, binding = 0) uniform SkyUniforms {
    vec4 RightTanAspect;
    vec4 UpTan;
    vec4 LookExposure;
    vec4 EnvironmentColorHasSky;
} sky;

layout(location = 0) out vec3 ViewRay;

void main() {
    vec2 positions[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 position = positions[gl_VertexIndex];
    gl_Position = vec4(position, 0.0, 1.0);
    ViewRay = sky.LookExposure.xyz + sky.RightTanAspect.xyz * position.x * sky.RightTanAspect.w
        + sky.UpTan.xyz * position.y * sky.UpTan.w;
}
