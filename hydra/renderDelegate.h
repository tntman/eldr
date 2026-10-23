#pragma once

#include "pxr/pxr.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/imaging/hd/renderDelegate.h"

#include <memory>

namespace vkr {
class Device;
class Renderer;
class Scene;
}

PXR_NAMESPACE_OPEN_SCOPE

#define HDVK_RENDER_SETTING_TOKENS                           \
    ((postProcess, "hdvk:postProcess"))                      \
    ((taa, "hdvk:taa"))                                      \
    ((ambientOcclusion, "hdvk:ambientOcclusion"))            \
    ((aoRadius, "hdvk:aoRadius"))                            \
    ((ssr, "hdvk:ssr"))                                      \
    ((autoExposure, "hdvk:autoExposure"))                    \
    ((exposureCompensation, "hdvk:exposureCompensation"))    \
    ((bloomIntensity, "hdvk:bloomIntensity"))                \
    ((shadows, "hdvk:shadows"))                              \
    ((localShadows, "hdvk:localShadows"))                    \
    ((softShadows, "hdvk:softShadows"))                      \
    ((taaSamples, "hdvk:taaSamples"))                        \
    ((cullPixels, "hdvk:cullPixels"))

TF_DECLARE_PUBLIC_TOKENS(HdVkRenderSettingsTokens, HDVK_RENDER_SETTING_TOKENS);

class HdVkTextureCache;

class HdVkRenderDelegate final : public HdRenderDelegate
{
public:
    explicit HdVkRenderDelegate(HdRenderSettingsMap const &settings = {});
    ~HdVkRenderDelegate() override;

    const TfTokenVector &GetSupportedRprimTypes() const override;
    const TfTokenVector &GetSupportedSprimTypes() const override;
    const TfTokenVector &GetSupportedBprimTypes() const override;
    HdResourceRegistrySharedPtr GetResourceRegistry() const override;
    HdAovDescriptor GetDefaultAovDescriptor(TfToken const &name) const override;
    HdRenderSettingDescriptorList GetRenderSettingDescriptors() const override;
    TfTokenVector GetMaterialRenderContexts() const override;

    HdRenderPassSharedPtr CreateRenderPass(
        HdRenderIndex *index, HdRprimCollection const &collection) override;

    HdInstancer *CreateInstancer(HdSceneDelegate *delegate,
                                 SdfPath const &id) override;
    void DestroyInstancer(HdInstancer *instancer) override;

    HdRprim *CreateRprim(TfToken const &typeId, SdfPath const &rprimId) override;
    void DestroyRprim(HdRprim *rprim) override;

    HdSprim *CreateSprim(TfToken const &typeId, SdfPath const &sprimId) override;
    HdSprim *CreateFallbackSprim(TfToken const &typeId) override;
    void DestroySprim(HdSprim *sprim) override;

    HdBprim *CreateBprim(TfToken const &typeId, SdfPath const &bprimId) override;
    HdBprim *CreateFallbackBprim(TfToken const &typeId) override;
    void DestroyBprim(HdBprim *bprim) override;

    void CommitResources(HdChangeTracker *tracker) override;

private:
    HdResourceRegistrySharedPtr _resourceRegistry;
    // Declaration order is destruction order in reverse: renderer and scene
    // release GPU resources before the device goes away.
    std::unique_ptr<vkr::Device> _device;     // null if Vulkan init failed
    std::unique_ptr<vkr::Scene> _scene;       // always present (CPU mirror)
    std::unique_ptr<vkr::Renderer> _renderer; // null if Vulkan init failed
    std::unique_ptr<HdVkTextureCache> _textures;
};

PXR_NAMESPACE_CLOSE_SCOPE
