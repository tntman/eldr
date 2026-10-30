#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace vkr {

// Intel Open Image Denoise (Apache 2.0, optional: HDVK_OIDN) for
// Frame::denoise: its "RT" filter on the film's linear HDR beauty, guided
// by the film's albedo and normal (rasterised, clean: cleanAux). On the CPU
// device; the environment variable HDVK_OIDN_DEVICE (cpu, cuda, default)
// picks another, HDVK_OIDN_PREFILTER=1 denoises the guides first.
class Denoiser {
public:
    Denoiser();
    ~Denoiser();
    Denoiser(const Denoiser &) = delete;
    Denoiser &operator=(const Denoiser &) = delete;

    // Denoises `rgba` (half RGBA, premultiplied, `width` x `height`) in
    // place, alpha kept. `albedo` and `normal` (3 floats per pixel) may be
    // null; where `normal` is 0 (nothing drawn) the albedo becomes the
    // colour, clamped to 1. False with `error` set if it couldn't run
    // (built without OIDN, no device): `rgba` is then unchanged.
    // `prefilter`: denoise the guides first (noisy guides).
    bool Run(uint16_t *rgba, float *albedo, float *normal, uint32_t width, uint32_t height, bool prefilter,
             std::string *error);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;  // the device, made on first Run
};

float HalfToFloat(uint16_t h);

} // namespace vkr
