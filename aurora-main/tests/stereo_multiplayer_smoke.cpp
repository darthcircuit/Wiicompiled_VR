// Opt-in D3D12 readback test: P1 is red, P2 green, P3 blue, P4 yellow.
// Both eyes must be entirely P1 while the original EFB keeps every pane.
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include "stereo.hpp"
#include "gfx/common.hpp"

#include <array>
#include <cstdio>
#include <filesystem>

using namespace aurora::webgpu;
namespace {
// Eyes match the EFB width so a one-pixel-class divider on the virtual screen
// still covers eye pixels; a 160x120 eye never rasterized it.
constexpr uint32_t kWidth = 640, kHeight = 480, kEfbWidth = 640, kEfbHeight = 528, kPitch = 2560;
constexpr uint64_t kBytes = kPitch * kEfbHeight;
std::array<wgpu::Buffer, 3> readbacks;
std::array<wgpu::TextureFormat, 3> formats;
uint64_t token = 0;
uint32_t copies = 0;
bool Provide(uint32_t, AuroraStereoFrame* frame, void*) {
  *frame = {};
  frame->frameToken = ++token;
  frame->contentTag = 42;
  for (auto& eye : frame->eyes) {
    eye.width = kWidth;
    eye.height = kHeight;
    eye.projection[0] = eye.projection[5] = 1;
    eye.projection[10] = eye.projection[11] = eye.projection[14] = -1;
    eye.viewFromCenter[0] = eye.viewFromCenter[5] = eye.viewFromCenter[10] = 1;
  }
  return true;
}
bool Encode(wgpu::CommandEncoder& encoder, const aurora::stereo::SinkFrame& frame, void*) noexcept {
  for (uint32_t i = 0; i < 3; ++i) {
    const auto texture = i < 2 ? *frame.eyes[i].texture : present_source().texture;
    formats[i] = texture.GetFormat();
    const wgpu::TexelCopyTextureInfo source{.texture = texture};
    const wgpu::TexelCopyBufferInfo destination{.layout = {.bytesPerRow = kPitch, .rowsPerImage = kEfbHeight},
                                                .buffer = readbacks[i]};
    const wgpu::Extent3D size{i < 2 ? kWidth : kEfbWidth, i < 2 ? kHeight : kEfbHeight, 1};
    encoder.CopyTextureToBuffer(&source, &destination, &size);
  }
  ++copies;
  return true;
}
void Draw(uint32_t players) {
  // GX depth occupies [-w, 0], unlike OpenGL's [-w, +w].
  Mtx44 projection{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, -1}, {0, 0, -1, 0}};
  Mtx transform{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, -2}};
  GXSetProjection(projection, GX_PERSPECTIVE);
  GXSetCurrentMtx(GX_PNMTX0);
  GXLoadPosMtxImm(transform, GX_PNMTX0);
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GXSetNumTexGens(0);
  GXSetNumChans(1);
  GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
  GXSetNumTevStages(1);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
  GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
  GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
  GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
  GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
  GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
  GXSetCullMode(GX_CULL_NONE);
  GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
  GXSetColorUpdate(GX_TRUE);
  GXSetAlphaUpdate(GX_TRUE);
  const GXColor colors[]{{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}};
  for (uint32_t p = 0; p < players; ++p) {
    const uint32_t width = players >= 3 ? kEfbWidth / 2 : kEfbWidth;
    const uint32_t height = players >= 2 ? kEfbHeight / 2 : kEfbHeight;
    const uint32_t x = players >= 3 ? (p % 2) * width : 0;
    const uint32_t y = players >= 3 ? (p / 2) * height : p * height;
    GXSetViewport(float(x), float(y), float(width), float(height), 0, 1);
    GXSetScissor(x, y, width, height);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(-2, -2, 0);
    GXColor4u8(colors[p].r, colors[p].g, colors[p].b, 255);
    GXPosition3f32(2, -2, 0);
    GXColor4u8(colors[p].r, colors[p].g, colors[p].b, 255);
    GXPosition3f32(2, 2, 0);
    GXColor4u8(colors[p].r, colors[p].g, colors[p].b, 255);
    GXPosition3f32(-2, 2, 0);
    GXColor4u8(colors[p].r, colors[p].g, colors[p].b, 255);
    GXEnd();
  }
  if (players == 1)
    return;
  // MKW's separators and per-pane backing quads can share one full-screen
  // orthographic viewport. Viewport filtering alone must not admit them in VR.
  Mtx44 ortho{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, -0.5f}, {0, 0, 0, 1}};
  Mtx identity{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
  GXSetProjection(ortho, GX_ORTHOGRAPHIC);
  GXLoadPosMtxImm(identity, GX_PNMTX0);
  GXSetViewport(0, 0, kEfbWidth, kEfbHeight, 0, 1);
  GXSetScissor(0, 0, kEfbWidth, kEfbHeight);
  const auto rect = [](float left, float bottom, float right, float top) {
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    for (const auto& p :
         std::array<std::array<float, 2>, 4>{{{left, bottom}, {right, bottom}, {right, top}, {left, top}}}) {
      GXPosition3f32(p[0], p[1], 0);
      GXColor4u8(0, 0, 0, 255);
    }
    GXEnd();
  };
  if (players >= 3)
    rect(0, 0, 1, 1); // The reported black quadrant.
  else
    rect(-1, -1, 1, 0);
  if (players >= 3) {
    GXSetLineWidth(6, GX_TO_ZERO);
    GXBegin(GX_LINES, GX_VTXFMT0, 2);
    GXPosition3f32(0, -1, 0);
    GXColor4u8(0, 0, 0, 255);
    GXPosition3f32(0, 1, 0);
    GXColor4u8(0, 0, 0, 255);
    GXEnd();
  }
  // MKW's partition_line layout draws the divider as one-pixel picture panes
  // that sample a pattern texture, so the divider here is a textured quad too:
  // texture use alone must not keep it out of the furniture filter.
  alignas(32) static const uint8_t blackTexels[8 * 8 * 2]{}; // RGB565 zero: opaque black.
  GXTexObj divider{};
  GXInitTexObj(&divider, blackTexels, 8, 8, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GXLoadTexObj(&divider, GX_TEXMAP0);
  GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
  GXSetNumTexGens(1);
  GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
  GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
  GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  for (const auto& p :
       std::array<std::array<float, 2>, 4>{{{-1, -0.006f}, {1, -0.006f}, {1, 0.006f}, {-1, 0.006f}}}) {
    GXPosition3f32(p[0], p[1], 0);
    GXColor4u8(255, 255, 255, 255);
    GXTexCoord2f32(p[0] * 0.5f + 0.5f, p[1] > 0 ? 1.f : 0.f);
  }
  GXEnd();
}
bool Check(uint32_t players) {
  bool okay = copies != 0;
  for (uint32_t i = 0; i < 3; ++i) {
    wgpu::MapAsyncStatus status{};
    const auto future = readbacks[i].MapAsync(wgpu::MapMode::Read, 0, kBytes, wgpu::CallbackMode::WaitAnyOnly,
                                              [&](wgpu::MapAsyncStatus result, wgpu::StringView) { status = result; });
    if (g_instance.WaitAny(future, 5'000'000'000) != wgpu::WaitStatus::Success ||
        status != wgpu::MapAsyncStatus::Success) {
      return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(readbacks[i].GetConstMappedRange());
    for (uint32_t quadrant = 0; quadrant < 4; ++quadrant) {
      const uint32_t width = i < 2 ? kWidth : kEfbWidth;
      const uint32_t height = i < 2 ? kHeight : kEfbHeight;
      const uint32_t x = (quadrant % 2) * width / 2 + width / 4;
      const uint32_t y = (quadrant / 2) * height / 2 + height / 4;
      uint32_t player = i < 2 || players == 1 ? 0 : players == 2 ? quadrant / 2 : quadrant;
      if (player >= players)
        continue;
      const auto* pixel = bytes + y * kPitch + x * 4;
      const bool bgra = formats[i] == wgpu::TextureFormat::BGRA8Unorm;
      const uint8_t r = pixel[bgra ? 2 : 0], g = pixel[1], b = pixel[bgra ? 0 : 2];
      const bool mask = i == 2 && players > 1 && (players == 2 ? quadrant >= 2 : quadrant == 1);
      const bool match = r == (!mask && (player == 0 || player == 3) ? 255 : 0) &&
                         g == (!mask && (player == 1 || player == 3) ? 255 : 0) &&
                         b == (!mask && player == 2 ? 255 : 0);
      if (!match)
        std::fprintf(stderr, "%uP target %u quadrant %u expected player %u, got %u,%u,%u\n", players, i, quadrant,
                     player + 1, r, g, b);
      okay &= match;
    }
    if (i < 2) {
      // Include the divider locations; quadrant-center samples alone miss them.
      for (uint32_t y = 4; y < kHeight - 4; ++y) {
        for (uint32_t x = 4; x < kWidth - 4; ++x) {
          const auto* pixel = bytes + y * kPitch + x * 4;
          const bool bgra = formats[i] == wgpu::TextureFormat::BGRA8Unorm;
          if (pixel[bgra ? 2 : 0] != 255 || pixel[1] != 0 || pixel[bgra ? 0 : 2] != 0) {
            std::fprintf(stderr, "%uP eye %u has split-screen overlay at %u,%u\n", players, i, x, y);
            okay = false;
            y = kHeight;
            break;
          }
        }
      }
    } else if (players > 1) {
      const auto* divider = bytes + (kEfbHeight / 2) * kPitch + (kEfbWidth / 4) * 4;
      if (divider[0] != 0 || divider[1] != 0 || divider[2] != 0) {
        std::fprintf(stderr, "Desktop divider was removed\n");
        okay = false;
      }
    }
    readbacks[i].Unmap();
  }
  return okay;
}
} // namespace

int main(int argc, char** argv) {
  std::filesystem::create_directories("stereo-multiplayer-cache");
  AuroraConfig config{};
  config.appName = "Aurora multiplayer VR validation";
  config.userPath = ".";
  config.cachePath = "stereo-multiplayer-cache";
  config.desiredBackend = BACKEND_D3D12;
  config.windowWidth = kWidth;
  config.windowHeight = kHeight;
  config.hasWindowPosition = true;
  config.windowPosX = config.windowPosY = -30000;
  config.xrInterop = true;
  config.logLevel = LOG_WARNING;
  config.logCallback = [](AuroraLogLevel, const char* module, const char* message, unsigned length) {
    std::fprintf(stderr, "%s: %.*s\n", module, int(length), message);
  };
  aurora_initialize(argc, argv, &config);
  aurora_set_skip_unready_pipelines(false);
  aurora_begin_frame();
  GXInit(nullptr, 0);
  const wgpu::BufferDescriptor descriptor{.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                          .size = kBytes};
  for (auto& buffer : readbacks)
    buffer = g_device.CreateBuffer(&descriptor);
  aurora_set_stereo_frame_provider(Provide, nullptr);
  aurora::gfx::set_stereo_hud_screen(true, 2.f, 1.f);
  aurora::stereo::set_sink(Encode, nullptr);
  bool okay = true;
  for (bool interpolate : {false, true}) {
    aurora_set_stereo_frame_interpolation(interpolate);
    for (uint32_t players : {1u, 2u, 3u, 4u, 1u}) {
      for (uint32_t frame = 0; frame < 3; ++frame) {
        aurora_update();
        if (!aurora_begin_frame())
          return 2;
        Draw(players);
        // Deliberately omit the setter for 1P to exercise per-frame reset.
        if (players > 1)
          aurora_set_stereo_local_player_count(players);
        aurora_end_frame_tagged(42);
        aurora_begin_frame();
        aurora_wait_for_frame_worker();
      }
      const bool passed = Check(players);
      okay &= passed;
      std::printf("%u players, interpolation %s: %s\n", players, interpolate ? "on" : "off", passed ? "PASS" : "FAIL");
    }
  }
  aurora_set_stereo_frame_interpolation(false);
  aurora_quiesce_frame_worker();
  aurora_set_stereo_frame_provider(nullptr, nullptr);
  aurora::stereo::set_sink(nullptr, nullptr);
  for (auto& buffer : readbacks)
    buffer = nullptr;
  aurora_shutdown();
  return okay ? 0 : 1;
}
