#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hd/renderBuffer.h"

#include <atomic>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// CPU-side AOV storage the render pass fills via readback. Always converged.
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
        _data.resize(size_t(_width) * _height * HdDataSizeOfFormat(format));
        return true;
    }

    unsigned int GetWidth() const override { return _width; }
    unsigned int GetHeight() const override { return _height; }
    unsigned int GetDepth() const override { return 1; }
    HdFormat GetFormat() const override { return _format; }
    bool IsMultiSampled() const override { return false; }

    void *Map() override { ++_mappers; return _data.data(); }
    void Unmap() override { --_mappers; }
    bool IsMapped() const override { return _mappers > 0; }

    void Resolve() override {}
    bool IsConverged() const override { return true; }

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
};

PXR_NAMESPACE_CLOSE_SCOPE
