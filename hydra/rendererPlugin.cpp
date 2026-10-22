#include "renderDelegate.h"

#include "device.h"

#include "pxr/imaging/hd/rendererCreateArgsSchema.h"
#include "pxr/imaging/hd/rendererPlugin.h"
#include "pxr/imaging/hd/rendererPluginRegistry.h"

PXR_NAMESPACE_OPEN_SCOPE

class HdVkRendererPlugin final : public HdRendererPlugin
{
public:
    HdRenderDelegate *CreateRenderDelegate() override
    {
        return new HdVkRenderDelegate();
    }

    HdRenderDelegate *CreateRenderDelegate(HdRenderSettingsMap const &settings) override
    {
        return new HdVkRenderDelegate(settings);
    }

    void DeleteRenderDelegate(HdRenderDelegate *delegate) override
    {
        delete delegate;
    }

    // Signature verified against USD 26.08 (HD_API_VERSION 103). Older USD
    // (incl. possibly Houdini 22's) uses IsSupported(bool) or
    // IsSupported(HdRendererCreateArgs const&); gate on HD_API_VERSION in step 7.
    bool IsSupported(HdRendererCreateArgsSchema const &,
                     std::string *reasonWhyNot) const override
    {
        // Probe once per process; creating a VkInstance takes tens of ms.
        static const vkr::DeviceInfo info = vkr::ProbeDevice();
        if (!info.supported && reasonWhyNot) {
            *reasonWhyNot = info.reasonWhyNot;
        }
        return info.supported;
    }
};

TF_REGISTRY_FUNCTION(TfType)
{
    HdRendererPluginRegistry::Define<HdVkRendererPlugin>();
}

PXR_NAMESPACE_CLOSE_SCOPE
