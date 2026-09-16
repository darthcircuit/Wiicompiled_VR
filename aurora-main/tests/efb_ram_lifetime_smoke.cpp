// Opt-in GPU regression for a depth-probe allocation reused during a scene
// restart. A late map callback must not overwrite the new camera allocation.
#include <aurora/aurora.h>
#include <dolphin/gx.h>
#include "gfx/efb_ram_copy.hpp"
#include "gfx/efb_ram_encoder.hpp"
#include "webgpu/gpu.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

using namespace aurora;
namespace {
constexpr uint8_t kCameraByte = 0x35;
std::array<uint8_t, 256> guest;

gfx::TextureHandle MakeTexture(uint32_t width, uint32_t height, std::array<uint8_t, 4> pixel) {
  auto texture = gfx::new_render_texture(width, height, GX_TF_RGBA8, "Probe lifetime test");
  if (texture->format == wgpu::TextureFormat::BGRA8Unorm)
    std::swap(pixel[0], pixel[2]);
  std::array<uint8_t, 256> pixels;
  for (size_t i = 0; i < width * height; ++i)
    std::copy(pixel.begin(), pixel.end(), pixels.begin() + i * 4);
  const wgpu::TexelCopyTextureInfo destination{.texture = texture->texture};
  const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = width * 4, .rowsPerImage = height};
  webgpu::g_queue.WriteTexture(&destination, pixels.data(), width * height * 4, &layout, &texture->size);
  return texture;
}

void SubmitProbe(const gfx::TextureHandle& texture, GXTexFmt format, uint32_t width, uint32_t height) {
  gfx::efb_ram::schedule(guest.data(), width, height, format, texture);
  gfx::efb_ram::seal_async_downloads();
  auto encoder = webgpu::g_device.CreateCommandEncoder();
  gfx::efb_ram::encode_async_downloads(encoder);
  auto commands = encoder.Finish();
  webgpu::g_queue.Submit(1, &commands);
  // The guest frees the probe and constructs a camera at the same address,
  // before the readback callback is registered.
  guest.fill(kCameraByte);
  gfx::efb_ram::after_submit();
}

bool CameraIntact() {
  return std::all_of(guest.begin(), guest.end(), [](uint8_t byte) { return byte == kCameraByte; });
}

bool AwaitProbe(const gfx::TextureHandle& texture, GXTexFmt format, uint32_t width, uint32_t height,
                const std::array<uint8_t, 4>& pixel) {
  const size_t size = gfx::efb_ram::encoded_size(format, width, height);
  std::array<uint8_t, 256> pixels{}, expected{};
  for (size_t i = 0; i < width * height; ++i)
    std::copy(pixel.begin(), pixel.end(), pixels.begin() + i * 4);
  if (!gfx::efb_ram::encode(expected.data(), size, format, width, height, pixels.data(), width, height, width * 4,
                            gfx::efb_ram::HostPixelOrder::RGBA))
    return false;

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    // Give the real GPU completion callback a chance to run while the old
    // destination belongs to the camera, then verify it remains untouched.
    webgpu::g_instance.ProcessEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!CameraIntact()) {
      std::fprintf(stderr, "Late GPU readback overwrote the reused camera allocation\n");
      return false;
    }
    // This explicit copy owns the destination again and may receive the result.
    gfx::efb_ram::schedule(guest.data(), width, height, format, texture);
    const bool arrived = std::equal(expected.begin(), expected.begin() + size, guest.begin());
    const bool canary =
        std::all_of(guest.begin() + size, guest.end(), [](uint8_t byte) { return byte == kCameraByte; });
    gfx::efb_ram::cancel();
    guest.fill(kCameraByte);
    if (!canary)
      return false;
    if (arrived)
      return true;
  } while (std::chrono::steady_clock::now() < deadline);
  std::fprintf(stderr, "Completed probe was not delivered by the next compatible copy\n");
  return false;
}

bool Run() {
  const std::array<uint8_t, 4> pixel{0xff, 0xfa, 0xb6, 0xff};
  auto large = MakeTexture(8, 8, pixel);
  auto small = MakeTexture(4, 4, pixel);
  SubmitProbe(large, GX_TF_Z24X8, 8, 8);
  if (!AwaitProbe(large, GX_TF_Z24X8, 8, 8, pixel))
    return false;
  std::puts("Reused camera allocation survives late 256-byte probe: PASS");

  // Same address, smaller allocation and different encoding: the retained
  // 256-byte result must not escape the new 32-byte destination.
  gfx::efb_ram::schedule(guest.data(), 4, 4, GX_TF_Z16, small);
  const bool fallback = std::all_of(guest.begin(), guest.begin() + 32, [](uint8_t b) { return b == 0xff; });
  const bool canary = std::all_of(guest.begin() + 32, guest.end(), [](uint8_t b) { return b == kCameraByte; });
  gfx::efb_ram::cancel();
  if (!fallback || !canary)
    return false;
  std::puts("Reused address rejects incompatible retained format and size: PASS");

  SubmitProbe(small, GX_TF_Z24X8, 4, 4);
  if (!AwaitProbe(small, GX_TF_Z24X8, 4, 4, pixel))
    return false;
  std::puts("Crash-dump 64-byte depth pattern delivered only during a new copy: PASS");
  SubmitProbe(small, GX_TF_Z16, 4, 4);
  if (!AwaitProbe(small, GX_TF_Z16, 4, 4, pixel))
    return false;
  std::puts("Changed probe format delivers compatible pixels without touching canaries: PASS");
  return true;
}
} // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::filesystem::create_directories("efb-lifetime-cache");
  AuroraConfig config{};
  config.appName = "Aurora EFB lifetime validation";
  config.userPath = ".";
  config.cachePath = "efb-lifetime-cache";
  config.desiredBackend = BACKEND_D3D12;
  config.windowWidth = 160;
  config.windowHeight = 120;
  config.hasWindowPosition = true;
  config.windowPosX = config.windowPosY = -30000;
  config.xrInterop = true;
  config.logLevel = LOG_WARNING;
  config.logCallback = [](AuroraLogLevel, const char* module, const char* message, unsigned length) {
    std::fprintf(stderr, "%s: %.*s\n", module, int(length), message);
  };
  aurora_initialize(argc, argv, &config);
  std::puts("GPU initialized");
  aurora_begin_frame();
  GXInit(nullptr, 0);
  std::puts("Starting readback lifetime checks");
  const bool passed = Run();
  std::puts("Readback lifetime checks finished");
  aurora_end_frame();
  aurora_quiesce_frame_worker();
  aurora_shutdown();
  return passed ? 0 : 1;
}
