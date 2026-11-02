#pragma once

#include "pxr/pxr.h"

#include "renderer.h"

#include <cstddef>
#include <cstdint>
#include <functional>

PXR_NAMESPACE_OPEN_SCOPE

// Tiled rendering (eldr:tileSize) of an offline frame: square tiles of
// `tileSize` pixels (rounded up to 64) plus a border, each a window of the
// whole image's camera (Frame::tile) on a film of the window's size, every
// tile to convergence; the interiors are assembled in host memory, and bloom,
// exposure and tonemapping run once over the whole image (PostImage). GPU
// memory then depends on the tile, not the image. `frame` is the whole
// image's; `sinks` are whole-image targets (colour gets the display image,
// linearColor the linear beauty, the rest as Render's; extra AOVs from the
// first sink only). `sampled(more)` runs after each sample, `more` false
// after the last; later samples of a tile use `laterStillCamera`.
void HdVkRenderTiles(vkr::Renderer &renderer, const vkr::Scene &scene, const vkr::Renderer::Frame &frame,
                     uint32_t tileSize, const vkr::Renderer::Targets *sinks, size_t sinkCount,
                     bool laterStillCamera, const std::function<void(bool more)> &sampled);

// The tile size eldr:tileSize asks for, rounded to whole 64 x 64 blocks of
// the sample noise (so a tile's pixels sample as the untiled image's); 0: off.
inline uint32_t HdVkTileSize(int setting)
{
    return setting > 0 ? (uint32_t(setting) + 63u) / 64u * 64u : 0u;
}

PXR_NAMESPACE_CLOSE_SCOPE
