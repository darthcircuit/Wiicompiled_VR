#pragma once

#include <aurora/event.h>

union SDL_Event;
struct ImDrawData;

namespace wgpu {
class RenderPassEncoder;
} // namespace wgpu

namespace aurora::imgui {
void create_context() noexcept;
void initialize() noexcept;
void shutdown() noexcept;

void process_event(const SDL_Event& event) noexcept;
bool wants_capture_event(const SDL_Event& event) noexcept;
void new_frame(const AuroraWindowSize& size) noexcept;
// Build this frame's ImGui draw data. Idempotent for the rest of the frame: the lists are built
// once and every presentation slot replays them. Reset by new_frame.
void render_frame_data() noexcept;
void render(const wgpu::RenderPassEncoder& pass) noexcept;

// The headset panel as aurora_imgui_set_stereo_overlay last set it.
struct StereoOverlay {
  ImDrawData* drawData = nullptr;
  float widthFraction = 0.f;
};
StereoOverlay latch_stereo_overlay() noexcept;
// Renders another context's draw data with this context's backend. The backend keeps one projection
// uniform for every pass, so a pass whose display size differs from the desktop's must be submitted
// before the next pass is recorded.
bool render_draw_data(const wgpu::RenderPassEncoder& pass, ImDrawData* data) noexcept;
} // namespace aurora::imgui
