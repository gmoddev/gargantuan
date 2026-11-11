#ifndef GARGANTUAN_INSTANCE_RECORD_GLSL
#define GARGANTUAN_INSTANCE_RECORD_GLSL

// Mirror SDLInstanceRecord exactly. This rigid variant has no skin palette;
// its two vertex storage buffers occupy consecutive slots 0 and 1.
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

layout(std430, set = 0, binding = 0) readonly buffer InstanceRecordBuffer {
    InstanceRecord Records[];
} InstanceData;
layout(std430, set = 0, binding = 1) readonly buffer VisibleInstanceIndexBuffer {
    uint Slots[];
} VisibleInstances;

#endif
