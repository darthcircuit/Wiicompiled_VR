#include <aurora/vulkan_interop.h>

#include "../internal.hpp"
#include "../stereo.hpp"
#include "gpu.hpp"

#if defined(__ANDROID__) && defined(WEBGPU_DAWN)

#include <android/hardware_buffer.h>
#include <magic_enum.hpp>
#include <poll.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aurora::vulkan_interop {
namespace {

Module Log("aurora::vulkan_interop");

int64_t to_vk_format(wgpu::TextureFormat format) noexcept {
  switch (format) {
  case wgpu::TextureFormat::RGBA8Unorm:
    return VK_FORMAT_R8G8B8A8_UNORM;
  case wgpu::TextureFormat::RGBA8UnormSrgb:
    return VK_FORMAT_R8G8B8A8_SRGB;
  case wgpu::TextureFormat::BGRA8Unorm:
    return VK_FORMAT_B8G8R8A8_UNORM;
  case wgpu::TextureFormat::BGRA8UnormSrgb:
    return VK_FORMAT_B8G8R8A8_SRGB;
  case wgpu::TextureFormat::RGBA16Float:
    return VK_FORMAT_R16G16B16A16_SFLOAT;
  default:
    return VK_FORMAT_UNDEFINED;
  }
}

// vkCmdCopyImage and WebGPU CopyTextureToTexture both require the two formats
// to be the same modulo sRGB encoding, so the bridge only accepts a target
// whose VkFormat sits in the same family as Aurora's eye output.
int copy_family(int64_t format) noexcept {
  switch (format) {
  case VK_FORMAT_R8G8B8A8_UNORM:
  case VK_FORMAT_R8G8B8A8_SRGB:
    return 1;
  case VK_FORMAT_B8G8R8A8_UNORM:
  case VK_FORMAT_B8G8R8A8_SRGB:
    return 2;
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    return 3;
  default:
    return 0;
  }
}

bool same_copy_family(int64_t left, int64_t right) noexcept {
  const int family = copy_family(left);
  return family != 0 && family == copy_family(right);
}

void close_fd(int& fd) noexcept {
  if (fd >= 0) {
    ::close(fd);
  }
  fd = -1;
}

// A sync file descriptor becomes readable once its fence signals, so a plain
// poll() is a CPU-side wait that needs no libsync.
void wait_sync_fd(int fd) noexcept {
  if (fd < 0) {
    return;
  }
  pollfd request{.fd = fd, .events = POLLIN, .revents = 0};
  while (::poll(&request, 1, 1000) == 0) {
    Log.warn("Waiting on a Dawn release fence took over a second");
  }
}

bool device_supports_bridge() noexcept {
  return webgpu::g_device && webgpu::g_backendType == wgpu::BackendType::Vulkan &&
         webgpu::g_device.HasFeature(wgpu::FeatureName::SharedTextureMemoryAHardwareBuffer) &&
         webgpu::g_device.HasFeature(wgpu::FeatureName::SharedFenceSyncFD);
}

// One AHardwareBuffer imported into Dawn. The import is created on first use
// and kept until the bridge is disabled; the ring the OpenXR side recycles is
// tiny (two buffers per eye), so this never grows beyond a handful of entries.
struct Import {
  AHardwareBuffer* buffer = nullptr;
  wgpu::SharedTextureMemory memory;
  wgpu::Texture texture;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  uint32_t width = 0;
  uint32_t height = 0;
  bool initialized = false;
  bool accessBegun = false;
};

struct PendingTarget {
  AHardwareBuffer* buffer = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  int64_t vkFormat = VK_FORMAT_UNDEFINED;
  int acquireFenceFd = -1;
  int32_t acquireImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

class StereoBridge final {
public:
  StereoBridge(AuroraVulkanStereoSubmittedCallback callback, void* userdata) noexcept
      : m_callback(callback), m_userdata(userdata) {}

  ~StereoBridge() { ReleaseImportsLocked(); }

  bool Initialize() noexcept {
    if (!device_supports_bridge()) {
      Log.error("Dawn Vulkan device lacks SharedTextureMemoryAHardwareBuffer or SharedFenceSyncFD");
      return false;
    }
    m_auroraFormat = webgpu::g_graphicsConfig.surfaceConfiguration.format;
    return to_vk_format(m_auroraFormat) != VK_FORMAT_UNDEFINED;
  }

  bool PrepareForDestruction() noexcept {
    std::lock_guard lock(m_mutex);
    // Dawn's queue is drained by aurora_quiesce_frame_worker() before this is
    // reached; all that can be outstanding here is a begun access whose
    // EndAccess never ran because the frame was abandoned.
    for (auto& [buffer, import] : m_imports) {
      if (import.accessBegun) {
        wgpu::SharedTextureMemoryEndAccessState end{};
        import.memory.EndAccess(import.texture, &end);
        import.accessBegun = false;
      }
    }
    ReleaseImportsLocked();
    return true;
  }

  bool SetTargets(uint64_t token, const AuroraVulkanStereoTarget* targets,
                  uint32_t targetCount) noexcept {
    if (token == 0 || targets == nullptr || targetCount == 0 ||
        targetCount > AURORA_VULKAN_STEREO_MAX_TARGETS) {
      return false;
    }
    std::lock_guard lock(m_mutex);
    if (m_framePending || m_encoded) {
      return false;
    }
    const int64_t auroraFormat = to_vk_format(m_auroraFormat);
    for (uint32_t eye = 0; eye < targetCount; ++eye) {
      const auto& target = targets[eye];
      if (target.buffer == nullptr || target.width == 0 || target.height == 0 ||
          !same_copy_family(target.vkFormat, auroraFormat)) {
        return false;
      }
    }
    for (uint32_t eye = 0; eye < targetCount; ++eye) {
      m_targets[eye] = {
          .buffer = targets[eye].buffer,
          .width = targets[eye].width,
          .height = targets[eye].height,
          .vkFormat = targets[eye].vkFormat,
          .acquireFenceFd = targets[eye].acquireFenceFd,
          .acquireImageLayout = targets[eye].acquireImageLayout,
      };
    }
    for (uint32_t eye = targetCount; eye < m_targets.size(); ++eye) {
      m_targets[eye] = {};
    }
    m_frameToken = token;
    m_targetCount = targetCount;
    m_framePending = true;
    return true;
  }

  bool Encode(wgpu::CommandEncoder& encoder, const stereo::SinkFrame& frame) noexcept {
    std::lock_guard lock(m_mutex);
    if (!m_framePending || m_encoded || frame.frameToken != m_frameToken) {
      return false;
    }
    if (EncodeLocked(encoder, frame)) {
      m_encoded = true;
      return true;
    }
    PublishAndClearFrameLocked(frame.frameToken, false);
    return false;
  }

  void Submitted(const stereo::SinkFrame& frame) noexcept {
    std::lock_guard lock(m_mutex);
    if (!m_framePending || !m_encoded || frame.frameToken != m_frameToken) {
      return;
    }
    std::array<AuroraVulkanStereoRelease, AURORA_VULKAN_STEREO_MAX_TARGETS> releases{};
    const bool success = EndAccessLocked(releases);
    NotifyLocked(frame.frameToken, success, releases);
    ClearFrameLocked();
  }

  void CancelPending() noexcept {
    std::lock_guard lock(m_mutex);
    if (!m_framePending) {
      return;
    }
    const uint64_t token = m_frameToken;
    std::array<AuroraVulkanStereoRelease, AURORA_VULKAN_STEREO_MAX_TARGETS> releases{};
    if (m_encoded) {
      EndAccessLocked(releases);
      for (auto& release : releases) {
        close_fd(release.releaseFenceFd);
      }
    }
    PublishAndClearFrameLocked(token, false);
  }

  bool CancelBeforeEncode(uint64_t token) noexcept {
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      return false;
    }
    if (token == 0 || !m_framePending || m_encoded || token != m_frameToken) {
      return false;
    }
    ClearFrameLocked();
    return true;
  }

private:
  Import* EnsureImport(uint32_t eye, const stereo::EyeImage& source) noexcept {
    const auto& target = m_targets[eye];
    if (source.texture == nullptr || source.format != m_auroraFormat ||
        source.size.width != target.width || source.size.height != target.height) {
      Log.error("Stereo eye {} does not match its OpenXR Vulkan target ({}x{} vs {}x{})", eye,
                source.size.width, source.size.height, target.width, target.height);
      return nullptr;
    }
    if (auto found = m_imports.find(target.buffer); found != m_imports.end()) {
      auto& import = found->second;
      if (import.width == target.width && import.height == target.height &&
          import.format == source.format) {
        return &import;
      }
      Log.error("AHardwareBuffer for eye {} was re-used with a different geometry", eye);
      return nullptr;
    }

    Import import;
    import.buffer = target.buffer;
    AHardwareBuffer_acquire(import.buffer);

    wgpu::SharedTextureMemoryAHardwareBufferDescriptor ahb{};
    ahb.handle = target.buffer;
    const wgpu::SharedTextureMemoryDescriptor memoryDescriptor{
        .nextInChain = &ahb,
        .label = eye == 0 ? "OpenXR left eye AHardwareBuffer" : "OpenXR right eye AHardwareBuffer",
    };
    import.memory = webgpu::g_device.ImportSharedTextureMemory(&memoryDescriptor);
    if (!import.memory) {
      Log.error("Dawn rejected the AHardwareBuffer import for eye {}", eye);
      AHardwareBuffer_release(import.buffer);
      return nullptr;
    }
    wgpu::SharedTextureMemoryProperties properties{};
    if (import.memory.GetProperties(&properties) != wgpu::Status::Success ||
        properties.size.width != target.width || properties.size.height != target.height ||
        (properties.usage & wgpu::TextureUsage::CopyDst) == wgpu::TextureUsage::None) {
      Log.error("Dawn reported incompatible AHardwareBuffer properties for eye {}", eye);
      AHardwareBuffer_release(import.buffer);
      return nullptr;
    }
    if (properties.format != source.format) {
      // The OpenXR side allocates R8G8B8A8_UNORM buffers because that is the only
      // 8-bit RGBA AHardwareBuffer format; gpu.cpp steers Aurora to RGBA8Unorm
      // under xrInterop so this mismatch only happens on a driver with no such
      // surface format, where a copy could never be legal anyway.
      Log.error("AHardwareBuffer format {} does not match Aurora's {} for eye {}",
                magic_enum::enum_name(properties.format), magic_enum::enum_name(source.format), eye);
      AHardwareBuffer_release(import.buffer);
      return nullptr;
    }
    const wgpu::TextureDescriptor textureDescriptor{
        .label = eye == 0 ? "OpenXR left eye shared texture" : "OpenXR right eye shared texture",
        .usage = wgpu::TextureUsage::CopyDst,
        .dimension = wgpu::TextureDimension::e2D,
        .size = {target.width, target.height, 1},
        .format = source.format,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    import.texture = import.memory.CreateTexture(&textureDescriptor);
    if (!import.texture) {
      Log.error("Dawn could not wrap the AHardwareBuffer for eye {}", eye);
      AHardwareBuffer_release(import.buffer);
      return nullptr;
    }
    import.format = source.format;
    import.width = target.width;
    import.height = target.height;
    const auto [inserted, ok] = m_imports.emplace(target.buffer, std::move(import));
    return ok ? &inserted->second : nullptr;
  }

  bool EncodeLocked(wgpu::CommandEncoder& encoder, const stereo::SinkFrame& frame) noexcept {
    std::array<Import*, AURORA_VULKAN_STEREO_MAX_TARGETS> imports{};
    for (uint32_t eye = 0; eye < m_targetCount; ++eye) {
      imports[eye] = EnsureImport(eye, frame.eyes[eye]);
      if (imports[eye] == nullptr) {
        return false;
      }
    }
    for (uint32_t eye = 0; eye < m_targetCount; ++eye) {
      auto& target = m_targets[eye];
      auto& import = *imports[eye];
      if (import.accessBegun) {
        Log.error("AHardwareBuffer for eye {} is still under a previous access", eye);
        RollbackAccesses(imports, eye);
        return false;
      }
      // The OpenXR side's release barrier leaves the image in acquireImageLayout;
      // Dawn's acquire barrier must repeat exactly that old/new pair, then it
      // transitions to its own copy-destination layout. A never-written buffer
      // has undefined contents and layout, which Dawn treats as uninitialized.
      const bool haveContents = import.initialized &&
                                target.acquireImageLayout != VK_IMAGE_LAYOUT_UNDEFINED;
      wgpu::SharedTextureMemoryVkImageLayoutBeginState layout{};
      layout.oldLayout = haveContents ? target.acquireImageLayout : VK_IMAGE_LAYOUT_UNDEFINED;
      layout.newLayout = haveContents ? target.acquireImageLayout : VK_IMAGE_LAYOUT_GENERAL;
      wgpu::SharedFence acquireFence;
      if (target.acquireFenceFd >= 0) {
        wgpu::SharedFenceSyncFDDescriptor syncFd{};
        syncFd.handle = target.acquireFenceFd;
        const wgpu::SharedFenceDescriptor fenceDescriptor{
            .nextInChain = &syncFd,
            .label = "OpenXR eye copy-out fence",
        };
        // Dawn duplicates the descriptor; the bridge still owns and closes its copy.
        acquireFence = webgpu::g_device.ImportSharedFence(&fenceDescriptor);
        close_fd(target.acquireFenceFd);
        if (!acquireFence) {
          Log.error("Dawn could not import the OpenXR copy-out fence for eye {}", eye);
          RollbackAccesses(imports, eye);
          return false;
        }
      }
      const std::array fences{acquireFence};
      // A sync file descriptor is binary; Dawn requires signaled value 1 for
      // SyncFD fences ("signaled value (0) was not 1" otherwise).
      const std::array<uint64_t, 1> values{1};
      wgpu::SharedTextureMemoryBeginAccessDescriptor begin{};
      begin.nextInChain = &layout;
      begin.concurrentRead = false;
      begin.initialized = haveContents;
      if (acquireFence) {
        begin.fenceCount = 1;
        begin.fences = fences.data();
        begin.signaledValueCount = 1;
        begin.signaledValues = values.data();
      }
      if (import.memory.BeginAccess(import.texture, &begin) != wgpu::Status::Success) {
        Log.error("Dawn BeginAccess failed for stereo eye {}", eye);
        RollbackAccesses(imports, eye);
        return false;
      }
      import.accessBegun = true;
    }
    for (uint32_t eye = 0; eye < m_targetCount; ++eye) {
      const auto& import = *imports[eye];
      const wgpu::TexelCopyTextureInfo source{
          .texture = *frame.eyes[eye].texture,
          .mipLevel = 0,
          .origin = {},
          .aspect = wgpu::TextureAspect::All,
      };
      const wgpu::TexelCopyTextureInfo destination{
          .texture = import.texture,
          .mipLevel = 0,
          .origin = {},
          .aspect = wgpu::TextureAspect::All,
      };
      const wgpu::Extent3D extent{import.width, import.height, 1};
      encoder.CopyTextureToTexture(&source, &destination, &extent);
      m_encodedImports[eye] = imports[eye];
    }
    return true;
  }

  void RollbackAccesses(const std::array<Import*, AURORA_VULKAN_STEREO_MAX_TARGETS>& imports,
                        uint32_t count) noexcept {
    for (uint32_t eye = 0; eye < count; ++eye) {
      if (imports[eye] != nullptr && imports[eye]->accessBegun) {
        wgpu::SharedTextureMemoryEndAccessState end{};
        imports[eye]->memory.EndAccess(imports[eye]->texture, &end);
        imports[eye]->initialized = end.initialized;
        imports[eye]->accessBegun = false;
      }
    }
    for (auto& target : m_targets) {
      close_fd(target.acquireFenceFd);
    }
  }

  bool EndAccessLocked(
      std::array<AuroraVulkanStereoRelease, AURORA_VULKAN_STEREO_MAX_TARGETS>& releases) noexcept {
    bool success = true;
    for (auto& release : releases) {
      release = {.releaseFenceFd = -1, .releasedImageLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    }
    for (uint32_t eye = 0; eye < m_targetCount; ++eye) {
      Import* import = m_encodedImports[eye];
      if (import == nullptr || !import->accessBegun) {
        success = false;
        continue;
      }
      wgpu::SharedTextureMemoryVkImageLayoutEndState layout{};
      wgpu::SharedTextureMemoryEndAccessState end{};
      end.nextInChain = &layout;
      if (import->memory.EndAccess(import->texture, &end) != wgpu::Status::Success) {
        Log.error("Dawn EndAccess failed for stereo eye {}", eye);
        success = false;
      } else {
        import->initialized = end.initialized;
        releases[eye].releasedImageLayout = layout.newLayout;
        for (size_t i = 0; i < end.fenceCount; ++i) {
          wgpu::SharedFenceSyncFDExportInfo syncFd{};
          wgpu::SharedFenceExportInfo info{};
          info.nextInChain = &syncFd;
          end.fences[i].ExportInfo(&info);
          if (info.type == wgpu::SharedFenceType::SyncFD && syncFd.handle >= 0) {
            // The fence keeps its descriptor; hand the caller an independent one.
            const int duplicate = ::dup(syncFd.handle);
            if (duplicate >= 0) {
              if (releases[eye].releaseFenceFd >= 0) {
                // Dawn normally returns exactly one fence per access. Both must
                // be honoured and one descriptor cannot express two fences, so
                // the earlier one is retired on the CPU before handing over the
                // latest.
                Log.warn("Dawn returned several release fences for eye {}; merging on the CPU", eye);
                wait_sync_fd(releases[eye].releaseFenceFd);
                close_fd(releases[eye].releaseFenceFd);
              }
              releases[eye].releaseFenceFd = duplicate;
            }
          } else {
            Log.error("Dawn returned a non-sync-fd fence for eye {}", eye);
            success = false;
          }
        }
      }
      import->accessBegun = false;
    }
    return success;
  }

  void ReleaseImportsLocked() noexcept {
    for (auto& [buffer, import] : m_imports) {
      import.texture = nullptr;
      import.memory = nullptr;
      if (import.buffer != nullptr) {
        AHardwareBuffer_release(import.buffer);
      }
    }
    m_imports.clear();
  }

  void ClearFrameLocked() noexcept {
    for (auto& target : m_targets) {
      close_fd(target.acquireFenceFd);
      target = {};
    }
    m_encodedImports = {};
    m_frameToken = 0;
    m_targetCount = 0;
    m_framePending = false;
    m_encoded = false;
  }

  void PublishAndClearFrameLocked(uint64_t token, bool success) noexcept {
    std::array<AuroraVulkanStereoRelease, AURORA_VULKAN_STEREO_MAX_TARGETS> releases{};
    for (auto& release : releases) {
      release = {.releaseFenceFd = -1, .releasedImageLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    }
    NotifyLocked(token, success, releases);
    ClearFrameLocked();
  }

  void NotifyLocked(uint64_t token, bool success,
                    const std::array<AuroraVulkanStereoRelease, AURORA_VULKAN_STEREO_MAX_TARGETS>&
                        releases) noexcept {
    if (m_callback != nullptr) {
      m_callback(token, success, releases.data(), m_targetCount, m_userdata);
    } else {
      for (auto release : releases) {
        close_fd(release.releaseFenceFd);
      }
    }
  }

  std::mutex m_mutex;
  std::unordered_map<AHardwareBuffer*, Import> m_imports;
  std::array<PendingTarget, AURORA_VULKAN_STEREO_MAX_TARGETS> m_targets{};
  std::array<Import*, AURORA_VULKAN_STEREO_MAX_TARGETS> m_encodedImports{};
  wgpu::TextureFormat m_auroraFormat = wgpu::TextureFormat::Undefined;
  AuroraVulkanStereoSubmittedCallback m_callback = nullptr;
  void* m_userdata = nullptr;
  uint64_t m_frameToken = 0;
  uint32_t m_targetCount = 0;
  bool m_framePending = false;
  bool m_encoded = false;
};

std::unique_ptr<StereoBridge> g_bridge;

bool sink_encode(wgpu::CommandEncoder& encoder, const stereo::SinkFrame& frame,
                 void* userdata) noexcept {
  return static_cast<StereoBridge*>(userdata)->Encode(encoder, frame);
}

void sink_submitted(const stereo::SinkFrame& frame, void* userdata) noexcept {
  static_cast<StereoBridge*>(userdata)->Submitted(frame);
}

} // namespace
} // namespace aurora::vulkan_interop

bool aurora_vulkan_get_native_handles(AuroraVulkanNativeHandles* handles) {
  if (handles == nullptr) {
    return false;
  }
  *handles = {};
  using namespace aurora::vulkan_interop;
  if (!aurora::webgpu::g_device || aurora::webgpu::g_backendType != wgpu::BackendType::Vulkan) {
    return false;
  }
  const int64_t format = to_vk_format(aurora::webgpu::g_graphicsConfig.surfaceConfiguration.format);
  if (format == VK_FORMAT_UNDEFINED) {
    return false;
  }
  *handles = {
      .colorVkFormat = format,
      .sharedTextureMemoryAHardwareBuffer =
          aurora::webgpu::g_device.HasFeature(wgpu::FeatureName::SharedTextureMemoryAHardwareBuffer),
      .sharedFenceSyncFd = aurora::webgpu::g_device.HasFeature(wgpu::FeatureName::SharedFenceSyncFD),
  };
  return true;
}

bool aurora_vulkan_enable_stereo_bridge(AuroraVulkanStereoSubmittedCallback submitted,
                                        void* userdata) {
  using namespace aurora::vulkan_interop;
  if (g_bridge || submitted == nullptr) {
    return false;
  }
  auto bridge = std::make_unique<StereoBridge>(submitted, userdata);
  if (!bridge->Initialize()) {
    return false;
  }
  aurora::stereo::set_sink(sink_encode, sink_submitted, bridge.get());
  g_bridge = std::move(bridge);
  return true;
}

bool aurora_vulkan_set_stereo_targets(uint64_t frameToken,
                                      const AuroraVulkanStereoTarget* targets,
                                      uint32_t targetCount) {
  using namespace aurora::vulkan_interop;
  return g_bridge && g_bridge->SetTargets(frameToken, targets, targetCount);
}

bool aurora_vulkan_cancel_stereo_targets(uint64_t frameToken) {
  using namespace aurora::vulkan_interop;
  return g_bridge && g_bridge->CancelBeforeEncode(frameToken);
}

bool aurora_vulkan_disable_stereo_bridge() {
  using namespace aurora::vulkan_interop;
  if (!g_bridge) {
    return true;
  }
  aurora::stereo::set_sink(nullptr, nullptr, nullptr);
  g_bridge->CancelPending();
  if (!g_bridge->PrepareForDestruction()) {
    (void)g_bridge.release();
    return false;
  }
  g_bridge.reset();
  return true;
}

#else

bool aurora_vulkan_get_native_handles(AuroraVulkanNativeHandles* handles) {
  if (handles != nullptr) {
    *handles = {};
  }
  return false;
}

bool aurora_vulkan_enable_stereo_bridge(AuroraVulkanStereoSubmittedCallback, void*) {
  return false;
}

bool aurora_vulkan_set_stereo_targets(uint64_t, const AuroraVulkanStereoTarget*, uint32_t) {
  return false;
}

bool aurora_vulkan_cancel_stereo_targets(uint64_t) { return false; }

bool aurora_vulkan_disable_stereo_bridge() { return true; }

#endif
