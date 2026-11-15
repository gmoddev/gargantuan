#version 450

layout(location = 0) in vec2 FragmentUV;
layout(set = 2, binding = 0) uniform sampler2D BaseColorTexture;
layout(set = 3, binding = 0) uniform MaskUniforms {
    vec4 AlphaFactorCutoff;
};

void main()
{
    if (AlphaFactorCutoff.z > 0.5 && texture(BaseColorTexture, FragmentUV).a * AlphaFactorCutoff.x < AlphaFactorCutoff.y)
        discard;
}
