// Standalone check of the renderer layer, no USD needed.
#include "device.h"

#include <vulkan/vulkan.h>

#include <cstdio>

int main()
{
    const vkr::DeviceInfo d = vkr::ProbeDevice();
    if (!d.supported) {
        std::printf("unsupported: %s\n", d.reasonWhyNot.c_str());
        return 1;
    }
    std::printf("%s, Vulkan %u.%u.%u\n", d.name.c_str(),
                VK_API_VERSION_MAJOR(d.apiVersion),
                VK_API_VERSION_MINOR(d.apiVersion),
                VK_API_VERSION_PATCH(d.apiVersion));
    return 0;
}
