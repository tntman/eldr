#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hgi/texture.h"

#include <cstdint>
#include <memory>

namespace vkr {
class Device;
class Renderer;
class SharedFrame;
}

PXR_NAMESPACE_OPEN_SCOPE

class Hgi;

// GPU sharing with an OpenGL app (usdview, usdrecord): each frame's colour
// and depth go from the renderer's Vulkan images to textures of the app's
// Hgi (HgiGL) on the GPU, which HdxAovInputTask takes as they are
// (HdRenderBuffer::GetResource), instead of a readback to the CPU and an
// upload back. The Vulkan images' memory and two semaphores are imported
// into GL (GL_EXT_memory_object_win32, GL_EXT_semaphore_win32; garch loads
// the entry points) and copied into the Hgi textures (glCopyImageSubData).
// Runs on the render pass's thread with the app's GL context current.
class HdVkGLInterop
{
public:
    // Null unless `hgi` is OpenGL, its context has the extensions and runs
    // on the renderer's GPU, and the device exports memory.
    static std::unique_ptr<HdVkGLInterop> Create(Hgi *hgi, vkr::Device &device);
    ~HdVkGLInterop();

    // Copies the renderer's last frame into Color() and Depth() (remade at
    // its size). False if GL reported an error: use the CPU readback (and
    // don't call again).
    bool Present(vkr::Renderer &renderer);
    HgiTextureHandle Color() const { return _textures[0]; }
    HgiTextureHandle Depth() const { return _textures[1]; }

private:
    HdVkGLInterop(Hgi *hgi, std::unique_ptr<vkr::SharedFrame> shared);
    void _Import();  // (re)imports the shared images
    void _Release();

    Hgi *_hgi;
    std::unique_ptr<vkr::SharedFrame> _shared;
    uint64_t _generation = 0;               // of the imported images
    uint32_t _width = 0, _height = 0;
    uint32_t _memory[2] = {}, _images[2] = {};  // GL: colour, depth
    uint32_t _ready = 0, _released = 0;     // GL semaphores
    HgiTextureHandle _textures[2];          // colour, depth in the app's Hgi
};

PXR_NAMESPACE_CLOSE_SCOPE
