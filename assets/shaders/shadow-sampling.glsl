#ifndef GARGANTUAN_SHADOW_SAMPLING
#define GARGANTUAN_SHADOW_SAMPLING

layout(set = 2, binding = 9) uniform sampler2DArrayShadow CascadeShadowMap;
layout(set = 2, binding = 10) uniform sampler2DArrayShadow SpotShadowMap;

const vec2 ShadowPoisson[4] = vec2[](
    vec2(-0.94201624, -0.39906216),
    vec2(0.94558609, -0.76890725),
    vec2(-0.094184101, -0.92938870),
    vec2(0.34495938, 0.29387760)
);

bool ShadowCoordinateInside(vec3 Coordinate) {
    return all(greaterThanEqual(Coordinate, vec3(0.0))) &&
        all(lessThanEqual(Coordinate, vec3(1.0)));
}

float SampleCascadeShadow(int Index, vec3 Normal, float NormalDotLight) {
    vec3 Relative = WorldPosition.xyz + Normal * world.ShadowControls.z -
        world.CascadeShadowOrigins[Index].xyz;
    vec4 Projected = world.CascadeShadowMatrices[Index] * vec4(Relative, 1.0);
    if (Projected.w <= 0.0) return 1.0;
    vec3 Coordinate = Projected.xyz / Projected.w;
    if (!ShadowCoordinateInside(Coordinate)) return 1.0;
    float TexelWorld = world.CascadeShadowMetrics[Index].x;
    float DepthSpan = max(world.CascadeShadowMetrics[Index].y, 0.001);
    float BaseWorld = min(world.ShadowControls.y * DepthSpan, 2.0 * TexelWorld);
    float SlopeWorld = 0.5 * TexelWorld * (1.0 - NormalDotLight);
    float CompareDepth = Coordinate.z - (BaseWorld + SlopeWorld) / DepthSpan;
    float Factor = 0.0;
    for (int Tap = 0; Tap < 4; ++Tap) {
        vec2 UV = Coordinate.xy + ShadowPoisson[Tap] * world.ShadowControls.w * 2.0;
        Factor += texture(CascadeShadowMap, vec4(UV, float(Index), CompareDepth));
    }
    return Factor * 0.25;
}

float EvaluateDirectionalCascades(vec3 Normal, float NormalDotLight) {
    int Count = clamp(int(world.ShadowControls.x + 0.5), 0, 4);
    if (Count == 0) return 1.0;
    float Depth = -(world.ViewMatrix * WorldPosition).z;
    int Index = Count;
    for (int Candidate = 0; Candidate < Count; ++Candidate) {
        if (Depth <= world.CascadeShadowSplits[Candidate].y) {
            Index = Candidate;
            break;
        }
    }
    if (Index == Count) return 1.0;
    float Current = SampleCascadeShadow(Index, Normal, NormalDotLight);
    if (Index + 1 == Count) return Current;
    vec4 Split = world.CascadeShadowSplits[Index];
    if (Depth < Split.z) return Current;
    float Width = max(Split.y - Split.z, 0.0001);
    float Weight = clamp((Depth - Split.z) / Width, 0.0, 1.0);
    return mix(Current, SampleCascadeShadow(Index + 1, Normal, NormalDotLight), Weight);
}

float EvaluateSpotShadow(float LayerPlusOne, vec3 Normal, float NormalDotLight) {
    if (LayerPlusOne < 0.5 || world.SpotShadowControls.x < 0.5) return 1.0;
    int Index = int(LayerPlusOne + 0.5) - 1;
    if (Index < 0 || Index >= 8) return 1.0;
    vec3 Relative = WorldPosition.xyz + Normal * world.SpotShadowControls.z -
        world.SpotShadowOrigins[Index].xyz;
    vec4 Projected = world.SpotShadowMatrices[Index] * vec4(Relative, 1.0);
    if (Projected.w <= 0.0) return 1.0;
    vec3 Coordinate = Projected.xyz / Projected.w;
    if (!ShadowCoordinateInside(Coordinate)) return 1.0;
    float Bias = world.SpotShadowControls.y * (1.0 + 2.0 * (1.0 - NormalDotLight));
    float Factor = 0.0;
    for (int Tap = 0; Tap < 4; ++Tap) {
        vec2 UV = Coordinate.xy + ShadowPoisson[Tap] * world.SpotShadowControls.w * 2.0;
        Factor += texture(SpotShadowMap, vec4(UV, float(Index), Coordinate.z - Bias));
    }
    return Factor * 0.25;
}
#endif
