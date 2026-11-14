#version 450
#extension GL_GOOGLE_include_directive : require

#include "instance-record.glsl"

layout(location = 0) in vec3 VertexPosition;
layout(location = 1) in vec3 VertexNormal;
layout(location = 2) in vec4 VertexTangent;
layout(location = 3) in vec2 VertexUV;
layout(location = 4) in uvec4 VertexJoints;
layout(location = 5) in vec4 VertexWeights;

layout(set = 1, binding = 0) uniform Uniforms {
    mat4 ShadowMatrix;
} uniforms;

layout(location = 0) out vec2 FragmentUV;

void main() {
    InstanceRecord Part = InstanceData.Records[VisibleInstances.Slots[gl_InstanceIndex]];
    gl_Position = uniforms.ShadowMatrix * Part.ModelMatrix * vec4(VertexPosition, 1.0);
    FragmentUV = VertexUV;
}
