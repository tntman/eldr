#include "denoise.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef HDVK_OIDN
#include <OpenImageDenoise/oidn.hpp>
#ifdef _WIN32
#include <windows.h>
#endif
#endif

namespace vkr {

float HalfToFloat(uint16_t h)
{
    const int exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    const float sign = (h & 0x8000) ? -1.0f : 1.0f;
    if (exponent == 0) {
        return sign * std::ldexp(float(mantissa), -24);
    }
    if (exponent == 31) {
        return mantissa ? 0.0f : sign * 65504.0f;
    }
    return sign * std::ldexp(float(mantissa | 0x400), exponent - 25);
}

#if defined(HDVK_OIDN) && defined(_WIN32)
namespace {

// OpenImageDenoise.dll is delay-loaded (CMakeLists.txt): the host's own if
// it has one (Houdini's $HFS/bin, which Karma uses too), else the copy next
// to this module (hdVk.dll, vkrtest), whose dependencies (its core, oneTBB)
// then load from there too. False if neither loads.
bool LoadOidn(std::string *error)
{
    if (GetModuleHandleW(L"OpenImageDenoise.dll") || LoadLibraryW(L"OpenImageDenoise.dll")) {
        return true;
    }
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&LoadOidn), &self) ||
        !GetModuleFileNameW(self, path, MAX_PATH)) {
        *error = "no module path for OpenImageDenoise.dll";
        return false;
    }
    std::wstring dir(path);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    // A host's oneTBB already loaded stays the one (Houdini's has every
    // entry point OIDN's CPU device imports).
    if (!GetModuleHandleW(L"tbb12.dll")) {
        LoadLibraryExW((dir + L"tbb12.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (!LoadLibraryExW((dir + L"OpenImageDenoise.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
        *error = "OpenImageDenoise.dll is neither the host's nor next to Eldr's module";
        return false;
    }
    return true;
}

} // namespace
#endif

// Round to nearest; finite inputs up to 65504 (the film's are clean).
uint16_t FloatToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint16_t sign = uint16_t((x >> 16) & 0x8000);
    const float a = std::min(std::fabs(f), 65504.0f);
    if (a < 6.103515625e-05f) {  // a subnormal half
        return uint16_t(sign | uint16_t(std::lround(a * 16777216.0f)));
    }
    std::memcpy(&x, &a, 4);
    x += 0x0FFF + ((x >> 13) & 1);  // round to nearest even
    return uint16_t(sign | (((x >> 23) - 112) << 10) | ((x >> 13) & 0x3FF));
}

struct Denoiser::Impl {
#ifdef HDVK_OIDN
    oidn::DeviceRef device;
    oidn::FilterRef beauty, albedo, normal;
#endif
};

Denoiser::Denoiser() = default;
Denoiser::~Denoiser() = default;

bool Denoiser::Run(uint16_t *rgba, float *albedo, float *normal, uint32_t width, uint32_t height, bool prefilter,
                   std::string *error)
{
#ifdef HDVK_OIDN
    if (!_impl) {
#ifdef _WIN32
        if (!LoadOidn(error)) {
            return false;
        }
#endif
        const char *env = std::getenv("HDVK_OIDN_DEVICE");
        const std::string kind = env ? env : "cpu";
        auto impl = std::make_unique<Impl>();
        impl->device = oidn::newDevice(kind == "cuda"      ? oidn::DeviceType::CUDA
                                       : kind == "default" ? oidn::DeviceType::Default
                                                           : oidn::DeviceType::CPU);
        const char *message = nullptr;
        if (!impl->device || impl->device.getError(message) != oidn::Error::None) {
            *error = std::string("no OIDN ") + kind + " device: " + (message ? message : "");
            return false;
        }
        impl->device.commit();
        if (impl->device.getError(message) != oidn::Error::None) {
            *error = std::string("OIDN device: ") + (message ? message : "");
            return false;
        }
        impl->beauty = impl->device.newFilter("RT");
        impl->albedo = impl->device.newFilter("RT");
        impl->normal = impl->device.newFilter("RT");
        _impl = std::move(impl);
    }
    const size_t pixels = size_t(width) * height;
    if (albedo && normal) {
        // Nothing drawn: the colour is the albedo (the background as seen).
        for (size_t p = 0; p < pixels; ++p) {
            const float *n = normal + 3 * p;
            if (n[0] == 0.0f && n[1] == 0.0f && n[2] == 0.0f) {
                for (int c = 0; c < 3; ++c) {
                    albedo[3 * p + c] = std::clamp(HalfToFloat(rgba[4 * p + c]), 0.0f, 1.0f);
                }
            }
        }
    }
    // Separate float images in and out (OIDN filters don't promise in-place).
    // The guides are the rasteriser's, noise-free: the beauty's filter trusts
    // them (cleanAux). `prefilter` first denoises them as OIDN recommends
    // for noisy guides (traced ones), at twice the cost.
    Impl &d = *_impl;
    std::vector<float> scratch(pixels * 3);
    auto denoiseGuide = [&](oidn::FilterRef &filter, const char *name, float *data) {
        filter.setImage(name, data, oidn::Format::Float3, width, height);
        filter.setImage("output", scratch.data(), oidn::Format::Float3, width, height);
        filter.commit();
        filter.execute();
        std::copy(scratch.begin(), scratch.end(), data);
    };
    if (albedo && prefilter) {
        denoiseGuide(d.albedo, "albedo", albedo);
    }
    if (normal && prefilter) {
        denoiseGuide(d.normal, "normal", normal);
    }
    std::vector<float> color(pixels * 3);
    for (size_t p = 0; p < pixels; ++p) {
        for (int c = 0; c < 3; ++c) {
            color[3 * p + c] = HalfToFloat(rgba[4 * p + c]);
        }
    }
    d.beauty.setImage("color", color.data(), oidn::Format::Float3, width, height);
    d.beauty.setImage("output", scratch.data(), oidn::Format::Float3, width, height);
    if (albedo) {
        d.beauty.setImage("albedo", albedo, oidn::Format::Float3, width, height);
    } else {
        d.beauty.unsetImage("albedo");
    }
    // OIDN takes a normal only with an albedo.
    if (albedo && normal) {
        d.beauty.setImage("normal", normal, oidn::Format::Float3, width, height);
    } else {
        d.beauty.unsetImage("normal");
    }
    d.beauty.set("hdr", true);
    d.beauty.set("cleanAux", albedo != nullptr);
    d.beauty.commit();
    d.beauty.execute();
    const char *message = nullptr;
    if (d.device.getError(message) != oidn::Error::None) {
        *error = std::string("OIDN: ") + (message ? message : "");
        return false;
    }
    for (size_t p = 0; p < pixels; ++p) {
        for (int c = 0; c < 3; ++c) {
            rgba[4 * p + c] = FloatToHalf(scratch[3 * p + c]);
        }
    }
    return true;
#else
    (void)rgba, (void)albedo, (void)normal, (void)width, (void)height, (void)prefilter;
    *error = "Eldr was built without Intel Open Image Denoise (OIDN_ROOT)";
    return false;
#endif
}

} // namespace vkr
