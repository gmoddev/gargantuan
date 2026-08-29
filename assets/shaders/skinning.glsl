#ifndef GARGANTUAN_SKINNING_GLSL
#define GARGANTUAN_SKINNING_GLSL

struct SkinPaletteEntry {
    mat4 PositionMatrix;
    mat4 NormalMatrix;
};

layout(std140, set = 0, binding = 0) readonly buffer SkinPaletteBuffer {
    SkinPaletteEntry Entries[];
} SkinPalette;

struct SkinnedVertex {
    vec3 Position;
    vec3 Normal;
    vec4 Tangent;
};

SkinnedVertex ApplySkinning(
    vec3 Position,
    vec3 Normal,
    vec4 Tangent,
    uvec4 Joints,
    vec4 Weights
) {
    vec4 SkinnedPosition = vec4(0.0);
    vec3 SkinnedNormal = vec3(0.0);
    vec3 SkinnedTangent = vec3(0.0);
    float AppliedWeight = 0.0;
    uint EntryCount = SkinPalette.Entries.length();

    for (uint Influence = 0; Influence < 4; ++Influence) {
        float Weight = Weights[Influence];
        uint Joint = Joints[Influence];
        if (Weight <= 0.0 || Joint >= EntryCount) continue;
        SkinnedPosition += SkinPalette.Entries[Joint].PositionMatrix * vec4(Position, 1.0) * Weight;
        SkinnedNormal += mat3(SkinPalette.Entries[Joint].NormalMatrix) * Normal * Weight;
        SkinnedTangent += mat3(SkinPalette.Entries[Joint].NormalMatrix) * Tangent.xyz * Weight;
        AppliedWeight += Weight;
    }

    if (AppliedWeight <= 0.000001) return SkinnedVertex(Position, Normal, Tangent);
    float ReciprocalWeight = 1.0 / AppliedWeight;
    return SkinnedVertex(
        SkinnedPosition.xyz * ReciprocalWeight,
        normalize(SkinnedNormal),
        vec4(normalize(SkinnedTangent), Tangent.w)
    );
}

#endif
