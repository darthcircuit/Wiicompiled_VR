// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "vr/openxr_integration.h"

#include "runtime_config.h"
#include "runtime_log.h"
#include "vr/mkw_vr_first_person.h"
#include "vr/mkw_vr_policy.h"
#include "vr/mkw_vr_instrumentation.h"
#include "vr/openxr_diagnostics.h"
#include <aurora/gfx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#if defined(MKW_ENABLE_OPENXR)
#include "vr/openxr_backend.h"
#include "vr/openxr_input.h"
#include "vr/openxr_runtime.h"
#if defined(_WIN32)
#include "vr/openxr_d3d12.h"
#define MKW_OPENXR_GRAPHICS_BACKEND 1
#elif defined(__ANDROID__)
#include "vr/openxr_android.h"
#include "vr/openxr_vulkan.h"
#include <time.h>
#define XR_USE_TIMESPEC
#include <openxr/openxr_platform.h>
#define MKW_OPENXR_GRAPHICS_BACKEND 1
#else
#define MKW_OPENXR_GRAPHICS_BACKEND 0
#endif
#else
#define MKW_OPENXR_GRAPHICS_BACKEND 0
#endif

namespace mkw::vr {
namespace {

inline constexpr float kDegreesToRadians = 0.01745329252f;

// A standalone headset has no desktop to fall back to, so VR is on unless the
// user's configuration turns it off. Desktop builds keep the opt-in default.
#if defined(__ANDROID__)
inline constexpr bool kVrEnabledDefault = true;
#else
inline constexpr bool kVrEnabledDefault = false;
#endif

void ConfigurePolicy(bool enabled) noexcept {
    MkwVRPolicyReset();
    MkwVRPolicyConfig config{};
    config.enabled = enabled;
    config.immersive_races = true;
    config.world_units_per_meter = RuntimeConfigFile::VrWorldUnitsPerMeter(500.0f);
    config.hud_distance_meters = RuntimeConfigFile::VrHudDistanceMeters(2.0f);
    config.hud_width_meters = RuntimeConfigFile::VrHudWidthMeters(2.4f);
    config.first_person_units_per_meter = RuntimeConfigFile::VrFirstPersonUnitsPerMeter();
    MkwVRPolicyConfigure(config);
    MkwVRInstrumentationInitialize();
    MkwVRFirstPersonApplyConfiguredSettings();
}

#if MKW_OPENXR_GRAPHICS_BACKEND

#if defined(_WIN32)
using GraphicsBackend = OpenXRD3D12Backend;
inline constexpr const char* kGraphicsBackendName = "D3D12";
#else
using GraphicsBackend = OpenXRVulkanBackend;
inline constexpr const char* kGraphicsBackendName = "Vulkan";
#endif

struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

Quaternion Normalize(Quaternion value) noexcept {
    const float length_squared = value.x * value.x + value.y * value.y +
                                 value.z * value.z + value.w * value.w;
    if (!(length_squared > 1.0e-12f)) {
        return {};
    }
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    value.x *= inverse_length;
    value.y *= inverse_length;
    value.z *= inverse_length;
    value.w *= inverse_length;
    return value;
}

Quaternion Conjugate(Quaternion value) noexcept {
    return {-value.x, -value.y, -value.z, value.w};
}

Quaternion Multiply(const Quaternion& left, const Quaternion& right) noexcept {
    return Normalize({
        left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z,
    });
}

std::array<float, 3> Rotate(const Quaternion& q, const std::array<float, 3>& value) noexcept {
    // Expanded q * [v,0] * conjugate(q), avoiding two temporary normalizations.
    const float tx = 2.0f * (q.y * value[2] - q.z * value[1]);
    const float ty = 2.0f * (q.z * value[0] - q.x * value[2]);
    const float tz = 2.0f * (q.x * value[1] - q.y * value[0]);
    return {
        value[0] + q.w * tx + (q.y * tz - q.z * ty),
        value[1] + q.w * ty + (q.z * tx - q.x * tz),
        value[2] + q.w * tz + (q.x * ty - q.y * tx),
    };
}

void RotationMatrix(const Quaternion& value, float matrix[9]) noexcept {
    const Quaternion q = Normalize(value);
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;
    matrix[0] = 1.0f - 2.0f * (yy + zz);
    matrix[1] = 2.0f * (xy - wz);
    matrix[2] = 2.0f * (xz + wy);
    matrix[3] = 2.0f * (xy + wz);
    matrix[4] = 1.0f - 2.0f * (xx + zz);
    matrix[5] = 2.0f * (yz - wx);
    matrix[6] = 2.0f * (xz - wy);
    matrix[7] = 2.0f * (yz + wx);
    matrix[8] = 1.0f - 2.0f * (xx + yy);
}

// Midpoint between the eyes: the head position the tracking origin is latched
// to. Callers check XR_VIEW_STATE_POSITION_VALID_BIT first.
std::array<float, 3> CenterPosition(const OpenXRFrame& frame) noexcept {
    const auto& left = frame.views[0].pose;
    const auto& right = frame.views[1].pose;
    return {
        (left.position.x + right.position.x) * 0.5f,
        (left.position.y + right.position.y) * 0.5f,
        (left.position.z + right.position.z) * 0.5f,
    };
}

// Places an upright screen `distance` metres ahead of the head. Only the
// head's yaw is used, so the screen is never pitched or rolled by whatever the
// player's head happened to be doing when it was anchored.
XrPosef ScreenPoseAhead(const OpenXRFrame& frame, float distance) noexcept {
    const auto& q = frame.views[0].pose.orientation;
    const float yaw =
        std::atan2(2.0f * (q.x * q.z + q.w * q.y), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const std::array<float, 3> center = CenterPosition(frame);
    XrPosef pose{};
    pose.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    pose.position = {center[0] - std::sin(yaw) * distance, center[1],
                     center[2] - std::cos(yaw) * distance};
    return pose;
}

void IdentityEye(AuroraStereoEye& eye) noexcept {
    std::fill(std::begin(eye.projection), std::end(eye.projection), 0.0f);
    eye.projection[0] = 1.0f;
    eye.projection[5] = 1.0f;
    eye.projection[10] = 1.0f;
    eye.projection[15] = 1.0f;
    std::fill(std::begin(eye.viewFromCenter), std::end(eye.viewFromCenter), 0.0f);
    eye.viewFromCenter[0] = 1.0f;
    eye.viewFromCenter[5] = 1.0f;
    eye.viewFromCenter[10] = 1.0f;
}

void ProjectionFromFov(const XrFovf& fov, float output[16]) noexcept {
    const float left = std::tan(fov.angleLeft);
    const float right = std::tan(fov.angleRight);
    const float down = std::tan(fov.angleDown);
    const float up = std::tan(fov.angleUp);
    const float inverse_width = 1.0f / (right - left);
    const float inverse_height = 1.0f / (up - down);
    std::fill(output, output + 16, 0.0f);
    output[0] = 2.0f * inverse_width;
    output[2] = (right + left) * inverse_width;
    output[5] = 2.0f * inverse_height;
    output[6] = (up + down) * inverse_height;
}

// The base is a position and nothing else. OpenXR keeps its reference spaces
// gravity-aligned, so handing the headset's rotation to the game camera as-is
// leaves the game's horizon level and its forward fixed to the reference space.
// Composing a latched head orientation in here instead would bake that instant's
// pitch and roll into the neutral and tilt the horizon for the rest of the session.
//
// lean_back_radians is the one deliberate exception: a fixed pitch of the game
// camera about the reference space's right axis, for a player sitting reclined.
// It multiplies in on the right, so it turns the world before the head rotation
// rather than after it, which is what makes it cancel a reclined head exactly
// and, when you then look sideways, roll the view the way a real recline would.
void ViewFromBase(const XrPosef& eye_pose, const std::array<float, 3>& base_position,
                  bool position_valid, float units_per_meter, float lean_back_radians,
                  float output[12]) noexcept {
    const Quaternion eye = Normalize({eye_pose.orientation.x, eye_pose.orientation.y,
                                      eye_pose.orientation.z, eye_pose.orientation.w});
    const Quaternion inverse_eye = Conjugate(eye);
    float rotation[9];
    if (lean_back_radians == 0.0f) {
        RotationMatrix(inverse_eye, rotation);
    } else {
        const float half_angle = 0.5f * lean_back_radians;
        const Quaternion lean{std::sin(half_angle), 0.0f, 0.0f, std::cos(half_angle)};
        RotationMatrix(Multiply(inverse_eye, lean), rotation);
    }

    std::array<float, 3> translation{};
    if (position_valid) {
        const std::array<float, 3> base_to_eye{
            base_position[0] - eye_pose.position.x,
            base_position[1] - eye_pose.position.y,
            base_position[2] - eye_pose.position.z,
        };
        translation = Rotate(inverse_eye, base_to_eye);
    }
    output[0] = rotation[0];
    output[1] = rotation[1];
    output[2] = rotation[2];
    output[3] = translation[0] * units_per_meter;
    output[4] = rotation[3];
    output[5] = rotation[4];
    output[6] = rotation[5];
    output[7] = translation[1] * units_per_meter;
    output[8] = rotation[6];
    output[9] = rotation[7];
    output[10] = rotation[8];
    output[11] = translation[2] * units_per_meter;
}

// Distinct names from openxr_runtime.cpp's helpers: both files can share a
// unity-build translation unit and the same anonymous namespace.
const char* DiagnosticSpaceName(XrReferenceSpaceType type) noexcept {
    switch (type) {
    case XR_REFERENCE_SPACE_TYPE_VIEW:
        return "VIEW";
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
        return "LOCAL";
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        return "STAGE";
    default:
        return "OTHER";
    }
}

const char* DiagnosticBlendModeName(XrEnvironmentBlendMode mode) noexcept {
    switch (mode) {
    case XR_ENVIRONMENT_BLEND_MODE_OPAQUE:
        return "OPAQUE";
    case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE:
        return "ADDITIVE";
    case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND:
        return "ALPHA_BLEND";
    default:
        return "OTHER";
    }
}

// What the runtime reported about the eyes this frame. The cant is the angle
// between the two eyes' forward axes: zero for parallel displays, and the
// headset's display tilt on canted ones (Pimax) unless the runtime is asked for
// parallel projections.
diagnostics::ViewGeometry DiagnosticViewGeometry(const OpenXRBackendFrame& frame) noexcept {
    constexpr float kRadiansToDegrees = 57.29577951f;
    diagnostics::ViewGeometry geometry{};
    std::array<std::array<float, 3>, kOpenXREyeCount> forward{};
    for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
        const XrView& view = frame.xr_frame.views[eye];
        geometry.fov_degrees[eye] = {view.fov.angleLeft * kRadiansToDegrees, view.fov.angleRight * kRadiansToDegrees,
                                     view.fov.angleUp * kRadiansToDegrees, view.fov.angleDown * kRadiansToDegrees};
        const auto& q = view.pose.orientation;
        forward[eye] = Rotate(Normalize({q.x, q.y, q.z, q.w}), {0.0f, 0.0f, -1.0f});
        geometry.width[eye] = frame.render_width[eye];
        geometry.height[eye] = frame.render_height[eye];
    }
    const float dot = forward[0][0] * forward[1][0] + forward[0][1] * forward[1][1] + forward[0][2] * forward[1][2];
    geometry.cant_degrees = std::acos(std::clamp(dot, -1.0f, 1.0f)) * kRadiansToDegrees;
    if ((frame.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0) {
        const auto& left = frame.xr_frame.views[0].pose.position;
        const auto& right = frame.xr_frame.views[1].pose.position;
        const float dx = right.x - left.x;
        const float dy = right.y - left.y;
        const float dz = right.z - left.z;
        geometry.ipd_millimeters = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0f;
    }
    return geometry;
}

class OpenXRIntegration final {
public:
    static OpenXRIntegration& Get() {
        static OpenXRIntegration integration;
        return integration;
    }

    OpenXRStartupResult Prepare(AuroraConfig& aurora_config) {
        Shutdown();
        {
            std::lock_guard lock(error_mutex_);
            last_error_.clear();
        }
        if (graphics_retained_) {
            SetError(std::string("OpenXR cannot be restarted after an unfenceable ") +
                     kGraphicsBackendName + " submission");
            return OpenXRStartupResult::Unavailable;
        }
        // The pacing thread is not running here (Shutdown above joined it), so
        // the sink may be replaced.
        diagnostics::SetLogSink(
            [](std::string_view line) { RT_LOG(RT_TAG_RUNTIME) << line << std::endl; });
        diagnostics::SetEnabled(RuntimeConfigFile::DiagnosticsOpenXRLogging(false));
        requested_ = RuntimeConfigFile::VrEnabled(kVrEnabledDefault);
        ConfigurePolicy(requested_);
        if (!requested_) {
            return OpenXRStartupResult::Disabled;
        }
        if (!BackendMatchesConfiguredGraphicsApi(aurora_config)) {
            return OpenXRStartupResult::Unavailable;
        }

        logger_ = [](OpenXRLogLevel level, std::string_view message) {
            const char* name = level == OpenXRLogLevel::Error ? "error" :
                               level == OpenXRLogLevel::Warning ? "warning" : "info";
            RT_LOG(RT_TAG_RUNTIME) << "[openxr::" << name << "] " << message << std::endl;
        };
#if defined(__ANDROID__)
        {
            std::string loader_error;
            if (!OpenXRAndroidInitializeLoader(logger_, &loader_error)) {
                SetError("OpenXR Android loader initialization failed: " + loader_error);
                return OpenXRStartupResult::Unavailable;
            }
        }
#endif
        runtime_ = std::make_unique<OpenXRRuntime>(logger_);
        backend_ = std::make_unique<GraphicsBackend>(logger_);

        OpenXRConfig config{};
        config.application_name = aurora_config.appName != nullptr ? aurora_config.appName
                                                                    : "WiiCompiled";
        config.engine_name = "Aurora";
        config.resolution_scale = RuntimeConfigFile::VrRenderScale(1.0f);
#if defined(_WIN32)
        config.required_extensions = {"XR_KHR_D3D12_enable"};
        config.optional_extensions = {"XR_KHR_win32_convert_performance_counter_time",
                                      "XR_FB_display_refresh_rate"};
#else
        // Either Vulkan binding extension is acceptable; the backend picks
        // whichever the runtime enabled, preferring enable2.
        config.required_extensions = {"XR_KHR_android_create_instance"};
        config.optional_extensions = {"XR_KHR_vulkan_enable2", "XR_KHR_vulkan_enable",
                                      "XR_KHR_convert_timespec_time",
                                      "XR_KHR_android_thread_settings",
                                      "XR_FB_display_refresh_rate"};
        config.instance_create_next = OpenXRAndroidInstanceCreateNext();
#endif
        if (!runtime_->Initialize(config)) {
            SetError("OpenXR instance initialization failed: " + runtime_->LastError().message);
            ResetPreparedObjects();
            return OpenXRStartupResult::Unavailable;
        }
        const auto& extensions = runtime_->EnabledExtensions();
        const auto has_extension = [&](const char* name) {
            return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
        };
#if defined(_WIN32)
        if (has_extension("XR_KHR_win32_convert_performance_counter_time")) {
            runtime_->LoadFunction("xrConvertTimeToWin32PerformanceCounterKHR", &convert_display_time_);
        }
#else
        if (has_extension("XR_KHR_convert_timespec_time")) {
            runtime_->LoadFunction("xrConvertTimeToTimespecTimeKHR", &convert_display_time_);
        }
#endif
        if (has_extension("XR_FB_display_refresh_rate")) {
            runtime_->LoadFunction("xrGetDisplayRefreshRateFB", &get_display_refresh_rate_);
        }
        interpolation_available_.store(convert_display_time_ != nullptr, std::memory_order_release);
        if (!backend_->QueryGraphicsRequirements(*runtime_)) {
            SetError(backend_->LastError());
            ResetPreparedObjects();
            return OpenXRStartupResult::Unavailable;
        }

        ApplyGraphicsRequirements(aurora_config);
        prepared_ = true;
        return OpenXRStartupResult::Prepared;
    }

    bool Start(AuroraBackend active_backend) {
        if (!prepared_ || runtime_ == nullptr || backend_ == nullptr) {
            return !requested_;
        }
        if (active_backend != kRequiredAuroraBackend) {
            SetError(std::string("Aurora could not create the OpenXR-required ") +
                     kGraphicsBackendName + " backend");
            ResetPreparedObjects();
            return false;
        }
        if (!backend_->BindAurora(*runtime_)) {
            SetError(backend_->LastError());
            ResetPreparedObjects();
            return false;
        }
        input_ = std::make_unique<OpenXRInput>(logger_);
        if (!input_->Create(*runtime_)) {
            RT_LOG(RT_TAG_RUNTIME) << "OpenXR controller input unavailable: " << input_->LastError()
                                   << std::endl;
            input_.reset();
        }

        stop_.store(false, std::memory_order_release);
        {
            std::lock_guard lock(interpolation_mutex_);
            interpolation_stopping_ = false;
        }
        teardown_requested_.store(false, std::memory_order_release);
        WithdrawPublishedFrame();
        aurora_set_stereo_frame_provider(&OpenXRIntegration::ProvideStereoFrame, this);
        provider_registered_ = true;
        if (convert_display_time_ != nullptr) {
            diagnostics::SetDisplayTimeConverter(
                [this](int64_t xr_time) { return static_cast<int64_t>(DisplayTimeNanos(xr_time)); });
        }
        running_.store(true, std::memory_order_release);
        try {
            pacing_thread_ = std::thread([this] { PacingThread(); });
        } catch (const std::exception& exception) {
            running_.store(false, std::memory_order_release);
            aurora_set_stereo_frame_provider(nullptr, nullptr);
            provider_registered_ = false;
            SetError(std::string("could not start the OpenXR pacing thread: ") + exception.what());
            ResetPreparedObjects();
            return false;
        }
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR asynchronous " << kGraphicsBackendName
                               << " presentation started" << std::endl;
        return true;
    }

    void Shutdown() noexcept {
        teardown_requested_.store(false, std::memory_order_release);
        // Stop idle replays before draining; no new worker job may race provider removal.
        {
            std::lock_guard lock(interpolation_mutex_);
            interpolation_stopping_ = true;
            aurora_set_stereo_frame_interpolation(false);
        }
        if (pacing_thread_.joinable()) {
            // Registration changes are only safe while no sealed frame is in
            // flight. The caller invokes us before Aurora teardown.
            aurora_quiesce_frame_worker();
            aurora_set_stereo_frame_provider(nullptr, nullptr);
            provider_registered_ = false;
            WithdrawPublishedFrame();
            {
                // Pair the predicate update with the wait mutex. Otherwise a
                // terminal pacing thread can observe false, miss the notify,
                // and make join wait forever.
                std::lock_guard lock(stop_mutex_);
                stop_.store(true, std::memory_order_release);
            }
            stop_cv_.notify_all();
            pacing_thread_.join();
        } else {
            if (provider_registered_) {
                aurora_quiesce_frame_worker();
                aurora_set_stereo_frame_provider(nullptr, nullptr);
                provider_registered_ = false;
            }
            ShutdownOrRetainGraphicsObjects();
        }
        running_.store(false, std::memory_order_release);
        MkwVRPolicySetSessionActive(false);
        // The converter reads runtime_; the pacing thread has stopped using it.
        diagnostics::SetDisplayTimeConverter({});
        backend_.reset();
        runtime_.reset();
        prepared_ = false;
        convert_display_time_ = nullptr;
        get_display_refresh_rate_ = nullptr;
        headset_hz_.store(0, std::memory_order_relaxed);
        rendered_fps_.store(0, std::memory_order_relaxed);
        interpolation_available_.store(false, std::memory_order_release);
        ResetTrackingOrigin();
        applied_session_run_serial_ = 0;
        session_was_active_ = false;
    }

    bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    void RequestRecenter() noexcept {
        recenter_requested_.store(true, std::memory_order_release);
    }

    void SetFrameInterpolationFps(uint32_t target) noexcept {
        frame_interpolation_fps_.store(NormalizeFrameInterpolationFps(target), std::memory_order_relaxed);
    }

    OpenXRFrameTiming FrameTiming() const noexcept {
        return {headset_hz_.load(std::memory_order_relaxed), rendered_fps_.load(std::memory_order_relaxed)};
    }

    bool FrameInterpolationAvailable() const noexcept {
        return interpolation_available_.load(std::memory_order_acquire);
    }

    void SetLeanBackDegrees(float degrees) noexcept {
        lean_back_degrees_.store(
            std::clamp(degrees, -RuntimeConfigFile::kVrLeanBackDegreesLimit,
                       RuntimeConfigFile::kVrLeanBackDegreesLimit),
            std::memory_order_relaxed);
    }

    void ServiceProducerFrameBoundary() noexcept {
        if (teardown_requested_.load(std::memory_order_acquire)) {
            Shutdown();
        }
    }

    std::string LastError() const {
        std::lock_guard lock(error_mutex_);
        return last_error_;
    }

private:
    struct PublishedFrame {
        AuroraStereoFrame frame{};
    };

#if defined(_WIN32)
    static constexpr AuroraBackend kRequiredAuroraBackend = BACKEND_D3D12;
#else
    static constexpr AuroraBackend kRequiredAuroraBackend = BACKEND_VULKAN;
#endif

    bool BackendMatchesConfiguredGraphicsApi(const AuroraConfig& aurora_config) {
        if (aurora_config.desiredBackend == BACKEND_AUTO ||
            aurora_config.desiredBackend == kRequiredAuroraBackend) {
            return true;
        }
        SetError(std::string("OpenXR requires the ") + kGraphicsBackendName +
                 " graphics backend on this platform");
        return false;
    }

    void ApplyGraphicsRequirements(AuroraConfig& aurora_config) {
        aurora_config.desiredBackend = kRequiredAuroraBackend;
        aurora_config.xrInterop = true;
#if defined(_WIN32)
        const auto& requirements = backend_->GraphicsRequirements();
        aurora_config.hasD3D12AdapterLuid = true;
        aurora_config.d3d12AdapterLuidLow = requirements.adapter_luid_low;
        aurora_config.d3d12AdapterLuidHigh = requirements.adapter_luid_high;
#endif
    }

    void ResetPreparedObjects() {
        diagnostics::SetDisplayTimeConverter({});
        ShutdownOrRetainGraphicsObjects();
        input_.reset();
        backend_.reset();
        runtime_.reset();
        prepared_ = false;
    }

    bool ShutdownOrRetainGraphicsObjects() noexcept {
        if (input_ != nullptr) {
            // Actions belong to the session and must go before it does.
            input_->Destroy();
            input_.reset();
        }
        if (backend_ != nullptr && !backend_->Shutdown()) {
            RT_LOG(RT_TAG_RUNTIME)
                << "OpenXR " << kGraphicsBackendName
                << " queue completion is unknown; retaining the backend, "
                   "runtime, session, and graphics resources until process exit"
                << std::endl;
            (void)backend_.release();
            (void)runtime_.release();
            graphics_retained_ = true;
            return false;
        }
        if (runtime_ != nullptr) {
            runtime_->Shutdown();
        }
        return true;
    }

    static bool ProvideStereoFrame(uint32_t, AuroraStereoFrame* output, void* userdata) {
        auto* self = static_cast<OpenXRIntegration*>(userdata);
        if (self == nullptr || output == nullptr) {
            return false;
        }
        // The packet storage is reused by the XR thread. Claim and copy it
        // under one short lock so cancellation cannot begin the next packet
        // while this callback is preempted between exchange and copy.
        std::lock_guard lock(self->published_mutex_);
        PublishedFrame* frame = self->published_.exchange(nullptr, std::memory_order_acq_rel);
        if (frame == nullptr) {
            return false;
        }
        diagnostics::NotePacketConsumed();
        *output = frame->frame;
        return true;
    }

    void PacingThread() noexcept {
#if defined(__ANDROID__)
        if (runtime_ != nullptr) {
            OpenXRAndroidRegisterThread(*runtime_, OpenXRAndroidThreadType::RendererMain);
        }
#endif
        bool fatal = false;
        bool presentation_logged = false;
        VRPresentationMode logged_presentation = VRPresentationMode::Desktop;
        uint32_t presentation_log_count = 0;
        bool immersive_submission_logged = false;
        while (!stop_.load(std::memory_order_acquire) && !fatal) {
            const OpenXREventStatus events = runtime_->PollEvents();
            const bool session_active = runtime_->IsSessionRunning();
            MkwVRPolicySetSessionActive(session_active);
            const uint64_t session_run_serial = runtime_->SessionRunSerial();
            if (session_run_serial != applied_session_run_serial_) {
                applied_session_run_serial_ = session_run_serial;
                ResetTrackingOrigin();
                diagnostics::OnSessionStarted();
            }
            if (session_active != session_was_active_) {
                session_was_active_ = session_active;
                if (!session_active) {
                    ResetTrackingOrigin();
                }
            }
            if (events == OpenXREventStatus::ExitRequested) {
                SetError("OpenXR runtime requested session exit; continuing on the mirror output");
                break;
            }
            if (events == OpenXREventStatus::Error) {
                SetError("OpenXR event processing failed: " + runtime_->LastError().message);
                break;
            }
            if (!session_active) {
                if (input_ != nullptr) {
                    input_->Idle();
                }
                SetInterpolationActive(false);
                interpolation_pacing_.Reset();
                rendered_fps_.store(0, std::memory_order_relaxed);
                WaitForStopOrDelay(std::chrono::milliseconds(5));
                continue;
            }

            const MkwVRPolicySnapshot policy = MkwVRPolicyGetSnapshot();
            // Diagnostics lift the cap: a presentation flickering between the
            // race and the virtual screen is exactly what a report needs to show.
            if ((!presentation_logged || policy.presentation != logged_presentation) &&
                (presentation_log_count < 16 || diagnostics::Enabled())) {
                presentation_logged = true;
                logged_presentation = policy.presentation;
                ++presentation_log_count;
                RT_LOG(RT_TAG_RUNTIME)
                    << "[mkw-vr] presentation="
                    << (policy.presentation == VRPresentationMode::ImmersiveRace
                            ? "immersive-race"
                            : policy.presentation == VRPresentationMode::VirtualScreen
                                  ? "virtual-screen"
                                  : "desktop")
                    << ", scene=" << static_cast<unsigned>(policy.scene.mode)
                    << ", screens=" << policy.scene.local_player_count
                    << ", camera-valid=" << policy.camera.valid
                    << ", scene-frame=" << policy.scene.guest_frame_index
                    << ", camera-frame=" << policy.camera.guest_frame_index
                    << ", bindings=0x" << std::hex << policy.available_bindings
                    << std::dec << std::endl;
            }
            OpenXRPresentation presentation{};
            const bool immersive = policy.presentation == VRPresentationMode::ImmersiveRace;
            presentation.mode = immersive ? OpenXRFrameMode::ImmersiveProjection
                                           : OpenXRFrameMode::VirtualScreen;
            presentation.quad_distance_meters = policy.config.hud_distance_meters;
            presentation.quad_width_meters = policy.config.hud_width_meters;

            // Updating this on the owner thread also confines retained replay to
            // validated race content. The provider checks policy tags again.
            const uint32_t interpolation_target = frame_interpolation_fps_.load(std::memory_order_relaxed);
            SetInterpolationActive(immersive && FrameInterpolationAvailable() && interpolation_target != 0);

            OpenXRBackendFrame frame{};
            const OpenXRBeginStatus begin = backend_->BeginFrame(presentation, frame);
            if (begin == OpenXRBeginStatus::SessionNotRunning) {
                MkwVRPolicySetSessionActive(false);
                continue;
            }
            if (begin == OpenXRBeginStatus::ExitRequested) {
                SetError("OpenXR runtime requested session exit; continuing on the mirror output");
                break;
            }
            if (begin == OpenXRBeginStatus::Error) {
                SetError(backend_->LastError());
                fatal = true;
                break;
            }

            UpdateFrameTiming(frame.xr_frame);
            if (diagnostics::Enabled()) {
                NoteFrameDiagnostics(frame, immersive);
            }
            // Both of these read this frame's located head pose and must run
            // before FinishFrame submits a layer built from it.
            ServiceRecenterRequest();
            UpdateVirtualScreenPose(frame);
            if (input_ != nullptr) {
                // After the screen is placed, so the pointer aims at this
                // frame's screen rather than the previous one's.
                input_->Sync(frame.xr_frame.predicted_display_time, PointerScreen(frame, policy, immersive));
            }

            if (!frame.expects_gpu_submission) {
                if (!backend_->FinishFrame(frame, false)) {
                    SetError(backend_->LastError());
                    fatal = true;
                }
                continue;
            }

            if (aurora_get_stereo_frame_interpolation() &&
                !interpolation_pacing_.ShouldRender(frame.xr_frame.predicted_display_time, interpolation_target)) {
                diagnostics::OnInterpolationSkip();
                if (!backend_->TryCancelPendingFrame(frame) || !backend_->FinishFrame(frame, false)) {
                    SetError(backend_->LastError());
                    fatal = true;
                }
                continue;
            }

            {
                std::lock_guard lock(published_mutex_);
                // First person renders at life-size scale, third person at the
                // configured diorama scale. Head translation and IPD are the
                // only things this multiplies, so a one-frame disagreement with
                // the camera's own switch is not observable.
                BuildPublishedFrame(frame, immersive, policy.EffectiveUnitsPerMeter(),
                                    policy.content_tag);
                diagnostics::OnPacketPublished();
                published_.store(&published_frame_, std::memory_order_release);
            }
            aurora_notify_stereo_frame();

            OpenXRSubmissionStatus submission = OpenXRSubmissionStatus::Timeout;
            bool canceled_before_encode = false;
            const auto cancel_after =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            while (!stop_.load(std::memory_order_acquire) &&
                   submission == OpenXRSubmissionStatus::Timeout) {
                // Fresh rendering wakes us immediately. A 50 ms keep-alive
                // protects stalls without issuing eager repeats during GPU work.
                submission = backend_->WaitForSubmission(frame, 50);
                if (submission == OpenXRSubmissionStatus::Timeout) {
                    // A pause, minimized window, or guest stall may leave no GX
                    // frame to consume this packet. Withdraw it, then cancel the
                    // matching bridge target only if Encode has not taken ownership.
                    if (std::chrono::steady_clock::now() >= cancel_after) {
                        WithdrawPublishedFrame();
                        canceled_before_encode = backend_->TryCancelPendingFrame(frame);
                        if (canceled_before_encode) {
                            diagnostics::OnPacketCanceled();
                            break;
                        }
                    }
                    diagnostics::OnKeepaliveRepeat();
                    if (!backend_->RepeatFrame(frame)) {
                        SetError(backend_->LastError());
                        fatal = true;
                        break;
                    }
                }
            }
            WithdrawPublishedFrame();
            if (stop_.load(std::memory_order_acquire)) {
                // Aurora has been drained by Shutdown(); backend shutdown below
                // cancels its pending target, then either safely releases the
                // XR image or retains the entire graph if GPU completion is unknown.
                break;
            }
            if (canceled_before_encode) {
                if (!backend_->FinishFrame(frame, false)) {
                    SetError(backend_->LastError());
                    fatal = true;
                }
                continue;
            }
            if (fatal) {
                break;
            }
            const bool submit = submission == OpenXRSubmissionStatus::Success;
            diagnostics::OnSubmission(submit);
            if (!backend_->FinishFrame(frame, submit)) {
                SetError(backend_->LastError());
                fatal = true;
            } else if (!submit) {
                SetError(std::string("Aurora's ") + kGraphicsBackendName +
                         " stereo copy failed; continuing on the mirror output");
                fatal = true;
            } else {
                ++timing_submissions_;
            }
            if (submit && !fatal && immersive && !immersive_submission_logged) {
                immersive_submission_logged = true;
                RT_LOG(RT_TAG_RUNTIME)
                    << "[mkw-vr] first immersive packet consumed and submitted as "
                       "an OpenXR projection layer"
                    << std::endl;
            }
        }

        SetInterpolationActive(false);
        running_.store(false, std::memory_order_release);
        MkwVRPolicySetSessionActive(false);
        if (!stop_.load(std::memory_order_acquire)) {
            // A runtime/backend failure can happen while Aurora is submitting.
            // Ask the producer to reach a safe frame boundary, drain Aurora,
            // and unregister the provider before this XR owner destroys state.
            teardown_requested_.store(true, std::memory_order_release);
            std::unique_lock lock(stop_mutex_);
            stop_cv_.wait(lock, [this] { return stop_.load(std::memory_order_acquire); });
        }
        ShutdownOrRetainGraphicsObjects();
    }

    void BuildPublishedFrame(const OpenXRBackendFrame& source, bool immersive,
                             float units_per_meter, uint64_t content_tag) noexcept {
        ApplyPendingReferenceSpaceChange(source.xr_frame);
        auto& destination = published_frame_.frame;
        destination = {};
        destination.frameToken = source.xr_frame.serial;
        destination.contentTag = content_tag;
        destination.displayTimeNanos = DisplayTimeNanos(source.xr_frame.predicted_display_time);
        destination.mode = immersive ? AURORA_STEREO_FRAME_IMMERSIVE_REPLAY
                                      : AURORA_STEREO_FRAME_VIRTUAL_SCREEN;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            destination.eyes[eye].width = source.render_width[eye];
            destination.eyes[eye].height = source.render_height[eye];
            IdentityEye(destination.eyes[eye]);
        }
        if (!immersive) {
            last_immersive_ = false;
            return;
        }

        const bool position_valid =
            (source.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        // Latch on the first immersive frame, after a recenter or an origin
        // change, and on re-entry from the virtual screen so a race start
        // recenters a player who shifted during the menus. Position only: the
        // heading and the horizon belong to the reference space, so no
        // transition here can tilt the view or redefine forward.
        if (position_valid && (!base_position_valid_ || !last_immersive_)) {
            base_position_ = CenterPosition(source.xr_frame);
            base_position_valid_ = true;
        }
        last_immersive_ = true;
        // Read once so both eyes are built from the same angle even if the
        // settings slider moves between them.
        const float lean_back_radians =
            lean_back_degrees_.load(std::memory_order_relaxed) * kDegreesToRadians;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            ProjectionFromFov(source.xr_frame.views[eye].fov,
                              destination.eyes[eye].projection);
            ViewFromBase(source.xr_frame.views[eye].pose, base_position_,
                         position_valid && base_position_valid_, units_per_meter,
                         lean_back_radians, destination.eyes[eye].viewFromCenter);
        }
    }

    // Runs once per located frame, before the virtual screen is placed and
    // before the immersive origin is latched, so a recenter reaches both from
    // this frame's head pose rather than the next one's.
    void ServiceRecenterRequest() noexcept {
        if (recenter_requested_.exchange(false, std::memory_order_acq_rel)) {
            ResetTrackingOrigin();
        }
    }

    // Anchors the menu screen in the application space and holds it there. The
    // pose is captured once, from the first frame whose head pose is good enough
    // to place it, and released again by a recenter or an origin change.
    void UpdateVirtualScreenPose(OpenXRBackendFrame& frame) noexcept {
        if (frame.presentation.mode != OpenXRFrameMode::VirtualScreen) {
            return;
        }
        constexpr XrViewStateFlags kPoseUsable =
            XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        if (!virtual_screen_pose_valid_ && frame.xr_frame.views_valid &&
            (frame.xr_frame.view_state_flags & kPoseUsable) == kPoseUsable) {
            virtual_screen_pose_ = ScreenPoseAhead(
                frame.xr_frame, std::max(0.25f, frame.presentation.quad_distance_meters));
            virtual_screen_pose_valid_ = true;
        }
        frame.presentation.quad_anchored = virtual_screen_pose_valid_;
        frame.presentation.quad_pose = virtual_screen_pose_;
    }

    // The rectangle the game picture covers on the screen this frame shows, in
    // the application space, for the Wii Remote pointer to aim at.
    //
    // Menus: the quad layer UpdateVirtualScreenPose placed (or its head-locked
    // fallback), sized like the backends size it: hud_width_meters across with
    // the eye texture's aspect, the desktop snapshot letterboxed into it and the
    // picture into the snapshot.
    //
    // Races: the 2D layer's screen, which Aurora hangs hud_distance_meters
    // ahead in the recorded centre-eye space. ViewFromBase maps a point p of
    // that space (in metres) to base + lean * p in the application space, so the
    // screen sits at base + lean * (0, 0, -distance), turned by the lean, its
    // height following the picture aspect as stereo_hud_screen's does. With the
    // 2D layer stretched across the eyes there is no screen to point at.
    OpenXRPointerScreen PointerScreen(const OpenXRBackendFrame& frame, const MkwVRPolicySnapshot& policy,
                                      bool immersive) const noexcept {
        OpenXRPointerScreen screen{};
        float picture_aspect = 0.0f;
        float snapshot_aspect = 0.0f;
        if (!aurora_get_stereo_screen_aspects(&picture_aspect, &snapshot_aspect)) {
            return screen;
        }
        const OpenXRFrame& xr_frame = frame.xr_frame;
        const bool views_usable = xr_frame.views_valid &&
                                  (xr_frame.view_state_flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
        const bool position_usable =
            views_usable && (xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;

        if (immersive) {
            const float distance = policy.config.hud_distance_meters;
            const float width = policy.config.hud_width_meters;
            if (!aurora_get_stereo_hud_screen_enabled() || !(distance > 0.0f) || !(width > 0.0f)) {
                return screen;
            }
            std::array<float, 3> base{};
            if (base_position_valid_ && last_immersive_) {
                base = base_position_;
            } else if (position_usable) {
                // BuildPublishedFrame latches exactly this for the frame.
                base = CenterPosition(xr_frame);
            } else {
                return screen;
            }
            const float half_angle =
                0.5f * lean_back_degrees_.load(std::memory_order_relaxed) * kDegreesToRadians;
            const Quaternion lean{std::sin(half_angle), 0.0f, 0.0f, std::cos(half_angle)};
            const std::array<float, 3> ahead = Rotate(lean, {0.0f, 0.0f, -distance});
            screen.pose.orientation = {lean.x, lean.y, lean.z, lean.w};
            screen.pose.position = {base[0] + ahead[0], base[1] + ahead[1], base[2] + ahead[2]};
            screen.half_width_meters = 0.5f * width;
            screen.half_height_meters = screen.half_width_meters / picture_aspect;
            screen.valid = true;
            return screen;
        }

        if (frame.presentation.mode != OpenXRFrameMode::VirtualScreen || frame.render_width[0] == 0 ||
            frame.render_height[0] == 0) {
            return screen;
        }
        if (frame.presentation.quad_anchored) {
            screen.pose = frame.presentation.quad_pose;
        } else if (position_usable) {
            // Head-locked in the view space: straight ahead of the head.
            const auto& head = xr_frame.views[0].pose.orientation;
            const Quaternion orientation = Normalize({head.x, head.y, head.z, head.w});
            const std::array<float, 3> center = CenterPosition(xr_frame);
            const std::array<float, 3> ahead = Rotate(
                orientation, {0.0f, 0.0f, -std::max(0.25f, frame.presentation.quad_distance_meters)});
            screen.pose.orientation = {orientation.x, orientation.y, orientation.z, orientation.w};
            screen.pose.position = {center[0] + ahead[0], center[1] + ahead[1], center[2] + ahead[2]};
        } else {
            return screen;
        }
        const float eye_aspect =
            static_cast<float>(frame.render_width[0]) / static_cast<float>(frame.render_height[0]);
        const std::array<float, 2> extents = wii_remote::MenuPictureHalfExtents(
            std::max(0.25f, frame.presentation.quad_width_meters), eye_aspect, snapshot_aspect, picture_aspect);
        screen.half_width_meters = extents[0];
        screen.half_height_meters = extents[1];
        screen.valid = true;
        return screen;
    }

    void ApplyPendingReferenceSpaceChange(const OpenXRFrame& frame) noexcept {
        if (runtime_->ConsumeAppSpaceChangesThrough(frame.predicted_display_time)) {
            ResetTrackingOrigin();
        }
    }

    void ResetTrackingOrigin() noexcept {
        base_position_ = {};
        base_position_valid_ = false;
        last_immersive_ = false;
        // The anchored menu screen is placed in the same space, so it is stale
        // for exactly the same reasons and is re-placed on the next frame.
        virtual_screen_pose_valid_ = false;
    }

    void SetInterpolationActive(bool active) noexcept {
        std::lock_guard lock(interpolation_mutex_);
        aurora_set_stereo_frame_interpolation(active && !interpolation_stopping_);
    }

    void UpdateFrameTiming(const OpenXRFrame& frame) noexcept {
        float hz = 0;
        if (get_display_refresh_rate_ == nullptr ||
            XR_FAILED(get_display_refresh_rate_(runtime_->Session(), &hz)) || !(hz > 0)) {
            if (frame.predicted_display_period > 0)
                hz = static_cast<float>(1.0e9 / static_cast<double>(frame.predicted_display_period));
        }
        headset_hz_.store(hz, std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - timing_start_).count();
        if (elapsed >= 1.0f) {
            rendered_fps_.store(static_cast<float>(timing_submissions_) / elapsed, std::memory_order_relaxed);
            timing_start_ = now;
            timing_submissions_ = 0;
        }
    }

    // Pacing thread, only while diagnostics are on.
    void NoteFrameDiagnostics(const OpenXRBackendFrame& frame, bool immersive) {
        const XrViewStateFlags flags = frame.xr_frame.view_state_flags;
        diagnostics::OnFrameBegun(immersive, frame.xr_frame.should_render, frame.xr_frame.views_valid,
                                  (flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0,
                                  (flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0);
        if (diagnostics::ConsumeSessionInfoRequest()) {
            LogDiagnosticSession(frame);
        }
        if (frame.xr_frame.should_render && frame.xr_frame.views_valid) {
            diagnostics::OnViewGeometry(DiagnosticViewGeometry(frame));
        }
    }

    // Everything about the headset and runtime that a pacing report is read
    // against. Written when logging starts and again for every new session.
    void LogDiagnosticSession(const OpenXRBackendFrame& frame) const {
        const auto& info = runtime_->RuntimeInfo();
        std::ostringstream line;
        line << "session: runtime '" << info.runtime_name << "' " << XR_VERSION_MAJOR(info.runtime_version) << '.'
             << XR_VERSION_MINOR(info.runtime_version) << '.' << XR_VERSION_PATCH(info.runtime_version)
             << ", system '" << info.system_name << "', vendor 0x" << std::hex << info.vendor_id << std::dec
             << ", orientation tracking " << info.supports_orientation_tracking << ", position tracking "
             << info.supports_position_tracking << ", max layers " << info.max_layer_count;
        diagnostics::Info(line.str());

        line.str({});
        line << "session: " << kGraphicsBackendName << " backend, reference space "
             << DiagnosticSpaceName(runtime_->AppSpaceType()) << ", blend mode "
             << DiagnosticBlendModeName(runtime_->EnvironmentBlendMode()) << ", extensions";
        for (const std::string& extension : runtime_->EnabledExtensions()) {
            line << ' ' << extension;
        }
        diagnostics::Info(line.str());

        line.str({});
        const auto& views = runtime_->ViewConfiguration();
        line << "session: recommended eye size " << views[0].properties.recommendedImageRectWidth << 'x'
             << views[0].properties.recommendedImageRectHeight << " / "
             << views[1].properties.recommendedImageRectWidth << 'x'
             << views[1].properties.recommendedImageRectHeight << ", max "
             << views[0].properties.maxImageRectWidth << 'x' << views[0].properties.maxImageRectHeight
             << ", render_scale " << runtime_->Config().resolution_scale << ", swapchains "
             << views[0].render_width << 'x' << views[0].render_height << " / " << views[1].render_width << 'x'
             << views[1].render_height;
        diagnostics::Info(line.str());

        line.str({});
        const uint32_t interpolation = frame_interpolation_fps_.load(std::memory_order_relaxed);
        line << "session: display period ";
        if (frame.xr_frame.predicted_display_period > 0) {
            const double period_ms = static_cast<double>(frame.xr_frame.predicted_display_period) / 1.0e6;
            line << period_ms << " ms (" << 1000.0 / period_ms << " Hz)";
        } else {
            line << "unknown";
        }
        line << ", refresh-rate extension " << (get_display_refresh_rate_ != nullptr ? "yes" : "no")
             << ", display-time conversion " << (convert_display_time_ != nullptr ? "yes" : "no")
             << ", VR frame interpolation "
             << (interpolation == 0 ? std::string("off")
                 : interpolation == 1 ? std::string("auto")
                                      : std::to_string(interpolation))
             << (FrameInterpolationAvailable() ? "" : " (unavailable)");
        diagnostics::Info(line.str());
    }

    // Converts the compositor's predicted display time onto the runtime's
    // steady clock, which is what Aurora's interpolation deadlines are paced by.
    uint64_t DisplayTimeNanos(XrTime display_time) noexcept {
        if (convert_display_time_ == nullptr) return 0;
        const auto now = std::chrono::steady_clock::now();
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
#if defined(_WIN32)
        LARGE_INTEGER display_counter{}, counter{}, frequency{};
        if (XR_FAILED(convert_display_time_(runtime_->Instance(), display_time, &display_counter)) ||
            !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
            !QueryPerformanceCounter(&counter)) return 0;
        const auto delta = static_cast<int64_t>(
            (static_cast<double>(display_counter.QuadPart) - static_cast<double>(counter.QuadPart)) *
            1.0e9 / static_cast<double>(frequency.QuadPart));
#else
        // XR_KHR_convert_timespec_time yields CLOCK_MONOTONIC, the clock behind
        // libc++'s steady_clock, so the delta is measured on that clock too.
        timespec display_spec{};
        timespec now_spec{};
        if (XR_FAILED(convert_display_time_(runtime_->Instance(), display_time, &display_spec)) ||
            clock_gettime(CLOCK_MONOTONIC, &now_spec) != 0) return 0;
        const int64_t display_ns = static_cast<int64_t>(display_spec.tv_sec) * 1'000'000'000ll + display_spec.tv_nsec;
        const int64_t monotonic_ns = static_cast<int64_t>(now_spec.tv_sec) * 1'000'000'000ll + now_spec.tv_nsec;
        const int64_t delta = display_ns - monotonic_ns;
#endif
        return now_ns + delta > 0 ? static_cast<uint64_t>(now_ns + delta) : 0;
    }

    void WaitForStopOrDelay(std::chrono::milliseconds delay) {
        std::unique_lock lock(stop_mutex_);
        stop_cv_.wait_for(lock, delay,
                          [this] { return stop_.load(std::memory_order_acquire); });
    }

    void WithdrawPublishedFrame() noexcept {
        std::lock_guard lock(published_mutex_);
        published_.store(nullptr, std::memory_order_release);
    }

    void SetError(std::string message) {
        {
            std::lock_guard lock(error_mutex_);
            last_error_ = std::move(message);
        }
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR: " << LastError() << std::endl;
    }

    OpenXRLogCallback logger_;
    std::unique_ptr<OpenXRRuntime> runtime_;
    std::unique_ptr<GraphicsBackend> backend_;
    std::unique_ptr<OpenXRInput> input_;
    std::thread pacing_thread_;
    std::atomic_bool stop_{false};
    std::atomic_bool running_{false};
    std::atomic_bool teardown_requested_{false};
    std::atomic_bool recenter_requested_{false};
    std::atomic<float> lean_back_degrees_{RuntimeConfigFile::VrLeanBackDegrees()};
    std::atomic_uint32_t frame_interpolation_fps_{RuntimeConfigFile::VrFrameInterpolationFps()};
    std::atomic_bool interpolation_available_{false};
    std::mutex interpolation_mutex_;
    bool interpolation_stopping_ = true;
    FrameInterpolationPacing interpolation_pacing_;
    std::atomic<float> headset_hz_{0};
    std::atomic<float> rendered_fps_{0};
    std::chrono::steady_clock::time_point timing_start_ = std::chrono::steady_clock::now();
    uint32_t timing_submissions_ = 0;
    PFN_xrGetDisplayRefreshRateFB get_display_refresh_rate_ = nullptr;
#if defined(_WIN32)
    using ConvertDisplayTime = XrResult (XRAPI_PTR*)(XrInstance, XrTime, LARGE_INTEGER*);
#else
    using ConvertDisplayTime = XrResult (XRAPI_PTR*)(XrInstance, XrTime, struct timespec*);
#endif
    ConvertDisplayTime convert_display_time_ = nullptr;
    std::atomic<PublishedFrame*> published_{nullptr};
    PublishedFrame published_frame_{};
    std::mutex published_mutex_;
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    mutable std::mutex error_mutex_;
    std::string last_error_;
    std::array<float, 3> base_position_{};
    bool base_position_valid_ = false;
    XrPosef virtual_screen_pose_{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    bool virtual_screen_pose_valid_ = false;
    bool last_immersive_ = false;
    uint64_t applied_session_run_serial_ = 0;
    bool session_was_active_ = false;
    bool requested_ = false;
    bool prepared_ = false;
    bool provider_registered_ = false;
    bool graphics_retained_ = false;
};

#endif // MKW_OPENXR_GRAPHICS_BACKEND

} // namespace

OpenXRStartupResult OpenXRPrepareAurora(AuroraConfig& config) {
#if !defined(MKW_ENABLE_OPENXR)
    (void)config;
    ConfigurePolicy(false);
    return RuntimeConfigFile::VrEnabled(kVrEnabledDefault) ? OpenXRStartupResult::Unavailable
                                                           : OpenXRStartupResult::Disabled;
#elif MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().Prepare(config);
#else
    (void)config;
    ConfigurePolicy(RuntimeConfigFile::VrEnabled(kVrEnabledDefault));
    if (!RuntimeConfigFile::VrEnabled(kVrEnabledDefault)) {
        return OpenXRStartupResult::Disabled;
    }
    RT_LOG(RT_TAG_RUNTIME) << "OpenXR is not wired to a graphics backend on this platform" << std::endl;
    return OpenXRStartupResult::Unavailable;
#endif
}

bool OpenXRStartAfterAurora(AuroraBackend active_backend) {
#if MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().Start(active_backend);
#else
    (void)active_backend;
    return !RuntimeConfigFile::VrEnabled(kVrEnabledDefault);
#endif
}

void OpenXRShutdownBeforeAurora() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().Shutdown();
#else
    MkwVRPolicySetSessionActive(false);
#endif
}

void OpenXRServiceProducerFrameBoundary() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().ServiceProducerFrameBoundary();
#endif
}

bool OpenXRIsRunning() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().IsRunning();
#else
    return false;
#endif
}

void OpenXRRequestRecenter() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().RequestRecenter();
#endif
}

void OpenXRSetLeanBackDegrees(float degrees) noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().SetLeanBackDegrees(degrees);
#else
    (void)degrees;
#endif
}

void OpenXRSetFrameInterpolationFps(uint32_t target) noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().SetFrameInterpolationFps(target);
#else
    (void)target;
#endif
}

OpenXRFrameTiming OpenXRGetFrameTiming() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().FrameTiming();
#else
    return {};
#endif
}

bool OpenXRFrameInterpolationAvailable() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().FrameInterpolationAvailable();
#else
    return false;
#endif
}

std::string OpenXRLastError() {
#if !defined(MKW_ENABLE_OPENXR)
    return "this build was compiled without OpenXR support";
#elif MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().LastError();
#else
    return "OpenXR is not wired to a graphics backend on this platform";
#endif
}

} // namespace mkw::vr
