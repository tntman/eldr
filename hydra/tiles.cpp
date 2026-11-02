#include "tiles.h"

#include "scene.h"

#include "pxr/base/gf/half.h"
#include "pxr/base/work/loops.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// A tile's interior [x0, x1) x [y0, y1) in the whole image (rows from the
// bottom), and where its window (tileWidth wide) starts.
struct Interior {
    uint32_t x0, y0, x1, y1, windowX, windowY, width, tileWidth;

    // Its pixels (`bytes` each) from the window's buffer into the whole image's.
    void Copy(const void *window, void *whole, size_t bytes) const
    {
        if (!window || !whole) {
            return;
        }
        WorkParallelForN(y1 - y0, [&](size_t begin, size_t end) {
            for (size_t y = y0 + begin; y < y0 + end; ++y) {
                std::memcpy(static_cast<uint8_t *>(whole) + (y * width + x0) * bytes,
                            static_cast<const uint8_t *>(window) + ((y - windowY) * tileWidth + (x0 - windowX)) * bytes,
                            (x1 - x0) * bytes);
            }
        });
    }
};

} // namespace

void HdVkRenderTiles(vkr::Renderer &renderer, const vkr::Scene &scene, const vkr::Renderer::Frame &frame,
                     uint32_t tileSize, const vkr::Renderer::Targets *sinks, size_t sinkCount,
                     bool laterStillCamera, const std::function<void(bool more)> &sampled)
{
    using Aovs = vkr::Aovs;
    const uint32_t width = frame.width, height = frame.height;
    // The border: how far past its tile a pixel's screen-space work reaches
    // (the film's filter, AO, SSR, the residual blurs); OIDN's network further.
    const uint32_t border = frame.denoise ? 128u : 64u;
    const uint32_t windowWidth = std::min(tileSize + 2 * border, width);
    const uint32_t windowHeight = std::min(tileSize + 2 * border, height);
    const size_t windowPixels = size_t(windowWidth) * windowHeight, pixels = size_t(width) * height;

    // The window's targets: what any sink asks for (extra AOVs: the first's).
    vkr::Renderer::Targets t;
    t.readback = false;  // Read fetches a tile's last sample
    std::vector<GfHalf> windowColor, hdr;
    std::vector<float> windowDepth;
    std::vector<int32_t> windowPrimId, windowInstanceId;
    for (size_t s = 0; s < sinkCount; ++s) {
        if ((sinks[s].color || sinks[s].linearColor) && hdr.empty()) {
            windowColor.resize(windowPixels * 4);
            hdr.resize(pixels * 4);
            t.color = windowColor.data();
        }
        if (sinks[s].depth && windowDepth.empty()) {
            windowDepth.resize(windowPixels);
            t.depth = windowDepth.data();
        }
        if (sinks[s].primId && windowPrimId.empty()) {
            windowPrimId.resize(windowPixels);
            t.primId = windowPrimId.data();
        }
        if (sinks[s].instanceId && windowInstanceId.empty()) {
            windowInstanceId.resize(windowPixels);
            t.instanceId = windowInstanceId.data();
        }
    }
    const vkr::Renderer::Targets &first = sinks[0];
    std::vector<float> windowAovs[Aovs::kCount];
    uint32_t aovFloats[Aovs::kCount] = {};
    for (uint32_t k = 0; k < Aovs::kCount; ++k) {
        if (first.aovs[k]) {
            aovFloats[k] = first.aovComponents[k] ? first.aovComponents[k] : Aovs::Components(Aovs::Kind(k));
            windowAovs[k].resize(windowPixels * aovFloats[k]);
            t.aovs[k] = windowAovs[k].data();
            t.aovComponents[k] = first.aovComponents[k];
            t.aovFilters[k] = first.aovFilters[k];
            t.aovFilterRadius[k] = first.aovFilterRadius[k];
        }
    }

    vkr::Renderer::Frame f = frame;
    f.width = windowWidth;
    f.height = windowHeight;
    f.tile[2] = width;
    f.tile[3] = height;
    f.sensorWidth = frame.sensorWidth * float(windowWidth) / float(width);  // the window's share of the sensor
    f.deferReadback = false;
    f.linearColor = false;
    if (frame.postProcess) {
        // Tiles give the exposed scene over no background; PostImage
        // blooms, tonemaps and composites the whole image.
        f.post.bloomIntensity = 0.0f;
        f.post.tonemapper = vkr::PostProcess::Settings::Tonemapper::None;
        std::fill_n(f.clearColor, 4, 0.0f);
    }
    const uint32_t across = (width + tileSize - 1) / tileSize, down = (height + tileSize - 1) / tileSize;
    const bool timing = std::getenv("HDVK_TIMING") != nullptr;
    for (uint32_t ty = 0; ty < down; ++ty) {
        for (uint32_t tx = 0; tx < across; ++tx) {
            Interior in;
            in.x0 = tx * tileSize;
            in.y0 = ty * tileSize;
            in.x1 = std::min(in.x0 + tileSize, width);
            in.y1 = std::min(in.y0 + tileSize, height);
            // The window: the tile and its border, kept inside the image
            // (its edges stay the image's edges for screen-space effects).
            in.windowX = std::min(in.x0 - std::min(in.x0, border), width - windowWidth);
            in.windowY = std::min(in.y0 - std::min(in.y0, border), height - windowHeight);
            in.width = width;
            in.tileWidth = windowWidth;
            f.tile[0] = in.windowX;
            f.tile[1] = in.windowY;
            std::copy(frame.viewProj, frame.viewProj + 16, f.viewProj);
            std::copy(frame.invViewProj, frame.invViewProj + 16, f.invViewProj);
            std::copy(frame.prevViewProj, frame.prevViewProj + 16, f.prevViewProj);
            vkr::Renderer::CropToTile(f, f.viewProj, f.invViewProj);
            vkr::Renderer::CropToTile(f, f.prevViewProj, nullptr);
            f.stillCamera = frame.stillCamera;
            const bool lastTile = ty + 1 == down && tx + 1 == across;
            const auto tileStart = std::chrono::steady_clock::now();
            int samples = 0;
            for (;;) {
                renderer.Render(scene, f, t);
                ++samples;
                const bool done = renderer.IsConverged() && !scene.LodsPending();
                sampled(!(done && lastTile));
                if (done) {
                    break;
                }
                f.stillCamera = laterStillCamera;
            }
            renderer.Read(t);
            if (timing) {
                std::fprintf(stderr, "HdVk tile %u/%u (%u,%u %ux%u): %d samples, %.0f ms\n", ty * across + tx + 1,
                             across * down, in.x0, in.y0, in.x1 - in.x0, in.y1 - in.y0, samples,
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tileStart)
                                 .count());
            }
            if (t.color) {
                in.Copy(t.color, hdr.data(), 8);
                // Unexposed, as PostImage takes it: the tile's exposure out.
                const double exposure = frame.postProcess ? renderer.GetStats().exposure : 1.0;
                if (exposure != 1.0 && exposure > 0.0) {
                    const float scale = float(1.0 / exposure);
                    WorkParallelForN(in.y1 - in.y0, [&](size_t begin, size_t end) {
                        for (size_t y = in.y0 + begin; y < in.y0 + end; ++y) {
                            GfHalf *p = hdr.data() + (y * width + in.x0) * 4;
                            for (size_t i = 0; i < size_t(in.x1 - in.x0) * 4; ++i) {
                                p[i] = i % 4 == 3 ? p[i] : GfHalf(float(p[i]) * scale);
                            }
                        }
                    });
                }
            }
            for (size_t s = 0; s < sinkCount; ++s) {
                in.Copy(t.depth, sinks[s].depth, 4);
                in.Copy(t.primId, sinks[s].primId, 4);
                in.Copy(t.instanceId, sinks[s].instanceId, 4);
            }
            for (uint32_t k = 0; k < Aovs::kCount; ++k) {
                in.Copy(t.aovs[k], first.aovs[k], size_t(aovFloats[k]) * 4);
            }
        }
    }
    for (size_t s = 0; s < sinkCount; ++s) {
        if (!sinks[s].color && !sinks[s].linearColor) {
            continue;
        }
        if (frame.postProcess && !sinks[s].color && !frame.post.autoExposure) {
            // Only the linear beauty (husk): tonemap.frag's exposure-only
            // path on the host, without the GPU's whole-image copies.
            const float scale = std::exp2(std::clamp(frame.post.exposureCompensation, -16.0f, 16.0f));
            const float *bg = frame.clearColor;
            GfHalf *dst = static_cast<GfHalf *>(sinks[s].linearColor);
            WorkParallelForN(pixels, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    const GfHalf *p = hdr.data() + i * 4;
                    const float coverage = std::clamp(float(p[3]), 0.0f, 1.0f);
                    const float glow[3] = {std::max(float(p[0]), 0.0f), std::max(float(p[1]), 0.0f),
                                           std::max(float(p[2]), 0.0f)};
                    const bool empty = coverage <= 0.0f && std::max({glow[0], glow[1], glow[2]}) <= 0.0f;
                    for (int c = 0; c < 4; ++c) {
                        const float v = c == 3 ? coverage : glow[c] * scale;
                        dst[i * 4 + c] = GfHalf(empty ? bg[c] : v + bg[c] * (1.0f - coverage));
                    }
                }
            });
        } else if (frame.postProcess) {
            renderer.PostImage(frame, hdr.data(), width, height, sinks[s].color, sinks[s].linearColor);
        } else {  // raw HDR: as it rendered
            for (void *dst : {sinks[s].color, sinks[s].linearColor}) {
                if (dst) {
                    std::memcpy(dst, hdr.data(), pixels * 8);
                }
            }
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
