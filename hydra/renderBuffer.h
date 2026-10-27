#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hgi/texture.h"

#include "timing.h"

#ifdef HDVK_HOUDINI
#include <UT/UT_HUSDExtraAOVResource.h>
#endif

#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// CPU-side AOV storage the render pass fills via readback. Always converged.
// The CPU copy is fetched only when someone maps it (SetFetch): an app
// (Houdini's viewport) may bind more than it reads each frame. With GPU
// sharing (HdVkGLInterop) colour and depth are also a texture of the app's
// Hgi (GetResource).
class HdVkRenderBuffer final : public HdRenderBuffer
{
public:
    explicit HdVkRenderBuffer(SdfPath const &id) : HdRenderBuffer(id) {}

    bool Allocate(GfVec3i const &dimensions, HdFormat format, bool) override
    {
        HdVkTimedCall timed(HdVkCallTimes::kAllocate);
        _Deallocate();
        if (dimensions[0] <= 0 || dimensions[1] <= 0 || dimensions[2] != 1 ||
            format == HdFormatInvalid) {
            return false;
        }
        _width = unsigned(dimensions[0]);
        _height = unsigned(dimensions[1]);
        _format = format;
        _texture = HgiTextureHandle();
        _fetch = nullptr;
        _external = nullptr;
        _mapped = true;  // a new buffer counts as read: its first frame fills it
        _data.resize(size_t(_width) * _height * HdDataSizeOfFormat(format));
        return true;
    }

    unsigned int GetWidth() const override { return _width; }
    unsigned int GetHeight() const override { return _height; }
    unsigned int GetDepth() const override { return 1; }
    HdFormat GetFormat() const override { return _format; }
    bool IsMultiSampled() const override { return false; }

    void *Map() override
    {
        HdVkTimedCall timed(HdVkCallTimes::kMap);
        if (_fetch) {
            std::function<void(void *)> fetch = std::move(_fetch);
            _fetch = nullptr;
            fetch(_data.data());
            _external = nullptr;
        }
        ++_mappers;
        _mapped = true;
        return _external ? _external : _data.data();
    }
    // Whether it was mapped since the last call (the render pass's, once a frame).
    bool TakeMapped() { return _mapped.exchange(false); }
    void Unmap() override { --_mappers; }
    bool IsMapped() const override { return _mappers > 0; }

    void Resolve() override {}
    bool IsConverged() const override { return true; }

    // GPU sharing: this frame's texture (empty: the CPU data is the frame).
    // Houdini: a Cryptomatte AOV tells husk about its rank layers (see
    // SetCrypto), which husk reads through this.
    VtValue GetResource(bool) const override
    {
#ifdef HDVK_HOUDINI
        if (_crypto) {
            HdAovSettingsMap map;
            map[TfToken("extra_aov_resource")] = VtValue(std::static_pointer_cast<void>(_crypto));
            return VtValue(map);
        }
#endif
        return _texture ? VtValue(_texture) : VtValue();
    }
#ifdef HDVK_HOUDINI
    // Cryptomatte for husk (UT_HUSDExtraAOVResource.h): the layers
    // <name>00..02 hold (id, coverage, id, coverage) per pixel, ranks 0-5,
    // in the three planes returned here one after the other (the buffer's
    // size, same row order as the AOV itself); `metadata` is the spec's
    // cryptomatte/<key>/... entries.
    // ponytail: husk takes the ranks the delegate offers, not the var's
    // cryptomatterank.
    float *SetCrypto(const std::string &name, std::map<std::string, std::string> metadata)
    {
        _cryptoPlane.assign(size_t(_width) * _height * 12, 0.0f);
        _crypto = std::make_shared<UT_HUSDExtraAOVResource>(
            [this](int plane) -> void * { return _cryptoPlane.data() + size_t(plane) * _width * _height * 4; },
            [](int) {});
        for (const char *layer : {"00", "01", "02"}) {
            _crypto->addPlane(name + layer, HdFormatFloat32Vec4);
        }
        _crypto->setMetadata(metadata);
        return _cryptoPlane.data();
    }
#endif
    void SetTexture(HgiTextureHandle texture) { _texture = texture; }
    // Fills the CPU copy on the next Map (null: it is current).
    void SetFetch(std::function<void(void *)> fetch) { _fetch = std::move(fetch); }
    // The frame where the renderer read it back (eldr:inPlaceReadback),
    // mapped instead of the CPU copy until the next fetch; null: the copy.
    void SetExternal(void *data) { _external = data; }
    // Keeps the frame the app reads as is, in its own copy, with nothing
    // to fetch (eldr:staleMovingIds).
    void Detach()
    {
        if (_external) {
            std::memcpy(_data.data(), _external, _data.size());
            _external = nullptr;
        }
        _fetch = nullptr;
    }

protected:
    void _Deallocate() override
    {
        _width = _height = 0;
        _format = HdFormatInvalid;
        _data = {};
    }

private:
    unsigned int _width = 0, _height = 0;
    HdFormat _format = HdFormatInvalid;
    std::vector<uint8_t> _data;
    std::atomic<int> _mappers{0};
    std::atomic<bool> _mapped{true};
    HgiTextureHandle _texture;
    std::function<void(void *)> _fetch;
    void *_external = nullptr;
#ifdef HDVK_HOUDINI
    std::vector<float> _cryptoPlane;
    UT_HUSDExtraAOVResourcePtr _crypto;
#endif
};

PXR_NAMESPACE_CLOSE_SCOPE
