#version 450

layout(location = 0) in vec2 FragmentUV;
layout(location = 1) in vec4 FragmentColor;

layout(set = 2, binding = 0) uniform sampler2D Atlas;
layout(set = 3, binding = 0) uniform UiBatchUniforms {
	vec4 Values;
} batch;

layout(location = 0) out vec4 OutputColor;

void main() {
	vec4 Sample = texture(Atlas, FragmentUV);
	// Geometry blended over a transparent viewport clear produces premultiplied
	// pixels. Convert to straight alpha for the ordinary GUI blend state.
	if (batch.Values.y > 0.5 && Sample.a > 0.0) Sample.rgb /= Sample.a;
	OutputColor = Sample * FragmentColor * batch.Values.x;
}
