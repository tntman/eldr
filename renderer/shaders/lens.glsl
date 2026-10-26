// Shared by the lens passes (lens.cpp): the push block, camera distance,
// circle of confusion and motion.

layout(push_constant) uniform LensParams {
    mat4 reprojection;  // NDC -> last frame's clip space (row-vector order)
    // Inverse projection's z and w columns over NDC z (row-vector order):
    // camera distance = -(x * z + y) / (z * z + w).
    vec4 depthToDistance;
    // Circle of confusion radius in pixels = x * |distance - y| / distance,
    // at most z (x 0: no depth of field); w: frame.
    vec4 lens;
    // Shutter in frames (0: no motion blur), longest half blur in pixels,
    // 1/width, 1/height.
    vec4 motion;
    // Residual motion blur: the sample's stratum of the shutter, from x to
    // y strata around the sample's time (motion_residual.frag).
    vec4 window;
} pc;

float Distance(float depth)
{
    float z = depth * 2.0 - 1.0;
    vec4 c = pc.depthToDistance;
    return -(c.x * z + c.y) / (c.z * z + c.w);
}

float Circle(float distance)
{
    return min(pc.lens.x * abs(distance - pc.lens.y) / max(distance, 1e-6), pc.lens.z);
}

// Half pixel p's blur over the shutter, in pixels: its motion since the
// last frame (the camera's from reprojecting the depth, plus its object's
// motion vector, as in taa.frag).
vec2 Motion(ivec2 p, float depth, vec2 objectMotion)
{
    vec2 uv = (vec2(p) + 0.5) * pc.motion.zw;
    vec4 prev = pc.reprojection * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    if (pc.motion.x <= 0.0 || prev.w <= 0.0) {
        return vec2(0.0);
    }
    vec2 m = (uv - prev.xy / prev.w * 0.5 - 0.5 - objectMotion) / pc.motion.zw * (0.5 * pc.motion.x);
    float l = length(m);
    return l > pc.motion.y ? m * (pc.motion.y / l) : m;
}

// Interleaved gradient noise (Jimenez 2014) in [0, 1), shifted per frame.
float Noise(vec2 pixel)
{
    vec2 q = pixel + 5.588238 * pc.lens.w;
    return fract(52.9829189 * fract(dot(q, vec2(0.06711056, 0.00583715))));
}

// The longest motion (xy) and the largest near-field circle (z) of the 3x3
// tiles (lens_tiles.comp) around pixel p.
vec4 NeighborMax(sampler2D tiles, ivec2 p)
{
    ivec2 t = p / 32, last = textureSize(tiles, 0) - 1;
    vec4 r = vec4(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec4 s = texelFetch(tiles, clamp(t + ivec2(x, y), ivec2(0), last), 0);
            r = vec4(dot(s.xy, s.xy) > dot(r.xy, r.xy) ? s.xy : r.xy, max(r.z, s.z), 0.0);
        }
    }
    return r;
}
