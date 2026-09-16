// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MKW_ENABLE_OPENXR)

#include "vr/openxr_runtime.h"

#include <cstdint>
#include <string>

namespace mkw::vr {

// OpenXR action-based controller input, surfaced to the rest of the runtime as
// one ordinary SDL gamepad.
//
// Quest Touch controllers are not visible to SDL's joystick layer (the OS does
// not expose them as HID gamepads), so a standalone headset build would have no
// input at all. Rather than adding a second input path through the PAD/WPAD
// HLE, this module syncs an OpenXR action set on the pacing thread and feeds a
// virtual SDL joystick (SDL_AttachVirtualJoystick) that Aurora's existing
// controller code opens, maps and assigns to player 1 exactly like a physical
// pad. Every binding the settings overlay already offers keeps working.
//
// Mapping (Oculus Touch profile; the same actions are also bound for
// khr/simple_controller so an unknown runtime still gets A/menu):
//   right A / B            -> gamepad South / East (GameCube A / B)
//   left  X / Y            -> gamepad West / North (GameCube X / Y)
//   index triggers         -> left / right trigger axes
//   grip squeezes          -> left / right shoulder buttons
//   left / right thumbstick-> left / right stick axes, clicks -> stick buttons
//   left menu              -> Start
//
// Lifetime: OpenXRInputCreate after the session exists (attaches the action
// set, which OpenXR permits once per session), OpenXRInputSync once per
// xrWaitFrame when the session is focused, OpenXRInputDestroy before the
// session is destroyed. All three run on the XR pacing thread; SDL's virtual
// joystick setters are internally locked, so the game thread may read the pad
// concurrently.
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

    // xrSyncActions + state reads, then publishes to the virtual gamepad.
    // predicted_display_time is the frame's XrTime for pose-based lookups.
    void Sync(XrTime predicted_display_time);

    // Rumble for the given hand (0 = left, 1 = right); amplitude 0..1.
    void ApplyHaptic(uint32_t hand, float amplitude, XrDuration duration);

    bool IsCreated() const noexcept { return m_created; }
    bool HasVirtualGamepad() const noexcept { return m_joystick_id != 0; }
    const std::string& LastError() const noexcept { return m_last_error; }

private:
    bool CreateActions();
    bool SuggestBindings();
    bool AttachVirtualGamepad();
    void DetachVirtualGamepad();
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
    XrAction m_haptic = XR_NULL_HANDLE;
    XrPath m_hand_paths[2]{};
    uint32_t m_joystick_id = 0; // SDL_JoystickID; 0 when detached
    void* m_joystick = nullptr; // SDL_Joystick*
    bool m_created = false;
    bool m_logged_sync_failure = false;
    std::string m_last_error;
};

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR)
