#include "glInterop.h"

#include "interop.h"
#include "renderer.h"

#include "pxr/base/gf/vec3i.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/imaging/hgi/hgi.h"
#include "pxr/imaging/hgi/tokens.h"

#include <algorithm>
#include <cstring>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

constexpr GLenum kGLFormats[2] = {GL_RGBA16F, GL_DEPTH_COMPONENT32F};
constexpr HgiFormat kHgiFormats[2] = {HgiFormatFloat16Vec4, HgiFormatFloat32};

// Errors the app left behind aren't ours (bounded: without a context
// glGetError may never clear).
void ClearGLErrors()
{
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
    }
}

} // namespace

std::unique_ptr<HdVkGLInterop> HdVkGLInterop::Create(Hgi *hgi, vkr::Device &device)
{
    if (!hgi || hgi->GetAPIName() != HgiTokens->OpenGL || !GarchGLApiLoad()) {
        return nullptr;
    }
    if (!GARCH_GLAPI_HAS(EXT_memory_object) || !GARCH_GLAPI_HAS(EXT_memory_object_win32) ||
        !GARCH_GLAPI_HAS(EXT_semaphore) || !GARCH_GLAPI_HAS(EXT_semaphore_win32) ||
        !(GARCH_GLAPI_HAS(VERSION_4_3) || GARCH_GLAPI_HAS(ARB_copy_image))) {
        return nullptr;
    }
    std::unique_ptr<vkr::SharedFrame> shared = vkr::SharedFrame::Create(device);
    if (!shared) {
        return nullptr;
    }
    // Semaphores only work between the same GPU and driver
    // (GL_EXT_external_objects).
    ClearGLErrors();
    GLint uuids = 0;
    glGetIntegerv(GL_NUM_DEVICE_UUIDS_EXT, &uuids);
    GLubyte deviceUuid[GL_UUID_SIZE_EXT] = {}, driverUuid[GL_UUID_SIZE_EXT] = {};
    if (uuids == 1) {
        glGetUnsignedBytei_vEXT(GL_DEVICE_UUID_EXT, 0, deviceUuid);
        glGetUnsignedBytevEXT(GL_DRIVER_UUID_EXT, driverUuid);
    }
    if (uuids != 1 || std::memcmp(deviceUuid, shared->DeviceUuid(), GL_UUID_SIZE_EXT) != 0 ||
        std::memcmp(driverUuid, shared->DriverUuid(), GL_UUID_SIZE_EXT) != 0) {
        return nullptr;
    }
    const vkr::SharedFrame::Handles handles = shared->GetHandles();
    std::unique_ptr<HdVkGLInterop> interop(new HdVkGLInterop(hgi, std::move(shared)));
    glGenSemaphoresEXT(1, &interop->_ready);
    glImportSemaphoreWin32HandleEXT(interop->_ready, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handles.ready);
    glGenSemaphoresEXT(1, &interop->_released);
    glImportSemaphoreWin32HandleEXT(interop->_released, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handles.released);
    return glGetError() == GL_NO_ERROR ? std::move(interop) : nullptr;
}

HdVkGLInterop::HdVkGLInterop(Hgi *hgi, std::unique_ptr<vkr::SharedFrame> shared)
    : _hgi(hgi)
    , _shared(std::move(shared))
{
}

HdVkGLInterop::~HdVkGLInterop()
{
    _Release();
    glDeleteSemaphoresEXT(1, &_ready);
    glDeleteSemaphoresEXT(1, &_released);
}

void HdVkGLInterop::_Release()
{
    glDeleteTextures(2, _images);
    glDeleteMemoryObjectsEXT(2, _memory);
    std::fill(_images, _images + 2, 0u);
    std::fill(_memory, _memory + 2, 0u);
    for (HgiTextureHandle &t : _textures) {
        if (t) {
            _hgi->DestroyTexture(&t);
        }
    }
}

void HdVkGLInterop::_Import()
{
    _Release();
    const vkr::SharedFrame::Handles &handles = _shared->GetHandles();
    void *const memory[2] = {handles.color, handles.depth};
    const GLuint64 bytes[2] = {handles.colorBytes, handles.depthBytes};
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glCreateMemoryObjectsEXT(2, _memory);
    glGenTextures(2, _images);
    for (int i = 0; i < 2; ++i) {
        const GLint dedicated = GL_TRUE;
        glMemoryObjectParameterivEXT(_memory[i], GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
        glImportMemoryWin32HandleEXT(_memory[i], bytes[i], GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, memory[i]);
        glBindTexture(GL_TEXTURE_2D, _images[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
        glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, kGLFormats[i], GLsizei(_width), GLsizei(_height), _memory[i], 0);

        HgiTextureDesc desc;
        desc.debugName = i == 0 ? "HdVk colour" : "HdVk depth";
        desc.format = kHgiFormats[i];
        desc.usage = (i == 0 ? HgiTextureUsageBitsColorTarget : HgiTextureUsageBitsDepthTarget) |
                     HgiTextureUsageBitsShaderRead;
        desc.dimensions = GfVec3i(int(_width), int(_height), 1);
        _textures[i] = _hgi->CreateTexture(desc);
    }
    glBindTexture(GL_TEXTURE_2D, GLuint(bound));
    _generation = _shared->Generation();
}

bool HdVkGLInterop::Present(vkr::Renderer &renderer)
{
    if (!renderer.ColorImage()) {
        return false;
    }
    ClearGLErrors();
    _width = renderer.Width();
    _height = renderer.Height();
    _shared->Copy(renderer.ColorImage(), renderer.DepthImage(), _width, _height);
    if (_shared->Generation() != _generation) {
        _Import();
    }
    // GL waits for the copy, copies the images into the Hgi textures and
    // signals that it is done with them (flushed: the next Copy waits).
    const GLenum layouts[2] = {GL_LAYOUT_TRANSFER_SRC_EXT, GL_LAYOUT_TRANSFER_SRC_EXT};
    glWaitSemaphoreEXT(_ready, 0, nullptr, 2, _images, layouts);
    for (int i = 0; i < 2; ++i) {
        glCopyImageSubData(_images[i], GL_TEXTURE_2D, 0, 0, 0, 0, GLuint(_textures[i]->GetRawResource()),
                           GL_TEXTURE_2D, 0, 0, 0, 0, GLsizei(_width), GLsizei(_height), 1);
    }
    glSignalSemaphoreEXT(_released, 0, nullptr, 2, _images, layouts);
    glFlush();
    _shared->Released();
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        TF_WARN("HdVk: sharing frames with OpenGL failed (0x%04x); reading them back instead", error);
        return false;
    }
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
