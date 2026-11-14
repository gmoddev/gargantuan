#ifndef GARGANTUAN_INSTANCE_RECORD_GLSL
#define GARGANTUAN_INSTANCE_RECORD_GLSL

// Mirror SDLInstanceRecord exactly. Slot 0 holds the identity skin palette;
// records and visible indices occupy slots 1 and 2.
struct InstanceRecord {
    mat4 ModelMatrix;
    mat4 NormalMatrix;
    vec4 Color;
    vec4 MaterialValues;
    vec4 MaterialFlags;
    vec4 EmissiveOcclusion;
    vec4 MaterialControls;
    vec4 ColorSamplerModes;
};

layout(std430, set = 0, binding = 1) readonly buffer InstanceRecordBuffer {
    InstanceRecord Records[];
} InstanceData;
layout(std430, set = 0, binding = 2) readonly buffer VisibleInstanceIndexBuffer {
    uint Slots[];
} VisibleInstances;

#endif
