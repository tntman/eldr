#include "device.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace vkr {

void Check(VkResult r, const char *what)
{
    if (r != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed (VkResult " +
                                 std::to_string(r) + ")");
    }
}

// Keep in sync with the features enabled in Device::Device().
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
    need(f.features.drawIndirectFirstInstance, "drawIndirectFirstInstance");
    need(f12.drawIndirectCount, "drawIndirectCount");
    need(f12.runtimeDescriptorArray, "runtimeDescriptorArray");
    need(f12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");
    need(f12.shaderSampledImageArrayNonUniformIndexing,
         "shaderSampledImageArrayNonUniformIndexing");
    need(f13.dynamicRendering, "dynamicRendering");
    need(f13.synchronization2, "synchronization2");
    return missing;
}

static VkInstance CreateInstance()
{
    uint32_t loaderVersion = 0;
    if (vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS ||
        loaderVersion < VK_API_VERSION_1_3) {
        throw std::runtime_error("Vulkan loader older than 1.3");
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "hdVk";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    Check(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance");
    return instance;
}

// Prefers a discrete GPU. Returns null and fills info->reasonWhyNot if none fit.
static VkPhysicalDevice PickDevice(VkInstance instance, DeviceInfo *info)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    bool bestDiscrete = false;
    std::string why;
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
        if (!best || (discrete && !bestDiscrete)) {
            best = pd;
            bestDiscrete = discrete;
            info->name = p.deviceName;
            info->apiVersion = p.apiVersion;
        }
    }
    info->supported = best != VK_NULL_HANDLE;
    if (!best) {
        info->reasonWhyNot = devices.empty() ? "no Vulkan devices" : why;
    }
    return best;
}

DeviceInfo ProbeDevice()
{
    DeviceInfo info;
    try {
        VkInstance instance = CreateInstance();
        PickDevice(instance, &info);
        vkDestroyInstance(instance, nullptr);
    } catch (const std::exception &e) {
        info.reasonWhyNot = e.what();
    }
    return info;
}

Device::Device()
{
    try {
        instance = CreateInstance();
        DeviceInfo info;
        physical = PickDevice(instance, &info);
        if (!physical) {
            throw std::runtime_error(info.reasonWhyNot);
        }

        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, families.data());
        queueFamily = uint32_t(std::find_if(families.begin(), families.end(),
            [](const VkQueueFamilyProperties &q) {
                return (q.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            }) - families.begin());
        if (queueFamily == n) {
            throw std::runtime_error("no graphics queue");
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;

        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.dynamicRendering = VK_TRUE;
        f13.synchronization2 = VK_TRUE;
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
        f12.drawIndirectCount = VK_TRUE;
        f12.runtimeDescriptorArray = VK_TRUE;
        f12.descriptorBindingPartiallyBound = VK_TRUE;
        f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
        f.features.multiDrawIndirect = VK_TRUE;
        f.features.drawIndirectFirstInstance = VK_TRUE;

        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        Check(vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
    } catch (...) {
        _Release();
        throw;
    }
}

Device::~Device()
{
    _Release();
}

void Device::_Release()
{
    if (device) {
        vkDestroyDevice(device, nullptr);
    }
    if (instance) {
        vkDestroyInstance(instance, nullptr);
    }
    device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
}

VkDeviceMemory Device::Allocate(VkMemoryRequirements req, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) {
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = i;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            Check(vkAllocateMemory(device, &ai, nullptr, &memory), "vkAllocateMemory");
            return memory;
        }
    }
    throw std::runtime_error("no suitable Vulkan memory type");
}

// ponytail: host-visible memory for everything, GPU reads over PCIe.
// Device-local + staging ring in step 6 when scenes get big.
void Device::Reserve(Buffer &b, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
    if (b.size >= bytes) {
        return;
    }
    const VkDeviceSize size = std::max<VkDeviceSize>({bytes, b.size + b.size / 2, 256});
    Destroy(b);

    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    Check(vkCreateBuffer(device, &ci, nullptr, &b.buffer), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    b.memory = Allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkBindBufferMemory(device, b.buffer, b.memory, 0), "vkBindBufferMemory");
    Check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
    b.size = size;
}

void Device::Destroy(Buffer &b)
{
    if (b.buffer) {
        vkDestroyBuffer(device, b.buffer, nullptr);
    }
    if (b.memory) {
        vkFreeMemory(device, b.memory, nullptr);
    }
    b = Buffer();
}

} // namespace vkr
