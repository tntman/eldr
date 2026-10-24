#include "ibl.h"

#include "vkmath.h"
#include "scene.h"

#include <algorithm>
#include <cmath>
#include <execution>
#include <numeric>

namespace vkr {

namespace {

constexpr float kPi = 3.14159265358979f;

float HalfToFloat(uint16_t h)
{
    const int exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    const float sign = (h & 0x8000) ? -1.0f : 1.0f;
    if (exponent == 0) {
        return sign * std::ldexp(float(mantissa), -24);
    }
    if (exponent == 31) {
        return mantissa ? 0.0f : sign * 65504.0f;  // NaN -> 0, Inf -> max half
    }
    return sign * std::ldexp(float(mantissa | 0x400), exponent - 25);
}

float SrgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

Vec3 Texel(const Texture &t, uint32_t x, uint32_t y)
{
    const uint32_t n = Channels(t.format);
    const size_t i = (size_t(y) * t.width + x) * n;
    // Clamp so a stray Inf/NaN texel can't poison every filtered texel.
    auto clean = [](float v) { return std::isfinite(v) ? std::max(v, 0.0f) : 0.0f; };
    float c[3] = {};
    for (uint32_t k = 0; k < std::min(n, 3u); ++k) {
        const size_t j = i + k;
        switch (t.format) {
        case Texture::Format::R8:
        case Texture::Format::RG8:
        case Texture::Format::RGBA8:      c[k] = t.pixels[j] / 255.0f; break;
        case Texture::Format::RGBA8_SRGB: c[k] = SrgbToLinear(t.pixels[j] / 255.0f); break;
        case Texture::Format::R16:
        case Texture::Format::RG16:
        case Texture::Format::RGBA16:
            c[k] = reinterpret_cast<const uint16_t *>(t.pixels.data())[j] / 65535.0f;
            break;
        case Texture::Format::RGBA32F: c[k] = clean(reinterpret_cast<const float *>(t.pixels.data())[j]); break;
        default: c[k] = clean(HalfToFloat(reinterpret_cast<const uint16_t *>(t.pixels.data())[j])); break;
        }
    }
    return n < 3 ? Vec3{c[0], c[0], c[0]} : Vec3{c[0], c[1], c[2]};  // gray (+ alpha)
}

// Bilinear latlong lookup; u wraps, v (from the top) clamps. Texture rows are
// stored bottom-up.
Vec3 SampleLatlong(const Texture &t, Vec3 d)
{
    const float u = std::atan2(d.z, d.x) / (2 * kPi) + 0.25f;
    const float vTop = std::acos(std::clamp(d.y, -1.0f, 1.0f)) / kPi;
    const float fx = (u - std::floor(u)) * t.width - 0.5f;
    const float fy = (1.0f - vTop) * t.height - 0.5f;
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const float ax = fx - x0, ay = fy - y0;
    auto wrapX = [&](int x) { return uint32_t((x % int(t.width) + int(t.width)) % int(t.width)); };
    auto clampY = [&](int y) { return uint32_t(std::clamp(y, 0, int(t.height) - 1)); };
    const Vec3 c00 = Texel(t, wrapX(x0), clampY(y0)), c10 = Texel(t, wrapX(x0 + 1), clampY(y0));
    const Vec3 c01 = Texel(t, wrapX(x0), clampY(y0 + 1)), c11 = Texel(t, wrapX(x0 + 1), clampY(y0 + 1));
    return (c00 * (1 - ax) + c10 * ax) * (1 - ay) + (c01 * (1 - ax) + c11 * ax) * ay;
}

// Vulkan cube convention: direction through face coordinates s, t in [0, 1].
Vec3 CubeDir(int face, float s, float t)
{
    const float sc = 2 * s - 1, tc = 2 * t - 1;
    switch (face) {
    case 0:  return {1, -tc, -sc};
    case 1:  return {-1, -tc, sc};
    case 2:  return {sc, 1, tc};
    case 3:  return {sc, -1, -tc};
    case 4:  return {sc, -tc, 1};
    default: return {-sc, -tc, -1};
    }
}

void CubeCoord(Vec3 d, int *face, float *s, float *t)
{
    const float ax = std::abs(d.x), ay = std::abs(d.y), az = std::abs(d.z);
    float ma, sc, tc;
    if (ax >= ay && ax >= az) {
        ma = ax;
        *face = d.x > 0 ? 0 : 1;
        sc = d.x > 0 ? -d.z : d.z;
        tc = -d.y;
    } else if (ay >= az) {
        ma = ay;
        *face = d.y > 0 ? 2 : 3;
        sc = d.x;
        tc = d.y > 0 ? d.z : -d.z;
    } else {
        ma = az;
        *face = d.z > 0 ? 4 : 5;
        sc = d.z > 0 ? d.x : -d.x;
        tc = -d.y;
    }
    *s = 0.5f * (sc / ma + 1);
    *t = 0.5f * (tc / ma + 1);
}

// One cube level as RGB floats, face-major.
struct CubeLevel {
    uint32_t size = 0;
    std::vector<Vec3> texels;
    Vec3 &At(int face, uint32_t x, uint32_t y) { return texels[(size_t(face) * size + y) * size + x]; }
    const Vec3 &At(int face, uint32_t x, uint32_t y) const
    {
        return texels[(size_t(face) * size + y) * size + x];
    }
};

// ponytail: bilinear within a face, clamped at edges (no seam filtering).
Vec3 SampleCube(const CubeLevel &c, Vec3 d)
{
    int face;
    float s, t;
    CubeCoord(d, &face, &s, &t);
    const float fx = s * c.size - 0.5f, fy = t * c.size - 0.5f;
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const float ax = fx - x0, ay = fy - y0;
    auto cl = [&](int v) { return uint32_t(std::clamp(v, 0, int(c.size) - 1)); };
    return (c.At(face, cl(x0), cl(y0)) * (1 - ax) + c.At(face, cl(x0 + 1), cl(y0)) * ax) * (1 - ay) +
           (c.At(face, cl(x0), cl(y0 + 1)) * (1 - ax) + c.At(face, cl(x0 + 1), cl(y0 + 1)) * ax) * ay;
}

Vec3 SampleCubeLod(const std::vector<CubeLevel> &chain, Vec3 d, float lod)
{
    lod = std::clamp(lod, 0.0f, float(chain.size() - 1));
    const int l0 = int(lod);
    const int l1 = std::min(l0 + 1, int(chain.size()) - 1);
    const float a = lod - l0;
    return SampleCube(chain[l0], d) * (1 - a) + SampleCube(chain[l1], d) * a;
}

// Runs fn(i) for i in [0, n) on all cores.
template <typename Fn>
void ParallelFor(size_t n, Fn fn)
{
    std::vector<size_t> idx(n);
    std::iota(idx.begin(), idx.end(), size_t(0));
    std::for_each(std::execution::par, idx.begin(), idx.end(), fn);
}

float RadicalInverse(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

// GGX half vector around +Z for the i-th of n Hammersley points.
Vec3 SampleGgx(uint32_t i, uint32_t n, float a)
{
    const float u1 = (i + 0.5f) / n, u2 = RadicalInverse(i);
    const float phi = 2 * kPi * u1;
    const float cosTheta = std::sqrt((1 - u2) / (1 + (a * a - 1) * u2));
    const float sinTheta = std::sqrt(1 - cosTheta * cosTheta);
    return {sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
}

float DGgx(float NoH, float a)
{
    const float a2 = a * a;
    const float f = (NoH * a2 - NoH) * NoH + 1;
    return a2 / (kPi * f * f);
}

float VSmithGgxCorrelated(float NoV, float NoL, float a)
{
    const float a2 = a * a;
    const float gv = NoL * std::sqrt(NoV * NoV * (1 - a2) + a2);
    const float gl = NoV * std::sqrt(NoL * NoL * (1 - a2) + a2);
    return 0.5f / (gv + gl);
}

// The maps of a cube whose level 0 is `chain[0]`.
EnvironmentMaps Prefilter(std::vector<CubeLevel> chain)
{
    const uint32_t size = chain[0].size;
    // Box-filtered chain down to 1x1, the source for filtered importance sampling.
    while (chain.back().size > 1) {
        const CubeLevel &src = chain.back();
        CubeLevel dst;
        dst.size = src.size / 2;
        dst.texels.resize(size_t(6) * dst.size * dst.size);
        for (int f = 0; f < 6; ++f) {
            for (uint32_t y = 0; y < dst.size; ++y) {
                for (uint32_t x = 0; x < dst.size; ++x) {
                    dst.At(f, x, y) = (src.At(f, 2 * x, 2 * y) + src.At(f, 2 * x + 1, 2 * y) +
                                       src.At(f, 2 * x, 2 * y + 1) + src.At(f, 2 * x + 1, 2 * y + 1)) * 0.25f;
                }
            }
        }
        chain.push_back(std::move(dst));
    }

    EnvironmentMaps env;
    env.size = size;
    env.levels = uint32_t(std::log2(size)) - 1;  // stop at 4x4
    for (uint32_t level = 0; level < env.levels; ++level) {
        const uint32_t n = size >> level;
        const size_t base = env.data.size();
        env.data.resize(base + size_t(6) * n * n * 4);
        float *out = env.data.data() + base;
        const float roughness = float(level) / float(env.levels - 1);
        const float a = std::max(roughness * roughness, 1e-4f);
        constexpr uint32_t kSamples = 128;
        const float texelSolidAngle = 4 * kPi / (6.0f * size * size);

        ParallelFor(size_t(6) * n, [&](size_t row) {
            const int face = int(row / n);
            const uint32_t y = uint32_t(row % n);
            for (uint32_t x = 0; x < n; ++x) {
                const Vec3 N = Normalize(CubeDir(face, (x + 0.5f) / n, (y + 0.5f) / n));
                Vec3 color;
                if (level == 0) {
                    color = chain[0].At(face, x, y);
                } else {
                    // Prefilter with N = V = R (split-sum assumption).
                    const Vec3 up = std::abs(N.z) < 0.999f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
                    const Vec3 tx = Normalize(Cross(up, N)), ty = Cross(N, tx);
                    float weight = 0;
                    for (uint32_t i = 0; i < kSamples; ++i) {
                        const Vec3 h = SampleGgx(i, kSamples, a);
                        const Vec3 H = tx * h.x + ty * h.y + N * h.z;
                        const Vec3 L = H * (2 * Dot(N, H)) - N;
                        const float NoL = Dot(N, L);
                        if (NoL <= 0) {
                            continue;
                        }
                        // Filtered importance sampling: read a blurrier level
                        // where samples are sparse (pdf = D/4 when N = V).
                        const float pdf = DGgx(h.z, a) * 0.25f;
                        const float sampleSolidAngle = 1.0f / (kSamples * pdf + 1e-6f);
                        const float lod = 0.5f * std::log2(sampleSolidAngle / texelSolidAngle) + 1;
                        color = color + SampleCubeLod(chain, L, lod) * NoL;
                        weight += NoL;
                    }
                    color = color * (1.0f / std::max(weight, 1e-6f));
                }
                float *o = out + ((size_t(face) * n + y) * n + x) * 4;
                o[0] = color.x;
                o[1] = color.y;
                o[2] = color.z;
                o[3] = 1;
            }
        });
    }

    // Diffuse: project level 0 onto SH9, then convolve with the cosine lobe.
    double sh[9][3] = {};
    const CubeLevel &c0 = chain[0];
    for (int f = 0; f < 6; ++f) {
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const float sc = 2 * (x + 0.5f) / size - 1, tc = 2 * (y + 0.5f) / size - 1;
                const float dOmega = (4.0f / (size * size)) /
                                     std::pow(1 + sc * sc + tc * tc, 1.5f);
                const Vec3 d = Normalize(CubeDir(f, (x + 0.5f) / size, (y + 0.5f) / size));
                const float basis[9] = {
                    0.282095f,
                    0.488603f * d.y, 0.488603f * d.z, 0.488603f * d.x,
                    1.092548f * d.x * d.y, 1.092548f * d.y * d.z,
                    0.315392f * (3 * d.z * d.z - 1),
                    1.092548f * d.x * d.z, 0.546274f * (d.x * d.x - d.y * d.y),
                };
                const Vec3 &L = c0.At(f, x, y);
                for (int i = 0; i < 9; ++i) {
                    sh[i][0] += L.x * basis[i] * dOmega;
                    sh[i][1] += L.y * basis[i] * dOmega;
                    sh[i][2] += L.z * basis[i] * dOmega;
                }
            }
        }
    }
    const float band[9] = {kPi, 2 * kPi / 3, 2 * kPi / 3, 2 * kPi / 3,
                           kPi / 4, kPi / 4, kPi / 4, kPi / 4, kPi / 4};
    for (int i = 0; i < 9; ++i) {
        for (int c = 0; c < 3; ++c) {
            env.sh[i][c] = float(sh[i][c] * band[i]);
        }
    }
    return env;
}

} // namespace

EnvironmentMaps BuildEnvironment(const Texture *latlong)
{
    // Base cube at roughly the source's resolution, at most 128 (Unreal's
    // default sky capture size).
    uint32_t size = 16;
    while (latlong && size < 128 && size * 4 * 2 <= latlong->width) {
        size *= 2;
    }

    // Level 0: 4x4 supersampled latlong lookups per texel.
    std::vector<CubeLevel> chain(1);
    chain[0].size = size;
    chain[0].texels.assign(size_t(6) * size * size, Vec3{1, 1, 1});
    if (latlong) {
        ParallelFor(size_t(6) * size, [&](size_t row) {
            const int face = int(row / size);
            const uint32_t y = uint32_t(row % size);
            for (uint32_t x = 0; x < size; ++x) {
                Vec3 sum;
                for (int sy = 0; sy < 4; ++sy) {
                    for (int sx = 0; sx < 4; ++sx) {
                        const Vec3 d = Normalize(CubeDir(face, (x + (sx + 0.5f) / 4) / size,
                                                         (y + (sy + 0.5f) / 4) / size));
                        sum = sum + SampleLatlong(*latlong, d);
                    }
                }
                chain[0].At(face, x, y) = sum * (1.0f / 16);
            }
        });
    }
    return Prefilter(std::move(chain));
}

EnvironmentMaps BuildEnvironment(const float *faces, uint32_t size)
{
    std::vector<CubeLevel> chain(1);
    chain[0].size = size;
    chain[0].texels.resize(size_t(6) * size * size);
    for (size_t i = 0; i < chain[0].texels.size(); ++i) {
        chain[0].texels[i] = {faces[i * 4], faces[i * 4 + 1], faces[i * 4 + 2]};
    }
    return Prefilter(std::move(chain));
}

std::vector<float> BuildBrdfLut(uint32_t size)
{
    std::vector<float> lut(size_t(size) * size * 2);
    constexpr uint32_t kSamples = 512;
    ParallelFor(size, [&](size_t j) {
        const float roughness = (j + 0.5f) / size;
        const float a = roughness * roughness;
        for (uint32_t i = 0; i < size; ++i) {
            const float NoV = (i + 0.5f) / size;
            const Vec3 V{std::sqrt(1 - NoV * NoV), 0, NoV};
            float A = 0, B = 0;
            for (uint32_t s = 0; s < kSamples; ++s) {
                const Vec3 H = SampleGgx(s, kSamples, a);
                const Vec3 L = H * (2 * Dot(V, H)) - V;
                const float NoL = L.z, NoH = H.z, VoH = std::max(Dot(V, H), 0.0f);
                if (NoL <= 0) {
                    continue;
                }
                // Visibility-form GGX with pdf = D * NoH / (4 VoH) divided out.
                const float g = VSmithGgxCorrelated(NoV, NoL, a) * 4 * NoL * VoH / NoH;
                const float fc = std::pow(1 - VoH, 5.0f);
                A += (1 - fc) * g;
                B += fc * g;
            }
            lut[(j * size + i) * 2] = A / kSamples;
            lut[(j * size + i) * 2 + 1] = B / kSamples;
        }
    });
    return lut;
}

} // namespace vkr
