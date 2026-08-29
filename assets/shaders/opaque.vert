#version 450
#extension GL_GOOGLE_include_directive : require

#include "skinning.glsl"

layout(location = 0) in vec3 VertexPosition;
layout(location = 1) in vec3 VertexNormal;
layout(location = 2) in vec4 VertexTangent;
layout(location = 3) in vec2 VertexUV;
layout(location = 4) in uvec4 VertexJoints;
layout(location = 5) in vec4 VertexWeights;

layout(set = 1, binding = 0) uniform WorldUniforms {
    mat4 ViewMatrix;
    mat4 ProjectionMatrix;
    mat4 ShadowBiasMatrix;
    vec4 SunDirectionIntensity;
    vec4 AmbientExposure;
    vec4 SunColorFogEnabled;
    vec4 FogColorStart;
    vec4 CameraPositionFogEnd;
} world;

layout(set = 1, binding = 1) uniform PartUniforms {
    mat4 ModelMatrix;
    mat4 NormalMatrix;
    vec4 Color;
    vec4 MaterialValues;
} part;

layout(location = 0) out vec3 FragmentNormal;
layout(location = 1) out vec4 FragmentColor;
layout(location = 2) out vec4 WorldPosition;
layout(location = 3) out vec4 ShadowPosition;
layout(location = 4) out vec2 FragmentUV;
layout(location = 5) out vec4 FragmentMaterialValues;

void main() {
	SkinnedVertex Vertex = ApplySkinning(
		VertexPosition, VertexNormal, VertexTangent, VertexJoints, VertexWeights
	);
    // NOTE: if u define ANY of the output variables before gl_Position, it
    // renders black. This shit tookw ay too long to debug
    gl_Position = world.ProjectionMatrix * world.ViewMatrix * part.ModelMatrix * vec4(Vertex.Position, 1.0f);

    FragmentNormal = normalize(mat3(part.NormalMatrix) * Vertex.Normal);
    FragmentColor = part.Color;
    WorldPosition = (part.ModelMatrix * vec4(Vertex.Position, 1.0f)).xyzw;
    ShadowPosition = world.ShadowBiasMatrix * WorldPosition;
    FragmentUV = VertexUV;
    FragmentMaterialValues = part.MaterialValues;
}
