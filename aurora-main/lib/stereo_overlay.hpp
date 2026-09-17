#pragma once

#include <aurora/math.hpp>
#include <webgpu/webgpu_cpp.h>

#include <cstdint>

struct ImDrawData;

// The headset-only panel of aurora_imgui_set_stereo_overlay. Everything here
// belongs to the frame worker, like the eye targets it draws into.
namespace aurora::stereo_overlay {

// Renders the panel's draw data into the panel texture and returns the command
// buffer holding that pass, which the caller submits before recording any other
// ImGui pass: the ImGui backend's projection uniform is shared by every pass, and
// the panel's canvas is not the desktop's size. Null draw data hides the panel,
// and so does any failure; both return a null command buffer.
wgpu::CommandBuffer prepare(ImDrawData* drawData, float widthFraction) noexcept;

// Draws the most recently prepared panel over a finished immersive eye, on the
// 2D layer's virtual screen seen through that eye's frustum and view.
void composite_immersive(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& eye,
                         const Mat4x4<float>& eyeFrustum, const Mat3x4<float>& viewFromCenter,
                         uint32_t eyeIndex) noexcept;

// Draws it over a virtual-screen eye image, which is shown flat as a quad.
void composite_flat(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& eye, const wgpu::Extent3D& size,
                    uint32_t eyeIndex) noexcept;

void shutdown() noexcept;

} // namespace aurora::stereo_overlay
