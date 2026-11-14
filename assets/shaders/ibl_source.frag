#version 450

layout(location = 0) out vec4 OutputColor;
layout(set = 2, binding = 0) uniform sampler2D SkyFace;
layout(set = 3, binding = 0) uniform SourceParameters {
    vec4 Dimensions;
} Source;

vec3 DecodeSrgb(vec3 Value) {
    return mix(pow((Value + 0.055) / 1.055, vec3(2.4)), Value / 12.92,
        lessThanEqual(Value, vec3(0.04045)));
}

vec3 FetchRadiance(ivec2 Position, ivec2 Size) {
    return DecodeSrgb(texelFetch(SkyFace, clamp(Position, ivec2(0), Size - 1), 0).rgb);
}

void main() {
    ivec2 Size = textureSize(SkyFace, 0);
    vec2 Scale = vec2(Size) / Source.Dimensions.x;
    vec3 Color;
    if (Scale.x > 1.0) {
        // A bounded box filter conserves energy when a large authored image is
        // reduced to the source cube. Decode every participating texel first.
        vec2 Start = floor(gl_FragCoord.xy) * Scale;
        vec2 End = min(Start + Scale, vec2(Size));
        ivec2 First = ivec2(floor(Start));
        ivec2 Last = ivec2(ceil(End));
        Color = vec3(0.0);
        for (int Y = First.y; Y < Last.y; ++Y) {
            for (int X = First.x; X < Last.x; ++X) {
                vec2 Cell = vec2(X, Y);
                vec2 Coverage = max(vec2(0.0), min(End, Cell + 1.0) - max(Start, Cell));
                Color += FetchRadiance(ivec2(X, Y), Size) * (Coverage.x * Coverage.y);
            }
        }
        Color /= (End.x - Start.x) * (End.y - Start.y);
    } else {
        vec2 Position = gl_FragCoord.xy * Scale - 0.5;
        ivec2 Base = ivec2(floor(Position));
        vec2 F = fract(Position);
        Color = mix(mix(FetchRadiance(Base, Size), FetchRadiance(Base + ivec2(1, 0), Size), F.x),
            mix(FetchRadiance(Base + ivec2(0, 1), Size), FetchRadiance(Base + 1, Size), F.x), F.y);
    }
    OutputColor = vec4(Color, 1.0);
}
