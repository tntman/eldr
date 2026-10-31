#include "device.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {

// Every shader's SPIR-V hashed at build time (shader_hash.cmake): a
// pipeline cache file keyed by it holds no entries of other shader
// versions, and a build whose shaders didn't change keeps its file.
constexpr uint64_t kAllShaders =
#include "shader_hash.inc"
    ;

} // namespace

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
    need(f.features.textureCompressionBC, "textureCompressionBC");  // eldr:textureCompression
    need(f.features.independentBlend, "independentBlend");  // translucency: blended colour, plain ids
    need(f.features.geometryShader, "geometryShader");  // gl_PrimitiveID in mesh.frag (uniform primvars)
    need(f.features.fragmentStoresAndAtomics, "fragmentStoresAndAtomics");  // mesh.frag's light splits and denoised SSR rays
    need(f.features.dualSrcBlend, "dualSrcBlend");  // volume.frag: coloured transmittance over the image
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
        f.features.textureCompressionBC = VK_TRUE;
        f.features.independentBlend = VK_TRUE;
        f.features.geometryShader = VK_TRUE;
        f.features.fragmentStoresAndAtomics = VK_TRUE;
        f.features.dualSrcBlend = VK_TRUE;

        // Optional: the driver's memory budget (usdview's and Houdini's stats).
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, extensions.data());
        auto has = [&](const char *name) {
            return std::any_of(extensions.begin(), extensions.end(),
                               [&](const VkExtensionProperties &e) { return std::strcmp(e.extensionName, name) == 0; });
        };
        std::vector<const char *> enabled;
        memoryBudget = has(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        if (memoryBudget) {
            enabled.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        }

        // Optional: ray tracing pipelines (no ray queries needed; Pascal
        // runs them in NVIDIA's compute fallback).
        VkPhysicalDeviceAccelerationStructureFeaturesKHR fas{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR frt{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, &fas};
        VkPhysicalDeviceVulkan12Features has12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &frt};
        VkPhysicalDeviceFeatures2 hasFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &has12};
        vkGetPhysicalDeviceFeatures2(physical, &hasFeatures);
        VkFormatProperties r32;
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R32_SFLOAT, &r32);
        constexpr VkFormatFeatureFlags kMinmax =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        samplerMinmax = has12.samplerFilterMinmax && (r32.optimalTilingFeatures & kMinmax) == kMinmax;
        f12.samplerFilterMinmax = samplerMinmax ? VK_TRUE : VK_FALSE;
        const char *disable = std::getenv("HDVK_NO_RAY_TRACING");
        rayTracing = !(disable && std::strcmp(disable, "1") == 0) && has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                     has(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) &&
                     has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) && fas.accelerationStructure &&
                     frt.rayTracingPipeline && has12.bufferDeviceAddress;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR eas{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR ert{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, &eas};
        if (rayTracing) {
            eas.accelerationStructure = VK_TRUE;
            ert.rayTracingPipeline = VK_TRUE;
            f13.pNext = &ert;
            f12.bufferDeviceAddress = VK_TRUE;
            enabled.insert(enabled.end(), {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                           VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
                                           VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME});
            rayTracingInputUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        }

        // Optional: GPU sharing with OpenGL (usdview), see interop.h.
        const char *win32[2] = {"VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"};
        externalWin32 = has(win32[0]) && has(win32[1]);
        if (externalWin32) {
            enabled.insert(enabled.end(), win32, win32 + 2);
        }

        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = uint32_t(enabled.size());
        dci.ppEnabledExtensionNames = enabled.data();
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
        SavePipelineCache();
        vkDestroyPipelineCache(device, pipelineCache, nullptr);
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
                                VkMemoryPropertyFlags preferred, bool texture, const void *next)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    for (const VkMemoryPropertyFlags wanted : {props | preferred, props}) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & wanted) == wanted) {
                // Ray tracing reads buffers through device addresses.
                VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, next};
                flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
                VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                        rayTracing ? static_cast<const void *>(&flags) : next};
                ai.allocationSize = req.size;
                ai.memoryTypeIndex = i;
                VkDeviceMemory memory = VK_NULL_HANDLE;
                Check(vkAllocateMemory(device, &ai, nullptr, &memory), "vkAllocateMemory");
                const bool local = mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                static bool told = false;
                if ((preferred & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) && !told && std::getenv("HDVK_TIMING")) {
                    told = true;
                    std::fprintf(stderr, "HdVk readback memory: type %u, flags 0x%x (host cached %d)\n", i,
                                 unsigned(mp.memoryTypes[i].propertyFlags),
                                 int((mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0));
                }
                std::lock_guard<std::mutex> lock(_memoryMutex);
                _allocations[memory] = {req.size, local, texture, i};
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
    WaitFrame();
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
    {
        std::lock_guard<std::mutex> lock(_memoryMutex);
        auto best = _recycled.end();
        for (auto it = _recycled.begin(); _recycler == std::this_thread::get_id() && it != _recycled.end(); ++it) {
            const Allocation &a = _allocations.at(*it);
            if (a.size >= req.size && (req.memoryTypeBits & (1u << a.type)) &&
                (best == _recycled.end() || a.size < _allocations.at(*best).size)) {
                best = it;
            }
        }
        if (best != _recycled.end()) {
            img.memory = *best;
            _recycled.erase(best);
        }
    }
    if (!img.memory) {
        if (_recycler == std::this_thread::get_id() && recycleGrowth > 1.0) {
            const VkDeviceSize a = std::max<VkDeviceSize>(req.alignment, 1);
            req.size = (VkDeviceSize(double(req.size) * recycleGrowth) + a - 1) / a * a;
        }
        img.memory = Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
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
    if (img.image) {
        WaitFrame();
    }
    vkDestroyImageView(device, img.view, nullptr);
    vkDestroyImage(device, img.image, nullptr);
    bool kept = false;
    if (img.memory) {
        std::lock_guard<std::mutex> lock(_memoryMutex);
        const auto it = _allocations.find(img.memory);
        kept = _recycler == std::this_thread::get_id() && it != _allocations.end() && it->second.deviceLocal && !it->second.texture;
        if (kept) {
            _recycled.push_back(img.memory);
        }
    }
    if (!kept) {
        Free(img.memory);
    }
    img = Image();
}

void Device::BeginRecycle()
{
    std::lock_guard<std::mutex> lock(_memoryMutex);
    _recycler = std::this_thread::get_id();
}

void Device::EndRecycle()
{
    std::vector<VkDeviceMemory> left;
    {
        std::lock_guard<std::mutex> lock(_memoryMutex);
        _recycler = std::thread::id();
        recycleGrowth = 1.0;
        left.swap(_recycled);
    }
    for (VkDeviceMemory m : left) {
        Free(m);
    }
}

void Device::Destroy(Buffer &b)
{
    if (b.buffer) {
        WaitFrame();
        vkDestroyBuffer(device, b.buffer, nullptr);
    }
    Free(b.memory);
    b = Buffer();
}

std::filesystem::path CacheFolder()
{
    const char *dir = std::getenv("HDVK_PIPELINE_CACHE_DIR");
    const char *local = std::getenv("LOCALAPPDATA");
    return dir ? std::filesystem::path(dir)
         : local ? std::filesystem::path(local) / "Eldr"
                 : std::filesystem::temp_directory_path() / "Eldr";
}

void Device::LoadPipelineCache(uint64_t shaderHash)
{
    if (pipelineCache) {
        return;
    }
    const std::filesystem::path folder = CacheFolder();
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    char name[96];
    int n = std::snprintf(name, sizeof(name), "pipelines-");
    for (uint8_t b : props.pipelineCacheUUID) {
        n += std::snprintf(name + n, sizeof(name) - n, "%02x", b);
    }
    std::snprintf(name + n, sizeof(name) - n, "-%016llx.bin",
                  static_cast<unsigned long long>(shaderHash ^ kAllShaders * 0x9E3779B97F4A7C15ull));
    _pipelineCachePath = (folder / name).string();

    // The driver checks the header (vendor, device, cache UUID) and starts
    // empty on data that isn't its own.
    std::vector<char> data;
    if (std::ifstream in{_pipelineCachePath, std::ios::binary}) {
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(device, &ci, nullptr, &pipelineCache) != VK_SUCCESS) {
        ci.initialDataSize = 0;  // unreadable data: start empty
        ci.pInitialData = nullptr;
        Check(vkCreatePipelineCache(device, &ci, nullptr, &pipelineCache), "vkCreatePipelineCache");
        data.clear();
    }
    pipelineCacheBytes = _savedCacheBytes = data.size();
}

void Device::SavePipelineCache()
{
    if (!pipelineCache) {
        return;
    }
    size_t size = 0;
    if (vkGetPipelineCacheData(device, pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0) {
        return;
    }
    // Nothing compiled since the last load or save (the cache only grows):
    // no write, so leaving is quick.
    bool unchanged;
    {
        std::lock_guard<std::mutex> lock(_pipelineCacheMutex);
        unchanged = size == _savedCacheBytes;
    }
    std::vector<char> data(unchanged ? 0 : size);
    if (!unchanged && vkGetPipelineCacheData(device, pipelineCache, &size, data.data()) != VK_SUCCESS) {
        return;
    }
    // Written aside, then moved over the old file: a crash mid-write leaves
    // the previous cache.
    std::lock_guard<std::mutex> lock(_pipelineCacheMutex);
    std::error_code error;
    const std::filesystem::path path(_pipelineCachePath), temp = path.string() + ".tmp";
    if (!unchanged) {
        std::filesystem::create_directories(path.parent_path(), error);
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(data.data(), std::streamsize(size));
            if (!out) {
                return;
            }
        }
        std::filesystem::rename(temp, path, error);
        if (error) {
            return;
        }
        _savedCacheBytes = size;
    }
    // Per driver, this build's file and the newest few of other builds stay
    // (a main and a few test builds share the folder); older ones are stale.
    constexpr size_t kOtherBuilds = 3;
    const std::string prefix = path.filename().string().substr(0, path.filename().string().rfind('-') + 1);
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> others;
    for (const auto &entry : std::filesystem::directory_iterator(path.parent_path(), error)) {
        const std::string file = entry.path().filename().string();
        if (entry.path() != path && file.compare(0, prefix.size(), prefix) == 0 && entry.path().extension() == ".bin") {
            others.emplace_back(entry.last_write_time(error), entry.path());
        }
    }
    std::sort(others.begin(), others.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (size_t i = kOtherBuilds; i < others.size(); ++i) {
        std::filesystem::remove(others[i].second, error);
    }
}

} // namespace vkr
