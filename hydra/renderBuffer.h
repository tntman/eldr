#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hgi/texture.h"

#include <atomic>
#include <functional>
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
        if (_fetch) {
            std::function<void(void *)> fetch = std::move(_fetch);
            _fetch = nullptr;
            fetch(_data.data());
        }
        ++_mappers;
        _mapped = true;
        return _data.data();
    }
    // Whether it was mapped since the last call (the render pass's, once a frame).
    bool TakeMapped() { return _mapped.exchange(false); }
    void Unmap() override { --_mappers; }
    bool IsMapped() const override { return _mappers > 0; }

    void Resolve() override {}
    bool IsConverged() const override { return true; }

    // GPU sharing: this frame's texture (empty: the CPU data is the frame).
    VtValue GetResource(bool) const override { return _texture ? VtValue(_texture) : VtValue(); }
    void SetTexture(HgiTextureHandle texture) { _texture = texture; }
    // Fills the CPU copy on the next Map (null: it is current).
    void SetFetch(std::function<void(void *)> fetch) { _fetch = std::move(fetch); }

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
};

PXR_NAMESPACE_CLOSE_SCOPE
