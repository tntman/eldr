// Headless check: plugin is discoverable, IsSupported passes, delegate constructs.
#include "pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h"
#include "pxr/imaging/hd/rendererCreateArgsSchema.h"
#include "pxr/imaging/hd/rendererPlugin.h"
#include "pxr/imaging/hd/rendererPluginHandle.h"
#include "pxr/imaging/hd/rendererPluginRegistry.h"

#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE

int main()
{
    const TfToken id("HdVkRendererPlugin");
    HdRendererPluginRegistry &registry = HdRendererPluginRegistry::GetInstance();

    HdRendererPluginHandle plugin = registry.GetOrCreateRendererPlugin(id);
    if (!plugin) {
        std::puts("FAIL: HdVkRendererPlugin not found, check PXR_PLUGINPATH_NAME");
        return 1;
    }
    std::string why;
#if HD_API_VERSION >= 103
    const HdRendererCreateArgsSchema args(nullptr);
#else
    const HdContainerDataSourceHandle args;  // Houdini 22's USD 26.05
#endif
    if (!plugin->IsSupported(args, &why)) {
        std::printf("FAIL: IsSupported returned false: %s\n", why.c_str());
        return 1;
    }
    if (!registry.CreateRenderDelegate(id)) {
        std::puts("FAIL: CreateRenderDelegate returned null");
        return 1;
    }
    std::printf("OK: '%s' registered, supported, delegate created\n",
                plugin->GetDisplayName().c_str());
    return 0;
}
