#ifndef GARGANTUAN_PROBE_LIGHTING
#define GARGANTUAN_PROBE_LIGHTING

layout(set = 2, binding = 11) uniform samplerCubeArray ProbeIrradiance;
layout(set = 2, binding = 12) uniform samplerCubeArray ProbeReflections;
struct ReflectionProbe {
    vec4 PositionIntensity;
    vec4 InverseRotation[3];
    vec4 HalfSizeBlend;
    vec4 CaptureOffsetBox;
    vec4 PriorityLayer;
};
layout(std430, set = 2, binding = 15) readonly buffer ReflectionProbeBuffer {
    ReflectionProbe ReflectionProbes[];
};

vec3 ProbeLocalDirection(ReflectionProbe Probe, vec3 Direction) {
    return vec3(dot(Probe.InverseRotation[0].xyz, Direction),
        dot(Probe.InverseRotation[1].xyz, Direction), dot(Probe.InverseRotation[2].xyz, Direction));
}

float ProbeWeight(ReflectionProbe Probe, vec3 Position) {
    vec3 Local = ProbeLocalDirection(Probe, Position - Probe.PositionIntensity.xyz);
    vec3 Margin = Probe.HalfSizeBlend.xyz - abs(Local);
    float Distance = min(Margin.x, min(Margin.y, Margin.z));
    if (Distance < 0.0) return 0.0;
    if (Probe.HalfSizeBlend.w == 0.0) return 1.0;
    float T = clamp(Distance / Probe.HalfSizeBlend.w, 0.0, 1.0);
    return T * T * (3.0 - 2.0 * T);
}

vec3 ProbeReflectionDirection(ReflectionProbe Probe, vec3 Position, vec3 Ray) {
    vec3 LocalRay = ProbeLocalDirection(Probe, Ray);
    if (Probe.CaptureOffsetBox.w < 0.5) return LocalRay;
    vec3 Local = ProbeLocalDirection(Probe, Position - Probe.PositionIntensity.xyz);
    float Exit = 1e30;
    for (int Axis = 0; Axis < 3; ++Axis) {
        if (abs(LocalRay[Axis]) <= 1e-12) continue;
        float Boundary = LocalRay[Axis] > 0.0 ? Probe.HalfSizeBlend[Axis] : -Probe.HalfSizeBlend[Axis];
        Exit = min(Exit, (Boundary - Local[Axis]) / LocalRay[Axis]);
    }
    vec3 Corrected = Local + LocalRay * Exit - Probe.CaptureOffsetBox.xyz;
    return SafeDirection(Corrected, LocalRay);
}

vec3 EvaluateSpatialEnvironment(vec3 Base, vec3 Normal, vec3 View, float Metallic, float Roughness) {
    vec3 Ambient = Base * (1.0 - Metallic) * world.AmbientExposure.rgb;
    vec3 F0 = mix(vec3(0.04), Base, Metallic);
    float NoV = clamp(dot(Normal, View), 0.0, 1.0);
    vec3 GrazingFresnel = max(vec3(1.0 - Roughness), F0);
    vec3 Fresnel = F0 + (GrazingFresnel - F0) * pow(1.0 - NoV, 5.0);
    vec3 DiffuseFactor = Base * (1.0 - Fresnel) * (1.0 - Metallic);
    vec3 ReflectionRay = reflect(-View, Normal);
    vec2 Brdf = texture(EnvironmentBrdf, vec2(NoV, Roughness)).rg;
    vec3 SpecularFactor = F0 * Brdf.x + Brdf.y;
    vec3 Global = vec3(0.0);
    if (world.EnvironmentLighting.x > 0.5 && world.EnvironmentLighting.z > 0.0) {
        vec3 Diffuse = texture(EnvironmentIrradiance, EnvironmentSampleDirection(Normal, world.EnvironmentRotationTone.xy)).rgb;
        vec3 Radiance = textureLod(EnvironmentReflections,
            EnvironmentSampleDirection(ReflectionRay, world.EnvironmentRotationTone.xy), Roughness * world.EnvironmentLighting.y).rgb;
        Global = world.EnvironmentLighting.z * (Diffuse * DiffuseFactor + Radiance * SpecularFactor);
    }
    int Selected[2] = int[](0, 0);
    vec2 Weights = vec2(0.0);
    vec2 Priorities = vec2(-2048.0);
    int Count = clamp(int(world.ProbeLighting.x + 0.5), 0, 12);
    for (int Candidate = 0; Candidate < Count; ++Candidate) {
        ReflectionProbe Probe = ReflectionProbes[Candidate];
        float Weight = ProbeWeight(Probe, WorldPosition.xyz);
        if (Weight <= 0.0) continue;
        float Priority = Probe.PriorityLayer.x;
        if (Priority > Priorities.x || (Priority == Priorities.x && Weight > Weights.x)) {
            Selected[1] = Selected[0]; Weights.y = Weights.x; Priorities.y = Priorities.x;
            Selected[0] = Candidate; Weights.x = Weight; Priorities.x = Priority;
        } else if (Priority > Priorities.y || (Priority == Priorities.y && Weight > Weights.y)) {
            Selected[1] = Candidate; Weights.y = Weight; Priorities.y = Priority;
        }
    }
    if (Priorities.x != Priorities.y) Weights.y *= 1.0 - Weights.x;
    float Sum = Weights.x + Weights.y;
    if (Sum > 1.0) Weights /= Sum;
    vec3 Indirect = max(0.0, 1.0 - Weights.x - Weights.y) * Global;
    for (int Index = 0; Index < 2; ++Index) {
        if (Weights[Index] <= 0.0) continue;
        ReflectionProbe Probe = ReflectionProbes[Selected[Index]];
        float Layer = Probe.PriorityLayer.y;
        vec3 Diffuse = texture(ProbeIrradiance, vec4(ProbeLocalDirection(Probe, Normal), Layer)).rgb;
        vec3 Radiance = textureLod(ProbeReflections,
            vec4(ProbeReflectionDirection(Probe, WorldPosition.xyz, ReflectionRay), Layer), Roughness * 6.0).rgb;
        Indirect += Weights[Index] * Probe.PositionIntensity.w * (Diffuse * DiffuseFactor + Radiance * SpecularFactor);
    }
    return Ambient + Indirect;
}
#endif
