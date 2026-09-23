// Opt-in real GPU test for the custom Dawn DLL. No headset required.
// Exercises the MinGW/MSVC C ABI, borrowed-image lifetime and repeated layout
// transitions. Run with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation as well.
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>
#include <dawn/webgpu_cpp.h>
#include <aurora/dawn_vulkan_abi.h>
#include <array>
#include <cstdio>
#include <cstdlib>

static void Check(bool ok) { if (!ok) { std::fputs("Native Vulkan bridge smoke failed\n", stderr); std::abort(); } }
static PFN_vkGetInstanceProcAddr hookProc;
static VkInstance hookInstance;
static int instances = 0, devices = 0, selections = 0;
static int32_t CreateInstance(void*, void* proc, const void* info, const void* allocator, void** out) {
    ++instances; hookProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(proc);
    auto create = reinterpret_cast<PFN_vkCreateInstance>(hookProc(nullptr, "vkCreateInstance"));
    auto result = create(static_cast<const VkInstanceCreateInfo*>(info), static_cast<const VkAllocationCallbacks*>(allocator), &hookInstance);
    *out = hookInstance; return result;
}
static int32_t CreateDevice(void*, void*, void* physical, const void* info, const void* allocator, void** out) {
    ++devices;
    auto create = reinterpret_cast<PFN_vkCreateDevice>(hookProc(hookInstance, "vkCreateDevice"));
    return create(static_cast<VkPhysicalDevice>(physical), static_cast<const VkDeviceCreateInfo*>(info),
                  static_cast<const VkAllocationCallbacks*>(allocator), reinterpret_cast<VkDevice*>(out));
}
static int32_t SelectPhysical(void*, void* instance, void** out) {
    ++selections;
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(hookProc(static_cast<VkInstance>(instance), "vkEnumeratePhysicalDevices"));
    uint32_t count = 1;
    auto result = enumerate(static_cast<VkInstance>(instance), &count, reinterpret_cast<VkPhysicalDevice*>(out));
    return result == VK_INCOMPLETE ? VK_SUCCESS : result;
}
int main() {
    auto dll = LoadLibraryW(L"webgpu_dawn.dll"); Check(dll != nullptr);
#define API(name, type) auto name = reinterpret_cast<type>(GetProcAddress(dll, "AuroraDawnVulkan" #name)); Check(name != nullptr)
    API(Version, AuroraDawnVulkanVersionFn);
    API(Configure, AuroraDawnVulkanConfigureFn);
    API(GetHandles, AuroraDawnVulkanHandlesFn);
    API(Wrap, AuroraDawnVulkanWrapFn);
    API(Release, AuroraDawnVulkanReleaseFn);
    API(Lock, AuroraDawnVulkanLockFn);
    API(Unlock, AuroraDawnVulkanUnlockFn);
    API(Drain, AuroraDawnVulkanDrainFn);
    Check(Version() == AURORA_DAWN_VULKAN_ABI);
    AuroraDawnVulkanHooks hooks{nullptr, CreateInstance, CreateDevice, SelectPhysical};
    Check(Configure(&hooks));
    wgpu::InstanceFeatureName feature = wgpu::InstanceFeatureName::TimedWaitAny;
    wgpu::InstanceDescriptor instanceDesc;
    instanceDesc.requiredFeatureCount = 1; instanceDesc.requiredFeatures = &feature;
    std::fputs("Creating WebGPU instance\n", stderr);
    auto instance = wgpu::CreateInstance(&instanceDesc);
    wgpu::RequestAdapterOptions options; options.backendType = wgpu::BackendType::Vulkan;
    wgpu::Adapter adapter;
    std::fputs("Requesting Vulkan adapter\n", stderr);
    auto future = instance.RequestAdapter(&options, wgpu::CallbackMode::WaitAnyOnly,
        [&](wgpu::RequestAdapterStatus status, wgpu::Adapter value, wgpu::StringView) {
            Check(status == wgpu::RequestAdapterStatus::Success); adapter = std::move(value);
        });
    Check(instance.WaitAny(future, 10'000'000'000) == wgpu::WaitStatus::Success);
    wgpu::FeatureName sync = wgpu::FeatureName::ImplicitDeviceSynchronization;
    wgpu::DeviceDescriptor deviceDesc;
    deviceDesc.requiredFeatureCount = 1; deviceDesc.requiredFeatures = &sync;
    deviceDesc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message) {
        std::fprintf(stderr, "Dawn: %.*s\n", static_cast<int>(message.length), message.data); Check(false);
    });
    std::fputs("Creating Vulkan device\n", stderr);
    auto device = adapter.CreateDevice(&deviceDesc); Check(!!device);
    Check(instances > 0 && devices > 0 && selections > 0);
    std::fputs("Getting native device\n", stderr);
    AuroraDawnVulkanHandles handles{}; Check(GetHandles(device.Get(), &handles));
    VkDevice vkDevice = static_cast<VkDevice>(handles.device);
    VkPhysicalDevice physical = static_cast<VkPhysicalDevice>(handles.physicalDevice);
    auto loader = LoadLibraryW(L"vulkan-1.dll"); Check(loader != nullptr);
    auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
#define VK(name) auto name = reinterpret_cast<PFN_##name>(getProc(static_cast<VkInstance>(handles.instance), #name)); Check(name != nullptr)
    VK(vkCreateImage); VK(vkGetImageMemoryRequirements); VK(vkGetPhysicalDeviceMemoryProperties);
    VK(vkAllocateMemory); VK(vkBindImageMemory); VK(vkCreateCommandPool); VK(vkAllocateCommandBuffers);
    VK(vkBeginCommandBuffer); VK(vkCmdPipelineBarrier); VK(vkEndCommandBuffer); VK(vkGetDeviceQueue);
    VK(vkQueueSubmit); VK(vkQueueWaitIdle); VK(vkDestroyCommandPool); VK(vkDestroyImage); VK(vkFreeMemory);
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D; imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {64, 16, 1}; imageInfo.mipLevels = 1; imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT; imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    std::fputs("Creating borrowed Vulkan image\n", stderr);
    VkImage image; Check(vkCreateImage(vkDevice, &imageInfo, nullptr, &image) == VK_SUCCESS);
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(vkDevice, image, &requirements);
    VkPhysicalDeviceMemoryProperties properties; vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    uint32_t memoryType = 0;
    while (!(requirements.memoryTypeBits & (1u << memoryType))) ++memoryType;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = memoryType;
    VkDeviceMemory memory; Check(vkAllocateMemory(vkDevice, &allocation, nullptr, &memory) == VK_SUCCESS);
    Check(vkBindImageMemory(vkDevice, image, memory, 0) == VK_SUCCESS);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; poolInfo.queueFamilyIndex = handles.queueFamily;
    VkCommandPool pool; Check(vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool; alloc.commandBufferCount = 1; alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer commands; Check(vkAllocateCommandBuffers(vkDevice, &alloc, &commands) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Check(vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    Check(vkEndCommandBuffer(commands) == VK_SUCCESS);
    VkQueue queue; vkGetDeviceQueue(vkDevice, handles.queueFamily, handles.queueIndex, &queue);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &commands;
    std::fputs("Locking native queue\n", stderr);
    auto guard = Lock(device.Get());
    Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    Check(vkQueueWaitIdle(queue) == VK_SUCCESS); Unlock(guard);
    wgpu::TextureDescriptor textureDesc;
    textureDesc.size = {64, 16, 1}; textureDesc.format = wgpu::TextureFormat::RGBA8Unorm;
    textureDesc.usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::RenderAttachment;
    std::fputs("Wrapping borrowed image\n", stderr);
    auto borrowed = wgpu::Texture::Acquire(static_cast<WGPUTexture>(Wrap(device.Get(), &textureDesc, reinterpret_cast<uint64_t>(image))));
    Check(!!borrowed);
    std::fputs("Creating copy resources\n", stderr);
    auto source = device.CreateTexture(&textureDesc);
    wgpu::BufferDescriptor bufferDesc; bufferDesc.size = 4096;
    bufferDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    auto readback = device.CreateBuffer(&bufferDesc);
    std::fputs("Running GPU copy\n", stderr);
    for (uint8_t value : {17, 99, 201}) {
        std::array<uint8_t, 4096> pixels; pixels.fill(value);
        wgpu::TexelCopyTextureInfo sourceInfo; sourceInfo.texture = source;
        wgpu::TexelCopyTextureInfo targetInfo; targetInfo.texture = borrowed;
        wgpu::TexelCopyBufferLayout layout; layout.bytesPerRow = 256; layout.rowsPerImage = 16;
        wgpu::Extent3D extent{64, 16, 1};
        device.GetQueue().WriteTexture(&sourceInfo, pixels.data(), pixels.size(), &layout, &extent);
        std::fputs("WriteTexture done\n", stderr);
        auto encoder = device.CreateCommandEncoder(); encoder.CopyTextureToTexture(&sourceInfo, &targetInfo, &extent);
        auto copy = encoder.Finish(); device.GetQueue().Submit(1, &copy);
        std::fputs("Copy submitted\n", stderr);
        void* textures[] = {borrowed.Get()}; Check(Release(device.Get(), textures, 1));
        std::fputs("Release transition done\n", stderr);
        encoder = device.CreateCommandEncoder();
        wgpu::TexelCopyBufferInfo destination; destination.buffer = readback; destination.layout = layout;
        encoder.CopyTextureToBuffer(&targetInfo, &destination, &extent);
        copy = encoder.Finish(); device.GetQueue().Submit(1, &copy);
        Check(Release(device.Get(), textures, 1));
        auto mapped = readback.MapAsync(wgpu::MapMode::Read, 0, 4096, wgpu::CallbackMode::WaitAnyOnly,
            [](wgpu::MapAsyncStatus status, wgpu::StringView) { Check(status == wgpu::MapAsyncStatus::Success); });
        Check(instance.WaitAny(mapped, 10'000'000'000) == wgpu::WaitStatus::Success);
        const auto* bytes = static_cast<const uint8_t*>(readback.GetConstMappedRange());
        for (size_t i = 0; i < pixels.size(); ++i) Check(bytes[i] == value);
        readback.Unmap();
    }
    Check(Drain(device.Get())); borrowed = nullptr;
    // The wrapper must not free the runtime-owned image or its memory.
    vkDestroyImage(vkDevice, image, nullptr); vkFreeMemory(vkDevice, memory, nullptr);
    vkDestroyCommandPool(vkDevice, pool, nullptr);
    Check(Configure(nullptr));
    std::puts("Native Vulkan bridge: three GPU copy/readback cycles passed");
}
