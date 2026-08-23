#version 450

layout(location = 0) in vec2 VertexPosition;
layout(location = 1) in vec2 VertexUV;
layout(location = 2) in vec4 VertexColor;

layout(set = 1, binding = 0) uniform UiViewportUniforms {
    vec2 ViewportSize;
    vec2 Padding;
} viewport;

layout(location = 0) out vec2 FragmentUV;
layout(location = 1) out vec4 FragmentColor;

void main() {
    vec2 normalized = VertexPosition / viewport.ViewportSize;
    gl_Position = vec4(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0, 0.0, 1.0);
    FragmentUV = VertexUV;
    FragmentColor = VertexColor;
}
