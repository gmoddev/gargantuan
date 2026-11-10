#ifndef GARGANTUAN_SCENE_COLOR
#define GARGANTUAN_SCENE_COLOR

vec3 SrgbToLinear(vec3 Encoded) {
    return mix(pow((Encoded + 0.055) / 1.055, vec3(2.4)), Encoded / 12.92,
        lessThanEqual(Encoded, vec3(0.04045)));
}

vec3 LinearToSrgb(vec3 Linear) {
    return mix(1.055 * pow(Linear, vec3(1.0 / 2.4)) - 0.055, Linear * 12.92,
        lessThanEqual(Linear, vec3(0.0031308)));
}

vec3 FiniteSceneColor(vec3 Linear) {
    return clamp(mix(Linear, vec3(0.0), isnan(Linear)), 0.0, 65504.0);
}

vec3 ToneMapScene(vec3 Linear, float Exposure) {
    vec3 Color = FiniteSceneColor(Linear) * Exposure;
    return clamp((Color * (2.51 * Color + 0.03)) /
        (Color * (2.43 * Color + 0.59) + 0.14), 0.0, 1.0);
}

#endif
