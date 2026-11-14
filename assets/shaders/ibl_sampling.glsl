const float IblPi = 3.14159265358979323846;

vec3 IblCubeDirection(int Face, vec2 Uv) {
    vec2 P = Uv * 2.0 - 1.0;
    // Inverse of the published Sky UV table and native cube face addressing.
    if (Face == 0) return normalize(vec3(1.0, -P.y, -P.x));
    if (Face == 1) return normalize(vec3(-1.0, -P.y, P.x));
    if (Face == 2) return normalize(vec3(P.x, 1.0, P.y));
    if (Face == 3) return normalize(vec3(P.x, -1.0, -P.y));
    if (Face == 4) return normalize(vec3(P.x, -P.y, 1.0));
    return normalize(vec3(-P.x, -P.y, -1.0));
}

vec2 IblHammersley(uint Index, uint Count) {
    return vec2(float(Index) / float(Count), float(bitfieldReverse(Index)) * 2.3283064365386963e-10);
}

vec3 IblToWorld(vec3 Sample, vec3 Normal) {
    vec3 Axis = abs(Normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 Tangent = normalize(cross(Axis, Normal));
    return Tangent * Sample.x + cross(Normal, Tangent) * Sample.y + Normal * Sample.z;
}

vec3 IblGgxHalfVector(vec2 Xi, float AlphaSquared) {
    float Phi = 2.0 * IblPi * Xi.x;
    float CosTheta = sqrt((1.0 - Xi.y) / (1.0 + (AlphaSquared - 1.0) * Xi.y));
    float SinTheta = sqrt(max(0.0, 1.0 - CosTheta * CosTheta));
    return vec3(cos(Phi) * SinTheta, sin(Phi) * SinTheta, CosTheta);
}
