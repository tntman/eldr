// Skinning a vertex by its joints, shared by mesh.vert and ray tracing's
// rt_skin.comp. Include after scene.glsl, a Joint(index, previous) that
// returns a joint matrix (this frame's or last frame's), and the bools
// kWideSkin (eight influences) and kDualQuaternion (mesh.vert:
// specialization constants; rt_skin.comp: set per instance).

// A joint's rotation (Gram-Schmidt on its columns) as a quaternion (xyz, w),
// and the scale/shear it leaves: m = rotation * scale.
vec4 Rotation(mat3 m, out mat3 scale)
{
    vec3 x = normalize(m[0]);
    vec3 y = normalize(m[1] - dot(m[1], x) * x);
    mat3 r = mat3(x, y, cross(x, y));
    scale = transpose(r) * m;
    // Shepperd: from the largest of w, x, y, z (r[column][row]).
    float t = r[0][0] + r[1][1] + r[2][2];
    if (t > 0.0) {
        float s = 0.5 / sqrt(t + 1.0);
        return vec4((r[1][2] - r[2][1]) * s, (r[2][0] - r[0][2]) * s, (r[0][1] - r[1][0]) * s, 0.25 / s);
    }
    if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        float s = 0.5 / sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]);
        return vec4(0.25 / s, (r[1][0] + r[0][1]) * s, (r[2][0] + r[0][2]) * s, (r[1][2] - r[2][1]) * s);
    }
    if (r[1][1] > r[2][2]) {
        float s = 0.5 / sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]);
        return vec4((r[1][0] + r[0][1]) * s, 0.25 / s, (r[2][1] + r[1][2]) * s, (r[2][0] - r[0][2]) * s);
    }
    float s = 0.5 / sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]);
    return vec4((r[2][0] + r[0][2]) * s, (r[2][1] + r[1][2]) * s, 0.25 / s, (r[0][1] - r[1][0]) * s);
}

mat3 RotationMatrix(vec4 q)
{
    vec3 q2 = 2.0 * q.xyz;
    float xx = q.x * q2.x, yy = q.y * q2.y, zz = q.z * q2.z;
    float xy = q.x * q2.y, xz = q.x * q2.z, yz = q.y * q2.z;
    float wx = q.w * q2.x, wy = q.w * q2.y, wz = q.w * q2.z;
    return mat3(1.0 - yy - zz, xy + wz, xz - wy, xy - wz, 1.0 - xx - zz, yz + wx, xz + wy, yz - wx, 1.0 - xx - yy);
}

// The dual-quaternion blend of the vertex's joints as a matrix: their
// rotations and translations flipped onto the first (strongest)
// influence's hemisphere, summed, normalized; then the blended scale.
mat4 DualQuaternionSkin(uvec4 s, vec4 w, uvec4 s2, uint base, bool previous)
{
    uint j[8] = uint[8](s.x & 0xFFFFu, s.x >> 16, s.y & 0xFFFFu, s.y >> 16,
                        s2.x & 0xFFFFu, s2.x >> 16, s2.y & 0xFFFFu, s2.y >> 16);
    vec4 w2 = kWideSkin ? vec4(unpackUnorm2x16(s2.z), unpackUnorm2x16(s2.w)) : vec4(0.0);
    float weight[8] = float[8](w.x, w.y, w.z, w.w, w2.x, w2.y, w2.z, w2.w);
    vec4 real = vec4(0.0), dual = vec4(0.0), pivot = vec4(0.0);
    mat3 scale = mat3(0.0);
    float sum = 0.0;
    for (int k = 0; k < (kWideSkin ? 8 : 4); ++k) {
        mat4 m = Joint(base + j[k], previous);
        mat3 sk;
        vec4 q = Rotation(mat3(m), sk);
        pivot = k == 0 ? q : pivot;
        float wq = dot(q, pivot) < 0.0 ? -weight[k] : weight[k];
        vec3 t = m[3].xyz;
        real += wq * q;
        dual += wq * 0.5 * vec4(q.w * t + cross(t, q.xyz), -dot(t, q.xyz));  // (t, 0) * q / 2
        scale += weight[k] * sk;
        sum += weight[k];
    }
    float len = max(length(real), 1e-6);
    real /= len;
    dual /= len;
    vec3 t = 2.0 * (real.w * dual.xyz - dual.w * real.xyz + cross(real.xyz, dual.xyz));  // 2 dual * conj(real)
    mat4 m = mat4(RotationMatrix(real) * (scale / max(sum, 1e-6)));
    m[3] = vec4(t, 1.0);
    return m;
}

// Linear blend skinning: the weighted sum of the vertex's joint matrices
// (16-bit joints in s.xy, unorm16 weights w; s2: influences 4..7).
mat4 SkinMatrix(uvec4 s, vec4 w, uvec4 s2, uint base, bool previous)
{
    if (kDualQuaternion) {
        return DualQuaternionSkin(s, w, s2, base, previous);
    }
    mat4 m = w.x * Joint(base + (s.x & 0xFFFFu), previous) + w.y * Joint(base + (s.x >> 16), previous) +
             w.z * Joint(base + (s.y & 0xFFFFu), previous) + w.w * Joint(base + (s.y >> 16), previous);
    float sum = w.x + w.y + w.z + w.w;
    if (kWideSkin) {
        vec4 w2 = vec4(unpackUnorm2x16(s2.z), unpackUnorm2x16(s2.w));
        m += w2.x * Joint(base + (s2.x & 0xFFFFu), previous) + w2.y * Joint(base + (s2.x >> 16), previous) +
             w2.z * Joint(base + (s2.y & 0xFFFFu), previous) + w2.w * Joint(base + (s2.y >> 16), previous);
        sum += w2.x + w2.y + w2.z + w2.w;
    }
    return m / max(sum, 1e-6);
}
