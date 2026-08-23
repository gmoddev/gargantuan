#version 450

layout(location = 0) in vec2 FragmentUV;
layout(location = 1) in vec4 FragmentColor;

layout(set = 2, binding = 0) uniform sampler2D Atlas;
layout(set = 3, binding = 0) uniform UiBatchUniforms {
	vec4 Values;
} batch;

layout(location = 0) out vec4 OutputColor;

void main() {
	OutputColor = texture(Atlas, FragmentUV) * FragmentColor * batch.Values.x;
}
