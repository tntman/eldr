// Block compression of 8-bit textures on the CPU (CompressTexture in
// scene.h): BC4 (gray), BC5 (gray + alpha), BC7 (colour). BC7 uses mode 6
// only (one RGBA line per 4x4 block, 16 steps): simple, no partition search.
// ponytail: mode 6 only, about 40 dB on photos; add BC7's partitioned modes
// (bc7e's search) if sharp two-colour edges show blocks in a final.
#include "scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <execution>
#include <numeric>

namespace vkr {
namespace {

constexpr int kWeights[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

void PutBits(uint8_t *out, int &pos, uint32_t v, int n)
{
    for (int i = 0; i < n; ++i, ++pos) {
        out[pos >> 3] |= uint8_t(((v >> i) & 1u) << (pos & 7));
    }
}

// A mode 6 endpoint: 7 bits per channel and a p-bit shared by its channels.
struct Endpoint {
    int q[4];
    int p;
    int Value(int c) const { return q[c] << 1 | p; }
};

Endpoint Quantize(const float v[4])
{
    Endpoint best{};
    float bestErr = 1e30f;
    for (int p = 0; p < 2; ++p) {
        Endpoint e{};
        e.p = p;
        float err = 0;
        for (int c = 0; c < 4; ++c) {
            e.q[c] = std::clamp(int(std::lround((v[c] - p) * 0.5f)), 0, 127);
            const float d = float(e.Value(c)) - v[c];
            err += d * d;
        }
        if (err < bestErr) {
            bestErr = err;
            best = e;
        }
    }
    return best;
}

int Interpolate(int a, int b, int w) { return ((64 - w) * a + w * b + 32) >> 6; }

// Nearest of the 16 steps for each texel; the summed squared error.
int Assign(const uint8_t px[16][4], const Endpoint e[2], uint8_t idx[16])
{
    int palette[16][4];
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 4; ++c) {
            palette[i][c] = Interpolate(e[0].Value(c), e[1].Value(c), kWeights[i]);
        }
    }
    int total = 0;
    for (int t = 0; t < 16; ++t) {
        int best = 1 << 30;
        for (int i = 0; i < 16; ++i) {
            int err = 0;
            for (int c = 0; c < 4; ++c) {
                const int d = palette[i][c] - px[t][c];
                err += d * d;
            }
            if (err < best) {
                best = err;
                idx[t] = uint8_t(i);
            }
        }
        total += best;
    }
    return total;
}

void EncodeBc7(const uint8_t px[16][4], uint8_t out[16])
{
    // The block's principal axis (power iteration on the covariance).
    float mean[4] = {};
    for (int t = 0; t < 16; ++t) {
        for (int c = 0; c < 4; ++c) {
            mean[c] += px[t][c] / 16.0f;
        }
    }
    float cov[4][4] = {};
    for (int t = 0; t < 16; ++t) {
        for (int a = 0; a < 4; ++a) {
            for (int b = 0; b < 4; ++b) {
                cov[a][b] += (px[t][a] - mean[a]) * (px[t][b] - mean[b]);
            }
        }
    }
    float axis[4] = {1, 1, 1, 1};
    for (int it = 0; it < 8; ++it) {
        float next[4] = {};
        for (int a = 0; a < 4; ++a) {
            for (int b = 0; b < 4; ++b) {
                next[a] += cov[a][b] * axis[b];
            }
        }
        const float len = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2] + next[3] * next[3]);
        if (len < 1e-6f) {
            break;
        }
        for (int c = 0; c < 4; ++c) {
            axis[c] = next[c] / len;
        }
    }
    float lo = 0, hi = 0;
    for (int t = 0; t < 16; ++t) {
        float d = 0;
        for (int c = 0; c < 4; ++c) {
            d += (px[t][c] - mean[c]) * axis[c];
        }
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    float ends[2][4];
    for (int c = 0; c < 4; ++c) {
        ends[0][c] = std::clamp(mean[c] + lo * axis[c], 0.0f, 255.0f);
        ends[1][c] = std::clamp(mean[c] + hi * axis[c], 0.0f, 255.0f);
    }
    Endpoint e[2] = {Quantize(ends[0]), Quantize(ends[1])};
    uint8_t idx[16];
    int err = Assign(px, e, idx);

    // Least-squares endpoints for the chosen steps, while that helps.
    for (int it = 0; it < 3 && err > 0; ++it) {
        float a00 = 0, a01 = 0, a11 = 0, b0[4] = {}, b1[4] = {};
        for (int t = 0; t < 16; ++t) {
            const float w = kWeights[idx[t]] / 64.0f, v = 1 - w;
            a00 += v * v;
            a01 += v * w;
            a11 += w * w;
            for (int c = 0; c < 4; ++c) {
                b0[c] += v * px[t][c];
                b1[c] += w * px[t][c];
            }
        }
        const float det = a00 * a11 - a01 * a01;
        if (std::abs(det) < 1e-6f) {
            break;
        }
        for (int c = 0; c < 4; ++c) {
            ends[0][c] = std::clamp((a11 * b0[c] - a01 * b1[c]) / det, 0.0f, 255.0f);
            ends[1][c] = std::clamp((a00 * b1[c] - a01 * b0[c]) / det, 0.0f, 255.0f);
        }
        const Endpoint f[2] = {Quantize(ends[0]), Quantize(ends[1])};
        uint8_t fidx[16];
        const int ferr = Assign(px, f, fidx);
        if (ferr >= err) {
            break;
        }
        err = ferr;
        e[0] = f[0];
        e[1] = f[1];
        std::memcpy(idx, fidx, 16);
    }

    // The first texel's index has an implicit top bit of 0.
    if (idx[0] & 8) {
        std::swap(e[0], e[1]);
        for (uint8_t &i : idx) {
            i = uint8_t(15 - i);
        }
    }
    std::memset(out, 0, 16);
    int pos = 0;
    PutBits(out, pos, 1u << 6, 7);  // mode 6
    for (int c = 0; c < 4; ++c) {
        PutBits(out, pos, uint32_t(e[0].q[c]), 7);
        PutBits(out, pos, uint32_t(e[1].q[c]), 7);
    }
    PutBits(out, pos, uint32_t(e[0].p), 1);
    PutBits(out, pos, uint32_t(e[1].p), 1);
    PutBits(out, pos, idx[0], 3);
    for (int t = 1; t < 16; ++t) {
        PutBits(out, pos, idx[t], 4);
    }
}

// BC4: the block's min and max, 8 steps between them.
void EncodeBc4(const uint8_t v[16], uint8_t out[8])
{
    const uint8_t lo = *std::min_element(v, v + 16), hi = *std::max_element(v, v + 16);
    std::memset(out, 0, 8);
    out[0] = hi;
    out[1] = lo;
    if (hi == lo) {
        return;  // all code 0: hi
    }
    int pos = 16;
    for (int t = 0; t < 16; ++t) {
        // Step s from hi (0) to lo (7); codes 0 = hi, 1 = lo, s + 1 between.
        const int s = int(std::lround(float(hi - v[t]) * 7.0f / float(hi - lo)));
        PutBits(out, pos, uint32_t(s == 0 ? 0 : s == 7 ? 1 : s + 1), 3);
    }
}

// One level (8-bit, `channels` per texel) into blocks, rows of blocks in
// parallel; edge blocks repeat the last row and column.
void EncodeLevel(const uint8_t *src, uint32_t w, uint32_t h, uint32_t channels, Texture::Format f, uint8_t *dst)
{
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    const size_t blockBytes = f == Texture::Format::BC4 ? 8 : 16;
    std::vector<uint32_t> rows(bh);
    std::iota(rows.begin(), rows.end(), 0u);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](uint32_t by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            uint8_t px[16][4];
            for (uint32_t t = 0; t < 16; ++t) {
                const uint32_t x = std::min(bx * 4 + t % 4, w - 1), y = std::min(by * 4 + t / 4, h - 1);
                const uint8_t *s = src + (size_t(y) * w + x) * channels;
                for (uint32_t c = 0; c < 4; ++c) {
                    px[t][c] = c < channels ? s[c] : 255;
                }
            }
            uint8_t *out = dst + (size_t(by) * bw + bx) * blockBytes;
            if (f == Texture::Format::BC4 || f == Texture::Format::BC5) {
                for (uint32_t c = 0; c < channels; ++c) {
                    uint8_t v[16];
                    for (int t = 0; t < 16; ++t) {
                        v[t] = px[t][c];
                    }
                    EncodeBc4(v, out + 8 * c);
                }
            } else {
                EncodeBc7(px, out);
            }
        }
    });
}

float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float LinearToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

} // namespace

bool CompressTexture(Texture &t)
{
    using F = Texture::Format;
    const uint32_t channels = t.format == F::R8 ? 1 : t.format == F::RG8 ? 2
                            : t.format == F::RGBA8 || t.format == F::RGBA8_SRGB ? 4 : 0;
    if (channels == 0 || t.levels != 1 || t.width == 0 || t.height == 0 ||
        t.pixels.size() != size_t(t.width) * t.height * channels) {
        return false;
    }
    const bool srgb = t.format == F::RGBA8_SRGB;
    const F format = channels == 1 ? F::BC4 : channels == 2 ? F::BC5 : srgb ? F::BC7_SRGB : F::BC7;
    const uint32_t levels = 1 + uint32_t(std::floor(std::log2(std::max(t.width, t.height))));

    // Mips are 2x2 boxes of the level above in floats (sRGB colour decoded,
    // as the GPU's blit of uncompressed textures filters it).
    float decode[256];
    for (int i = 0; i < 256; ++i) {
        decode[i] = srgb ? SrgbToLinear(i / 255.0f) : i / 255.0f;
    }
    std::vector<float> level(t.pixels.size());
    for (size_t i = 0; i < level.size(); ++i) {
        level[i] = srgb && i % 4 != 3 ? decode[t.pixels[i]] : t.pixels[i] / 255.0f;
    }
    std::vector<uint8_t> bytes = t.pixels, out;
    uint32_t w = t.width, h = t.height;
    for (uint32_t l = 0; l < levels; ++l) {
        if (l > 0) {
            const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
            std::vector<float> next(size_t(nw) * nh * channels);
            for (uint32_t y = 0; y < nh; ++y) {
                const uint32_t y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
                for (uint32_t x = 0; x < nw; ++x) {
                    const uint32_t x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
                    for (uint32_t c = 0; c < channels; ++c) {
                        auto at = [&](uint32_t xx, uint32_t yy) { return level[(size_t(yy) * w + xx) * channels + c]; };
                        next[(size_t(y) * nw + x) * channels + c] =
                            0.25f * (at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1));
                    }
                }
            }
            level.swap(next);
            w = nw;
            h = nh;
            bytes.resize(level.size());
            for (size_t i = 0; i < level.size(); ++i) {
                const float v = srgb && i % 4 != 3 ? LinearToSrgb(level[i]) : level[i];
                bytes[i] = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
            }
        }
        const size_t at = out.size();
        out.resize(at + LevelBytes(format, w, h));
        EncodeLevel(bytes.data(), w, h, channels, format, out.data() + at);
    }
    t.pixels.swap(out);
    t.format = format;
    t.levels = levels;
    return true;
}

} // namespace vkr
