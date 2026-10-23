#include "device.h"

#include <algorithm>
#include <cstring>
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

VkShaderModule CreateShaderModule(VkDevice device, const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    Check(vkCreateShaderModule(device, &ci, nullptr, &module), "vkCreateShaderModule");
    return module;
}

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageSubresourceRange range,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                  VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = range;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
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
    need(f.features.samplerAnisotropy, "samplerAnisotropy");
    need(f.features.independentBlend, "independentBlend");  // translucency: blended colour, plain ids
    need(f12.drawIndirectCount, "drawIndirectCount");
    need(f12.runtimeDescriptorArray, "runtimeDescriptorArray");
    need(f12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");
    need(f12.shaderSampledImageArrayNonUniformIndexing,
         "shaderSampledImageArrayNonUniformIndexing");
    need(f12.separateDepthStencilLayouts, "separateDepthStencilLayouts");
    need(f13.dynamicRendering, "dynamicRendering");
    need(f13.synchronization2, "synchronization2");
    need(f13.shaderDemoteToHelperInvocation, "shaderDemoteToHelperInvocation");  // discard (cutouts)
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
        f13.shaderDemoteToHelperInvocation = VK_TRUE;
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
        f12.drawIndirectCount = VK_TRUE;
        f12.runtimeDescriptorArray = VK_TRUE;
        f12.descriptorBindingPartiallyBound = VK_TRUE;
        f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        f12.separateDepthStencilLayouts = VK_TRUE;
        VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
        f.features.multiDrawIndirect = VK_TRUE;
        f.features.drawIndirectFirstInstance = VK_TRUE;
        f.features.samplerAnisotropy = VK_TRUE;
        f.features.independentBlend = VK_TRUE;

        // Optional: the driver's memory budget (usdview's and Houdini's stats).
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, extensions.data());
        const char *budget = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
        memoryBudget = std::any_of(extensions.begin(), extensions.end(), [&](const VkExtensionProperties &e) {
            return std::strcmp(e.extensionName, budget) == 0;
        });

        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = memoryBudget ? 1 : 0;
        dci.ppEnabledExtensionNames = &budget;
        Check(vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, queueFamily, 0, &queue);

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = queueFamily;
        Check(vkCreateCommandPool(device, &pci, nullptr, &_submitPool), "vkCreateCommandPool");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(vkCreateFence(device, &fci, nullptr, &_submitFence), "vkCreateFence");
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
        vkDestroyFence(device, _submitFence, nullptr);
        vkDestroyCommandPool(device, _submitPool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if (instance) {
        vkDestroyInstance(instance, nullptr);
    }
    device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
}

VkDeviceMemory Device::Allocate(VkMemoryRequirements req, VkMemoryPropertyFlags props,
                                VkMemoryPropertyFlags preferred, bool texture)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    for (const VkMemoryPropertyFlags wanted : {props | preferred, props}) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & wanted) == wanted) {
                VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                ai.allocationSize = req.size;
                ai.memoryTypeIndex = i;
                VkDeviceMemory memory = VK_NULL_HANDLE;
                Check(vkAllocateMemory(device, &ai, nullptr, &memory), "vkAllocateMemory");
                const bool local = mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                std::lock_guard<std::mutex> lock(_memoryMutex);
                _allocations[memory] = {req.size, local, texture};
                (local ? _memory.deviceLocal : _memory.host) += req.size;
                _memory.textures += texture ? req.size : 0;
                _memory.peakDeviceLocal = std::max(_memory.peakDeviceLocal, _memory.deviceLocal);
                return memory;
            }
        }
    }
    throw std::runtime_error("no suitable Vulkan memory type");
}

// ponytail: host-visible memory for everything, GPU reads over PCIe.
// Device-local + staging ring in step 6 when scenes get big.
void Device::Reserve(Buffer &b, VkDeviceSize bytes, VkBufferUsageFlags usage,
                     VkMemoryPropertyFlags preferred)
{
    if (b.buffer && b.size >= bytes) {
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
    b.memory = Allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        preferred);
    Check(vkBindBufferMemory(device, b.buffer, b.memory, 0), "vkBindBufferMemory");
    Check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
    b.size = size;
}

void Device::Submit(const std::function<void(VkCommandBuffer)> &record)
{
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = _submitPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    Check(vkAllocateCommandBuffers(device, &ai, &cmd), "vkAllocateCommandBuffers");

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult r = vkBeginCommandBuffer(cmd, &bi);
    if (r == VK_SUCCESS) {
        record(cmd);
        r = vkEndCommandBuffer(cmd);
    }
    if (r == VK_SUCCESS) {
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        r = vkQueueSubmit2(queue, 1, &si, _submitFence);
        if (r == VK_SUCCESS) {
            r = vkWaitForFences(device, 1, &_submitFence, VK_TRUE, UINT64_MAX);
            vkResetFences(device, 1, &_submitFence);
        }
    }
    vkFreeCommandBuffers(device, _submitPool, 1, &cmd);
    Check(r, "Device::Submit");
}

Buffer Device::CreateDeviceBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage)
{
    Buffer b;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = std::max<VkDeviceSize>(bytes, 256);
    ci.usage = usage;
    Check(vkCreateBuffer(device, &ci, nullptr, &b.buffer), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    b.memory = Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkBindBufferMemory(device, b.buffer, b.memory, 0), "vkBindBufferMemory");
    b.size = ci.size;
    return b;
}

Image Device::CreateImage(VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                          uint32_t mips, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                          VkImageViewType viewType, VkImageCreateFlags flags)
{
    Image img;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = flags;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {width, height, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    Check(vkCreateImage(device, &ci, nullptr, &img.image), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, img.image, &req);
    img.memory = Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkBindImageMemory(device, img.image, img.memory, 0), "vkBindImageMemory");

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = viewType;
    vci.format = format;
    vci.subresourceRange = {aspect, 0, mips, 0, layers};
    Check(vkCreateImageView(device, &vci, nullptr, &img.view), "vkCreateImageView");
    return img;
}

void Device::Free(VkDeviceMemory memory)
{
    if (!memory) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_memoryMutex);
        const auto it = _allocations.find(memory);
        if (it != _allocations.end()) {
            const Allocation &a = it->second;
            (a.deviceLocal ? _memory.deviceLocal : _memory.host) -= a.size;
            _memory.textures -= a.texture ? a.size : 0;
            _allocations.erase(it);
        }
    }
    vkFreeMemory(device, memory, nullptr);
}

Device::MemoryStats Device::Memory() const
{
    MemoryStats stats;
    {
        std::lock_guard<std::mutex> lock(_memoryMutex);
        stats = _memory;
    }
    if (memoryBudget) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 mp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};
        vkGetPhysicalDeviceMemoryProperties2(physical, &mp);
        for (uint32_t h = 0; h < mp.memoryProperties.memoryHeapCount; ++h) {
            if (mp.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                stats.heapSize += mp.memoryProperties.memoryHeaps[h].size;
                stats.heapUsage += budget.heapUsage[h];
                stats.heapBudget += budget.heapBudget[h];
            }
        }
    }
    return stats;
}

void Device::Destroy(Image &img)
{
    vkDestroyImageView(device, img.view, nullptr);
    vkDestroyImage(device, img.image, nullptr);
    Free(img.memory);
    img = Image();
}

void Device::Destroy(Buffer &b)
{
    if (b.buffer) {
        vkDestroyBuffer(device, b.buffer, nullptr);
    }
    Free(b.memory);
    b = Buffer();
}

} // namespace vkr
