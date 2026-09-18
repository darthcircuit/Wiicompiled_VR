// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MKW_ENABLE_OPENXR)

#include "vr/openxr_runtime.h"

#include <array>
#include <cstdint>

namespace mkw::vr {

// Backend-neutral frame vocabulary shared by every graphics binding.
//
// The OpenXR pacing thread in openxr_integration.cpp is written against these
// types and the method surface documented on OpenXRD3D12Backend; each concrete
// backend (D3D12 on Windows, Vulkan on Android) implements that same surface so
// the pacing, retained-layer and policy logic is compiled once for both.

enum class OpenXRFrameMode {
    ImmersiveProjection,
    VirtualScreen,
};

enum class OpenXRBeginStatus {
    Ready,
    SessionNotRunning,
    ExitRequested,
    Error,
};

enum class OpenXRSubmissionStatus {
    Success,
    // GPU work may have touched the compositor image or the shared buffers with no completion
    // marker to wait on; the session cannot continue.
    Failed,
    // The eye copy was not submitted and nothing touched the compositor image or the shared
    // buffers, so the frame may end without a layer and the next one is tried normally.
    Skipped,
    Timeout,
    ShuttingDown,
};

struct OpenXRPresentation {
    OpenXRFrameMode mode = OpenXRFrameMode::ImmersiveProjection;

    // Used only by VirtualScreen.
    float quad_distance_meters = 2.0f;
    float quad_width_meters = 2.4f;

    // When quad_anchored is set, the quad is placed at quad_pose in the
    // application reference space and stays put as the player looks around.
    // Otherwise it falls back to being head-locked in XR_VIEW_SPACE, centered
    // straight ahead at -Z, which is what happens until tracking has produced a
    // head pose good enough to anchor against.
    bool quad_anchored = false;
    XrPosef quad_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
};

struct OpenXRBackendFrame {
    OpenXRFrame xr_frame;
    OpenXRPresentation presentation;
    std::array<uint32_t, kOpenXREyeCount> render_width{};
    std::array<uint32_t, kOpenXREyeCount> render_height{};
    bool expects_gpu_submission = false;
};

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR)
