// MaterialX's procedural noise (noise2d/3d, fractal2d/3d, cellnoise2d/3d,
// worleynoise2d/3d) for material programs (program.glsl), ported from
// MaterialX's genglsl library (libraries/stdlib/genglsl/lib/mx_noise.glsl,
// as USD 26.08 ships it) so results match Storm, which renders MaterialX
// with it. The mx_ functions are that file's (those HdVk uses); the Mx
// ones below compute what its perlin noise and node implementations
// (mx_noise2d_vector4.glsl, ...) do.
//
// The library is a modified version of the noise library found in Open
// Shading Language (github.com/imageworks/OpenShadingLanguage,
// src/include/OSL/oslnoise.h). Its copyright notice:
// ------------------------------------------------------------------------
// Copyright (c) 2009-2010 Sony Pictures Imageworks Inc., et al.
// All Rights Reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
// * Neither the name of Sony Pictures Imageworks nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// ------------------------------------------------------------------------

#extension GL_EXT_control_flow_attributes : require

float mx_select(bool b, float t, float f)
{
    return b ? t : f;
}

float mx_negate_if(float val, bool b)
{
    return b ? -val : val;
}

int mx_floor(float x)
{
    return int(floor(x));
}

float mx_floorfrac(float x, out int i)
{
    i = mx_floor(x);
    return x - float(i);
}

float mx_gradient_float(uint hash, float x, float y)
{
    uint h = hash & 7u;
    float u = mx_select(h<4u, x, y);
    float v = 2.0 * mx_select(h<4u, y, x);
    return mx_negate_if(u, bool(h&1u)) + mx_negate_if(v, bool(h&2u));
}

float mx_gradient_float(uint hash, float x, float y, float z)
{
    uint h = hash & 15u;
    float u = mx_select(h<8u, x, y);
    float v = mx_select(h<4u, y, mx_select((h==12u)||(h==14u), x, z));
    return mx_negate_if(u, bool(h&1u)) + mx_negate_if(v, bool(h&2u));
}

uint mx_rotl32(uint x, int k)
{
    return (x<<k) | (x>>(32-k));
}

void mx_bjmix(inout uint a, inout uint b, inout uint c)
{
    a -= c; a ^= mx_rotl32(c, 4); c += b;
    b -= a; b ^= mx_rotl32(a, 6); a += c;
    c -= b; c ^= mx_rotl32(b, 8); b += a;
    a -= c; a ^= mx_rotl32(c,16); c += b;
    b -= a; b ^= mx_rotl32(a,19); a += c;
    c -= b; c ^= mx_rotl32(b, 4); b += a;
}

uint mx_bjfinal(uint a, uint b, uint c)
{
    c ^= b; c -= mx_rotl32(b,14);
    a ^= c; a -= mx_rotl32(c,11);
    b ^= a; b -= mx_rotl32(a,25);
    c ^= b; c -= mx_rotl32(b,16);
    a ^= c; a -= mx_rotl32(c,4);
    b ^= a; b -= mx_rotl32(a,14);
    c ^= b; c -= mx_rotl32(b,24);
    return c;
}

float mx_bits_to_01(uint bits)
{
    return float(bits) / float(uint(0xffffffff));
}

float mx_fade(float t)
{
   return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

uint mx_hash_int(int x, int y)
{
    uint len = 2u;
    uint a, b, c;
    a = b = c = uint(0xdeadbeef) + (len << 2u) + 13u;
    a += uint(x);
    b += uint(y);
    return mx_bjfinal(a, b, c);
}

uint mx_hash_int(int x, int y, int z)
{
    uint len = 3u;
    uint a, b, c;
    a = b = c = uint(0xdeadbeef) + (len << 2u) + 13u;
    a += uint(x);
    b += uint(y);
    c += uint(z);
    return mx_bjfinal(a, b, c);
}

float mx_cell_noise_float(vec2 p)
{
    int ix = mx_floor(p.x);
    int iy = mx_floor(p.y);
    return mx_bits_to_01(mx_hash_int(ix, iy));
}

float mx_cell_noise_float(vec3 p)
{
    int ix = mx_floor(p.x);
    int iy = mx_floor(p.y);
    int iz = mx_floor(p.z);
    return mx_bits_to_01(mx_hash_int(ix, iy, iz));
}

vec3 mx_cell_noise_vec3(vec2 p)
{
    int ix = mx_floor(p.x);
    int iy = mx_floor(p.y);
    return vec3(
            mx_bits_to_01(mx_hash_int(ix, iy, 0)),
            mx_bits_to_01(mx_hash_int(ix, iy, 1)),
            mx_bits_to_01(mx_hash_int(ix, iy, 2))
    );
}

vec3 mx_cell_noise_vec3(vec3 p)
{
    int ix = mx_floor(p.x);
    int iy = mx_floor(p.y);
    int iz = mx_floor(p.z);
    uint a, b, c;
    a = b = c = uint(0xdeadbeef) + (4u << 2u) + 13u;
    a += uint(ix);
    b += uint(iy);
    c += uint(iz);
    mx_bjmix(a, b, c);
    return vec3(
            mx_bits_to_01(mx_bjfinal(a,      b, c)),
            mx_bits_to_01(mx_bjfinal(a + 1u, b, c)),
            mx_bits_to_01(mx_bjfinal(a + 2u, b, c))
    );
}

vec2 mx_worley_cell_position(int x, int y, int xoff, int yoff, float jitter)
{
    vec3  tmp = mx_cell_noise_vec3(vec2(x+xoff, y+yoff));
    vec2  off = vec2(tmp.x, tmp.y);

    off -= 0.5f;
    off *= jitter;
    off += 0.5f;

    return vec2(float(x), float(y)) + off;
}

vec3 mx_worley_cell_position(int x, int y, int z, int xoff, int yoff, int zoff, float jitter)
{
    vec3  off = mx_cell_noise_vec3(vec3(x+xoff, y+yoff, z+zoff));

    off -= 0.5f;
    off *= jitter;
    off += 0.5f;

    return vec3(float(x), float(y), float(z)) + off;
}

// Below: HdVk's entry points, `dims` (2: p.xy, 3: p.xyz) and the output's
// components (1 float, splat; 2 vector2; 3 vector3/color3; 4 vector4/color4).
// Loops where mx_noise.glsl unrolls, and one call site each, keep the
// interpreter small: unrolled, the noise made every material program's
// pixels ~15% slower, noise or not.

// mx_perlin_noise_float (vec: mx_perlin_noise_vec3, a hash byte per
// channel) at p: the corners' gradients weighted by the fade curves, which
// is mx_bilerp's and mx_trilerp's result up to float rounding.
vec3 MxPerlin(vec3 p, uint dims, bool vec)
{
    ivec3 cell = ivec3(0);
    vec3 f = vec3(mx_floorfrac(p.x, cell.x), mx_floorfrac(p.y, cell.y), dims == 3u ? mx_floorfrac(p.z, cell.z) : 0.0);
    vec3 fade = vec3(mx_fade(f.x), mx_fade(f.y), mx_fade(f.z));
    vec3 result = vec3(0.0);
    [[dont_unroll]] for (uint c = 0u; c < (dims == 2u ? 4u : 8u); ++c) {
        bvec3 far = bvec3(uvec3(c, c >> 1, c >> 2) & 1u);
        ivec3 corner = cell + ivec3(far);
        vec3 d = f - vec3(far);
        vec3 w = mix(1.0 - fade, fade, far);
        uint h = dims == 2u ? mx_hash_int(corner.x, corner.y) : mx_hash_int(corner.x, corner.y, corner.z);
        uvec3 b = vec ? uvec3(h, h >> 8, h >> 16) & 0xFFu : uvec3(h);
        vec3 g = dims == 2u ? vec3(mx_gradient_float(b.x, d.x, d.y), mx_gradient_float(b.y, d.x, d.y),
                                   mx_gradient_float(b.z, d.x, d.y))
                            : vec3(mx_gradient_float(b.x, d.x, d.y, d.z), mx_gradient_float(b.y, d.x, d.y, d.z),
                                   mx_gradient_float(b.z, d.x, d.y, d.z));
        result += (w.x * w.y * (dims == 3u ? w.z : 1.0)) * g;
    }
    return result * (dims == 2u ? 0.6616 : 0.9820);
}

// noise2d/3d (one octave, not `fractal`) and fractal2d/3d before amplitude
// and pivot: a float, or a vec3 for vector3/color3, vector4 and noise's
// vector2 (its xy); a second float at an offset is fractal's vector2 y and
// vector4's w (mx_noise*_*.glsl, mx_fractal*_noise_*).
vec4 MxLayered(vec3 p, uint dims, uint components, bool fractal, int octaves, float lacunarity, float diminish)
{
    bool vec = components >= 3u || (!fractal && components == 2u);
    uint values = components == 4u || (fractal && components == 2u) ? 2u : 1u;
    vec3 q = p + (fractal ? vec3(19, 193, 17) : vec3(19, 73, 29));  // 2d: the xy
    vec4 result = vec4(0.0);
    float amplitude = 1.0;
    [[dont_unroll]] for (int i = 0; i < octaves; ++i) {
        [[dont_unroll]] for (uint v = 0u; v < values; ++v) {
            vec3 n = amplitude * MxPerlin(v == 0u ? p : q, dims, v == 0u && vec);
            if (v == 1u) {
                result[components == 2u ? 1 : 3] += n.x;
            } else if (vec) {
                result.xyz += n;
            } else {
                result.x += n.x;
            }
        }
        amplitude *= diminish;
        p *= lacunarity;
        q *= lacunarity;
    }
    return result;
}

float MxCellNoise(vec3 p, uint dims)
{
    return dims == 2u ? mx_cell_noise_float(p.xy) : mx_cell_noise_float(p);
}

// worleynoise2d/3d with the euclidean metric (all MaterialX uses): the
// three nearest feature distances, or with style 1 (solid) the nearest
// cell's cell noise; one loop for every output width, as the float,
// vector2 and vector3 variants find the same nearest cell. `metric` (Karma's
// kma_voronoinoise3d): 0 euclidean, 1 squared, 2 manhattan, 3 chebyshev.
vec4 MxWorley(vec3 p, uint dims, uint components, float jitter, int style, uint metric)
{
    int X, Y, Z = 0;
    vec3 localpos = vec3(mx_floorfrac(p.x, X), mx_floorfrac(p.y, Y), dims == 3u ? mx_floorfrac(p.z, Z) : 0.0);
    vec3 sqdist = vec3(1e6f, 1e6f, 1e6f);
    vec3 minpos = vec3(0, 0, 0);
    int zRange = dims == 3u ? 1 : 0;
    [[dont_unroll]] for (int x = -1; x <= 1; ++x) {
        [[dont_unroll]] for (int y = -1; y <= 1; ++y) {
            [[dont_unroll]] for (int z = -zRange; z <= zRange; ++z) {
                vec3 cellpos = dims == 3u ? mx_worley_cell_position(x, y, z, X, Y, Z, jitter) - localpos
                                          : vec3(mx_worley_cell_position(x, y, X, Y, jitter) - localpos.xy, 0.0);
                vec3 d = abs(cellpos);
                float dist = metric == 2u ? d.x + d.y + d.z : metric == 3u ? max(d.x, max(d.y, d.z)) : dot(d, d);
                if (dist < sqdist.x) {
                    sqdist.z = sqdist.y;
                    sqdist.y = sqdist.x;
                    sqdist.x = dist;
                    minpos = cellpos;
                } else if (dist < sqdist.y) {
                    sqdist.z = sqdist.y;
                    sqdist.y = dist;
                } else if (dist < sqdist.z) {
                    sqdist.z = dist;
                }
            }
        }
    }
    if (style == 1) {
        if (components == 1u) {
            return vec4(dims == 2u ? mx_cell_noise_float(minpos.xy + p.xy) : mx_cell_noise_float(minpos + p));
        }
        return vec4(dims == 2u ? mx_cell_noise_vec3(minpos.xy + p.xy) : mx_cell_noise_vec3(minpos + p), 0.0);
    }
    vec3 dist = metric == 0u ? sqrt(sqdist) : sqdist;  // the others are distances already (1: squared)
    return components == 1u ? vec4(dist.x) : vec4(dist, 0.0);
}
