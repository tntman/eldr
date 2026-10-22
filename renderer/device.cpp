#include "device.h"

#include <vulkan/vulkan.h>

#include <vector>

namespace vkr {

static std::string MissingFeatures(VkPhysicalDevice pd)
{
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    vkGetPhysicalDeviceFeatures2(pd, &f);

    std::string missing;
    auto need = [&](VkBool32 has, const char *name) {
        if (!has) {
            missing += missing.empty() ? name : std::string(", ") + name;
        }
    };
    need(f.features.multiDrawIndirect, "multiDrawIndirect");
    need(f12.drawIndirectCount, "drawIndirectCount");
    need(f12.runtimeDescriptorArray, "runtimeDescriptorArray");
    need(f12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");
    need(f12.shaderSampledImageArrayNonUniformIndexing,
         "shaderSampledImageArrayNonUniformIndexing");
    need(f13.dynamicRendering, "dynamicRendering");
    need(f13.synchronization2, "synchronization2");
    return missing;
}

DeviceInfo ProbeDevice()
{
    DeviceInfo info;
    std::string why;

    uint32_t loaderVersion = 0;
    if (vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS ||
        loaderVersion < VK_API_VERSION_1_3) {
        info.reasonWhyNot = "Vulkan loader older than 1.3";
        return info;
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "hdVk";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) {
        info.reasonWhyNot = "vkCreateInstance failed";
        return info;
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    bool haveDiscrete = false;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        if (p.apiVersion < VK_API_VERSION_1_3) {
            why += std::string(p.deviceName) + ": Vulkan < 1.3; ";
            continue;
        }
        const std::string missing = MissingFeatures(pd);
        if (!missing.empty()) {
            why += std::string(p.deviceName) + " lacks " + missing + "; ";
            continue;
        }
        const bool discrete = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (!info.supported || (discrete && !haveDiscrete)) {
            info.supported = true;
            info.name = p.deviceName;
            info.apiVersion = p.apiVersion;
            haveDiscrete = discrete;
        }
    }
    vkDestroyInstance(instance, nullptr);

    if (!info.supported) {
        info.reasonWhyNot = devices.empty() ? "no Vulkan devices" : why;
    }
    return info;
}

} // namespace vkr
