// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MKW_ENABLE_OPENXR)

#include "vr/openxr_runtime.h"
#include "vr/openxr_settings_panel.h"
#include "vr/openxr_wii_remote.h"

#include <array>
#include <cstdint>
#include <string>

namespace mkw::vr {

// The rectangle the game's picture occupies on whichever virtual screen is
// showing it, in the application reference space: the target the Wii Remote
// pointer is aimed at. The pose faces +Z with +X right and +Y up across the
// picture. Invalid when no screen can be pointed at (for instance a race with
// its 2D layer left stretched across the eyes).
struct OpenXRPointerScreen {
    bool valid = false;
    XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    float half_width_meters = 0.0f;
    float half_height_meters = 0.0f;
};

// OpenXR action-based controller input.
//
// Quest Touch controllers are not visible to SDL's joystick layer (the OS does
// not expose them as HID gamepads), so a standalone headset build would have no
// input at all. This module syncs an OpenXR action set on the pacing thread and
// feeds a virtual SDL joystick (SDL_AttachVirtualJoystick) that Aurora's
// existing controller code opens and assigns to a port exactly like a physical
// pad. What the game then sees on that port depends on OpenXRControllerMode:
//
// Wii Remote (default): the port is served through KPAD as a Wii Remote with a
// Nunchuk, like DolphinXR's OpenXR Wii Remote. Every XR frame the aim and grip
// poses are located at the measured current time, turned into both
// accelerometers and the IR pointer (the right aim ray against the virtual
// screen the renderer is showing), and published through openxr_wii_remote.h.
// Buttons follow DolphinXR's "OpenXR Wii Remote" profile (see RemoteButtons).
// The game's rumble drives both controllers' haptics.
//
// Gamepad: the virtual joystick is read through PAD as a GameCube controller,
// and every binding the settings overlay offers applies:
//   right A / B            -> gamepad South / East (GameCube A / B)
//   left  X / Y            -> gamepad West / North (GameCube X / Y)
//   index triggers         -> left / right trigger axes
//   grip squeezes          -> left / right shoulder buttons
//   left / right thumbstick-> left / right stick axes, clicks -> stick buttons
//   left menu              -> Start
//
// Both are bound for the Oculus Touch profile; khr/simple_controller gets
// select/menu and the poses so an unknown runtime still offers something.
//
// Clicking both thumbsticks opens the in-headset settings panel
// (openxr_settings_panel.h). While it is open, and until every button has been
// released after it closes, the game sees idle controllers: the chord, the
// pointer and the triggers belong to the panel.
//
// Lifetime: Create after the session exists (attaches the action set, which
// OpenXR permits once per session), Sync once per xrWaitFrame, Idle while the
// session is not running, Destroy before the session is destroyed. All of them
// run on the XR pacing thread; SDL's virtual joystick setters and the Wii
// Remote bridge are internally locked, so the game thread may read concurrently.
class OpenXRInput final {
public:
    explicit OpenXRInput(OpenXRLogCallback logger = {});
    ~OpenXRInput();

    OpenXRInput(const OpenXRInput&) = delete;
    OpenXRInput& operator=(const OpenXRInput&) = delete;

    // Creates the action set/actions/spaces and attaches them to the session.
    // Returns false (with LastError set) if the runtime rejects the action set;
    // the caller continues without controller input rather than failing VR.
    bool Create(OpenXRRuntime& runtime);
    void Destroy();

    // xrSyncActions + state reads, then publishes to the virtual gamepad and
    // the Wii Remote bridge. predicted_display_time is the frame's XrTime;
    // screen is where the Wii Remote pointer can land this frame, and
    // settings_panel where the settings panel is (its whole rectangle).
    void Sync(XrTime predicted_display_time, const OpenXRPointerScreen& screen,
              const OpenXRPointerScreen& settings_panel);

    // Publishes a remote with nothing held, at rest and not pointing, and stops
    // the haptics, for frames without focused input.
    void Idle();

    // Rumble for the given hand (0 = left, 1 = right); amplitude 0..1.
    void ApplyHaptic(uint32_t hand, float amplitude, XrDuration duration);

    bool IsCreated() const noexcept { return m_created; }
    bool HasVirtualGamepad() const noexcept { return m_joystick_id != 0; }
    const std::string& LastError() const noexcept { return m_last_error; }

private:
    static constexpr uint32_t kHands = 2;

    bool CreateActions();
    bool SuggestBindings();
    void CreatePoseSpaces();
    void DestroyPoseSpaces();
    void LoadInputClock();
    XrTime InputSampleTime(XrTime predicted_display_time) const;
    bool AttachVirtualGamepad();
    void DetachVirtualGamepad();
    // `withheld` publishes a remote at rest with nothing held and no pointer,
    // while still tracking motion so releasing it does not read as a jolt.
    void PublishWiiRemote(XrTime input_time, const OpenXRPointerScreen& screen,
                          const std::array<wii_remote::HandInputs, kHands>& hands, uint32_t injected_buttons,
                          bool withheld);
    void PublishSettingsPanel(XrTime input_time, const OpenXRPointerScreen& panel,
                              const settings_panel::Frame& frame);
    void UpdateRumble();
    void StopRumble();
    bool Check(XrResult result, const char* operation);
    void Log(OpenXRLogLevel level, const std::string& message) const noexcept;

    OpenXRLogCallback m_logger;
    OpenXRRuntime* m_runtime = nullptr;
    XrActionSet m_action_set = XR_NULL_HANDLE;
    XrAction m_thumbstick = XR_NULL_HANDLE;
    XrAction m_thumbstick_click = XR_NULL_HANDLE;
    XrAction m_trigger = XR_NULL_HANDLE;
    XrAction m_squeeze = XR_NULL_HANDLE;
    XrAction m_button_primary = XR_NULL_HANDLE;   // A / X
    XrAction m_button_secondary = XR_NULL_HANDLE; // B / Y
    XrAction m_menu = XR_NULL_HANDLE;
    XrAction m_aim_pose = XR_NULL_HANDLE;
    XrAction m_grip_pose = XR_NULL_HANDLE;
    XrAction m_haptic = XR_NULL_HANDLE;
    XrPath m_hand_paths[kHands]{};
    XrSpace m_aim_spaces[kHands]{};
    XrSpace m_grip_spaces[kHands]{};
    // xrConvertWin32PerformanceCounterToTimeKHR / xrConvertTimespecTimeToTimeKHR,
    // when the runtime offers them; the input time falls back to display time.
    PFN_xrVoidFunction m_convert_now_to_xr_time = nullptr;
    wii_remote::MotionTracker m_motion[kHands];
    wii_remote::PointerFilter m_pointer;
    settings_panel::Controls m_panel_controls;
    XrTime m_last_input_time = 0;
    bool m_panel_select_held = false;
    std::array<float, 2> m_horizon{1.0f, 0.0f};
    bool m_haptics_active[kHands]{};
    uint32_t m_joystick_id = 0; // SDL_JoystickID; 0 when detached
    void* m_joystick = nullptr; // SDL_Joystick*
    bool m_created = false;
    bool m_logged_sync_failure = false;
    bool m_logged_pointer = false;
    std::string m_last_error;
};

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR)
