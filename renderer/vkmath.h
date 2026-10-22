// Minimal vector/matrix helpers. Matrices use USD's row-vector convention
// and memory order (p' = p * M, A * B applies A first), which is what the
// shaders expect when they load them as GLSL column-major mat4.
#pragma once

#include <algorithm>
#include <cmath>

namespace vkr {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(Vec3 a)
{
    const float l = Length(a);
    return l > 0 ? a * (1.0f / l) : a;
}

struct Mat4 {
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
};

inline Mat4 operator*(const Mat4 &a, const Mat4 &b)
{
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] +
                        a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        }
    }
    return r;
}

// Point transform with perspective divide.
inline Vec3 TransformPoint(const Mat4 &m, Vec3 p)
{
    const float x = p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0];
    const float y = p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1];
    const float z = p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2];
    const float w = p.x * m.m[0][3] + p.y * m.m[1][3] + p.z * m.m[2][3] + m.m[3][3];
    return {x / w, y / w, z / w};
}

// Rotation-only view looking down `forward` (camera looks down -Z).
inline Mat4 LookAlong(Vec3 forward)
{
    const Vec3 f = Normalize(forward);
    const Vec3 up = std::abs(f.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    const Vec3 r = Normalize(Cross(f, up));
    const Vec3 u = Cross(r, f);
    Mat4 v;
    v.m[0][0] = r.x; v.m[0][1] = u.x; v.m[0][2] = -f.x;
    v.m[1][0] = r.y; v.m[1][1] = u.y; v.m[1][2] = -f.y;
    v.m[2][0] = r.z; v.m[2][1] = u.z; v.m[2][2] = -f.z;
    return v;
}

// GL-style orthographic projection (view z in [-n, -f] -> NDC z in [-1, 1]).
inline Mat4 Ortho(float l, float r, float b, float t, float n, float f)
{
    Mat4 p;
    p.m[0][0] = 2 / (r - l);
    p.m[1][1] = 2 / (t - b);
    p.m[2][2] = -2 / (f - n);
    p.m[3][0] = -(r + l) / (r - l);
    p.m[3][1] = -(t + b) / (t - b);
    p.m[3][2] = -(f + n) / (f - n);
    return p;
}

} // namespace vkr
