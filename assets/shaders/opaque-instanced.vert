#version 450
#extension GL_GOOGLE_include_directive : require

#include "skinning.glsl"
#include "world-uniforms.glsl"
#include "instance-record.glsl"

layout(location = 0) in vec3 VertexPosition;
layout(location = 1) in vec3 VertexNormal;
layout(location = 2) in vec4 VertexTangent;
layout(location = 3) in vec2 VertexUV;
layout(location = 4) in uvec4 VertexJoints;
layout(location = 5) in vec4 VertexWeights;

layout(location = 0) out vec3 FragmentNormal;
layout(location = 1) out vec4 FragmentColor;
layout(location = 2) out vec4 WorldPosition;
layout(location = 3) out vec4 ShadowPosition;
layout(location = 4) out vec2 FragmentUV;
layout(location = 5) out vec4 FragmentMaterialValues;
layout(location = 6) out vec4 FragmentTangent;
layout(location = 7) flat out vec4 FragmentMaterialFlags;
layout(location = 8) flat out vec4 FragmentEmissiveOcclusion;
layout(location = 9) flat out vec4 FragmentMaterialControls;
layout(location = 10) flat out vec4 FragmentColorSamplerModes;

void main() {
    InstanceRecord Part = InstanceData.Records[VisibleInstances.Slots[gl_InstanceIndex]];
    SkinnedVertex Vertex = ApplySkinning(
        VertexPosition, VertexNormal, VertexTangent, VertexJoints, VertexWeights
    );
    gl_Position = world.ProjectionMatrix * world.ViewMatrix * Part.ModelMatrix * vec4(Vertex.Position, 1.0f);
    FragmentNormal = normalize(mat3(Part.NormalMatrix) * Vertex.Normal);
    FragmentColor = Part.Color;
    WorldPosition = (Part.ModelMatrix * vec4(Vertex.Position, 1.0f)).xyzw;
    ShadowPosition = world.ShadowBiasMatrix * WorldPosition;
    FragmentUV = VertexUV;
    FragmentMaterialValues = Part.MaterialValues;
    mat3 Model = mat3(Part.ModelMatrix);
    float Orientation = determinant(Model) < 0.0 ? -1.0 : 1.0;
    FragmentTangent = vec4(Model * Vertex.Tangent.xyz, Orientation * Vertex.Tangent.w);
    FragmentMaterialFlags = Part.MaterialFlags;
    FragmentEmissiveOcclusion = Part.EmissiveOcclusion;
    FragmentMaterialControls = Part.MaterialControls;
    FragmentColorSamplerModes = Part.ColorSamplerModes;
}
