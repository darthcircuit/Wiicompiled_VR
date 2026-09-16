// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MKW_ENABLE_OPENXR)

#include "vr/openxr_input.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace mkw::vr {
namespace {

#if defined(__ANDROID__)
// Debug-only remote button presses for headset experiments driven over adb, so
// a menu can be reached without someone wearing the headset:
//   adb shell setprop debug.wiicompiled.inject <sequence>:<button>
// A new sequence number holds the button for kInjectHoldFrames XR frames.
// Buttons: a, b, x, y, start, up, down, left, right. The property is unset in
// normal use, so this costs one property read every few frames.
constexpr uint32_t kInjectHoldFrames = 12;
constexpr uint32_t kInjectPollFrames = 4;

struct InjectedPress {
    long sequence = -1;
    std::string button;
    uint32_t frames_left = 0;
    uint32_t poll_countdown = 0;
};

InjectedPress& Injection() {
    static InjectedPress press;
    return press;
}

void PollInjection() {
    InjectedPress& press = Injection();
    if (press.frames_left > 0) {
        --press.frames_left;
    }
    if (press.poll_countdown > 0) {
        --press.poll_countdown;
        return;
    }
    press.poll_countdown = kInjectPollFrames;
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.wiicompiled.inject", value) <= 0) {
        return;
    }
    char* end = nullptr;
    const long sequence = std::strtol(value, &end, 10);
    if (end == value || *end != ':' || sequence == press.sequence) {
        return;
    }
    const bool first_read = press.sequence < 0;
    press.sequence = sequence;
    if (first_read) {
        return; // A value left over from an earlier run is not a new press.
    }
    press.button = end + 1;
    press.frames_left = kInjectHoldFrames;
}

bool Injected(const char* button) {
    const InjectedPress& press = Injection();
    return press.frames_left > 0 && press.button == button;
}
#else
void PollInjection() {}
bool Injected(const char*) { return false; }
#endif

constexpr uint32_t kHandCount = 2;

struct Binding {
    XrAction* action;
    const char* path;
};

Sint16 ToAxis(float value) noexcept {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<Sint16>(std::lround(clamped * 32767.0f));
}

// Trigger axes are reported by SDL gamepads on the positive half only.
Sint16 ToTrigger(float value) noexcept {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<Sint16>(std::lround(clamped * 32767.0f));
}

} // namespace

OpenXRInput::OpenXRInput(OpenXRLogCallback logger) : m_logger(std::move(logger)) {}

OpenXRInput::~OpenXRInput() {
    Destroy();
}

bool OpenXRInput::Create(OpenXRRuntime& runtime) {
    m_last_error.clear();
    if (m_created) {
        return true;
    }
    if (!runtime.IsInitialized() || !runtime.HasSession()) {
        m_last_error = "OpenXR input needs an initialized runtime with a session";
        return false;
    }
    m_runtime = &runtime;

    if (!Check(xrStringToPath(runtime.Instance(), "/user/hand/left", &m_hand_paths[0]),
               "xrStringToPath(/user/hand/left)") ||
        !Check(xrStringToPath(runtime.Instance(), "/user/hand/right", &m_hand_paths[1]),
               "xrStringToPath(/user/hand/right)")) {
        Destroy();
        return false;
    }
    if (!CreateActions() || !SuggestBindings()) {
        Destroy();
        return false;
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &m_action_set;
    if (!Check(xrAttachSessionActionSets(runtime.Session(), &attach), "xrAttachSessionActionSets")) {
        Destroy();
        return false;
    }
    m_created = true;
    if (!AttachVirtualGamepad()) {
        Log(OpenXRLogLevel::Warning,
            "SDL refused the virtual gamepad; OpenXR controllers will not reach the game");
    }
    Log(OpenXRLogLevel::Info, "OpenXR controller actions attached");
    return true;
}

bool OpenXRInput::CreateActions() {
    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(set_info.actionSetName, "mkw_gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(set_info.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    set_info.priority = 0;
    if (!Check(xrCreateActionSet(m_runtime->Instance(), &set_info, &m_action_set), "xrCreateActionSet")) {
        return false;
    }

    struct Spec {
        XrAction* action;
        const char* name;
        const char* localized;
        XrActionType type;
    };
    const std::array<Spec, 8> specs{{
        {&m_thumbstick, "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT},
        {&m_thumbstick_click, "thumbstick_click", "Thumbstick Click", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_trigger, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT},
        {&m_squeeze, "squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT},
        {&m_button_primary, "button_primary", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_button_secondary, "button_secondary", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_menu, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_haptic, "haptic", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT},
    }};
    for (const Spec& spec : specs) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        info.actionType = spec.type;
        std::strncpy(info.actionName, spec.name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(info.localizedActionName, spec.localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        info.countSubactionPaths = kHandCount;
        info.subactionPaths = m_hand_paths;
        if (!Check(xrCreateAction(m_action_set, &info, spec.action), spec.name)) {
            return false;
        }
    }
    return true;
}

bool OpenXRInput::SuggestBindings() {
    const auto suggest = [&](const char* profile, const std::vector<Binding>& bindings, bool required) {
        XrPath profile_path = XR_NULL_PATH;
        if (!Check(xrStringToPath(m_runtime->Instance(), profile, &profile_path), profile)) {
            return false;
        }
        std::vector<XrActionSuggestedBinding> suggested;
        suggested.reserve(bindings.size());
        for (const Binding& binding : bindings) {
            XrPath path = XR_NULL_PATH;
            if (XR_FAILED(xrStringToPath(m_runtime->Instance(), binding.path, &path))) {
                continue;
            }
            suggested.push_back({*binding.action, path});
        }
        XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        info.interactionProfile = profile_path;
        info.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
        info.suggestedBindings = suggested.data();
        const XrResult result = xrSuggestInteractionProfileBindings(m_runtime->Instance(), &info);
        m_runtime->ObserveResult(result);
        if (XR_FAILED(result)) {
            std::ostringstream message;
            message << "xrSuggestInteractionProfileBindings(" << profile << ") failed (" << result << ')';
            if (required) {
                m_last_error = message.str();
                Log(OpenXRLogLevel::Error, m_last_error);
                return false;
            }
            Log(OpenXRLogLevel::Warning, message.str());
        }
        return true;
    };

    // Meta Quest Touch controllers (Quest 2 / 3 / Pro all expose this profile).
    const std::vector<Binding> touch{
        {&m_thumbstick, "/user/hand/left/input/thumbstick"},
        {&m_thumbstick, "/user/hand/right/input/thumbstick"},
        {&m_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
        {&m_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
        {&m_trigger, "/user/hand/left/input/trigger/value"},
        {&m_trigger, "/user/hand/right/input/trigger/value"},
        {&m_squeeze, "/user/hand/left/input/squeeze/value"},
        {&m_squeeze, "/user/hand/right/input/squeeze/value"},
        {&m_button_primary, "/user/hand/left/input/x/click"},
        {&m_button_primary, "/user/hand/right/input/a/click"},
        {&m_button_secondary, "/user/hand/left/input/y/click"},
        {&m_button_secondary, "/user/hand/right/input/b/click"},
        {&m_menu, "/user/hand/left/input/menu/click"},
        {&m_haptic, "/user/hand/left/output/haptic"},
        {&m_haptic, "/user/hand/right/output/haptic"},
    };
    if (!suggest("/interaction_profiles/oculus/touch_controller", touch, true)) {
        return false;
    }
    // Minimal fallback so an unfamiliar runtime still offers a select and a menu.
    const std::vector<Binding> simple{
        {&m_button_primary, "/user/hand/right/input/select/click"},
        {&m_button_secondary, "/user/hand/left/input/select/click"},
        {&m_menu, "/user/hand/left/input/menu/click"},
        {&m_haptic, "/user/hand/left/output/haptic"},
        {&m_haptic, "/user/hand/right/output/haptic"},
    };
    suggest("/interaction_profiles/khr/simple_controller", simple, false);
    return true;
}

bool OpenXRInput::AttachVirtualGamepad() {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) |
                       (1u << SDL_GAMEPAD_BUTTON_WEST) | (1u << SDL_GAMEPAD_BUTTON_NORTH) |
                       (1u << SDL_GAMEPAD_BUTTON_START) | (1u << SDL_GAMEPAD_BUTTON_LEFT_STICK) |
                       (1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK) |
                       (1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) |
                       (1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY) |
                     (1u << SDL_GAMEPAD_AXIS_RIGHTX) | (1u << SDL_GAMEPAD_AXIS_RIGHTY) |
                     (1u << SDL_GAMEPAD_AXIS_LEFT_TRIGGER) | (1u << SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    desc.name = "OpenXR Touch Controllers";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (id == 0) {
        m_last_error = std::string("SDL_AttachVirtualJoystick failed: ") + SDL_GetError();
        Log(OpenXRLogLevel::Warning, m_last_error);
        return false;
    }
    SDL_Joystick* joystick = SDL_OpenJoystick(id);
    if (joystick == nullptr) {
        m_last_error = std::string("SDL_OpenJoystick failed: ") + SDL_GetError();
        Log(OpenXRLogLevel::Warning, m_last_error);
        SDL_DetachVirtualJoystick(id);
        return false;
    }
    m_joystick_id = id;
    m_joystick = joystick;
    return true;
}

void OpenXRInput::DetachVirtualGamepad() {
    if (m_joystick != nullptr) {
        SDL_CloseJoystick(static_cast<SDL_Joystick*>(m_joystick));
        m_joystick = nullptr;
    }
    if (m_joystick_id != 0) {
        SDL_DetachVirtualJoystick(m_joystick_id);
        m_joystick_id = 0;
    }
}

void OpenXRInput::Destroy() {
    DetachVirtualGamepad();
    if (m_action_set != XR_NULL_HANDLE) {
        // Destroying the set destroys every action created from it.
        xrDestroyActionSet(m_action_set);
        m_action_set = XR_NULL_HANDLE;
    }
    m_thumbstick = m_thumbstick_click = m_trigger = m_squeeze = XR_NULL_HANDLE;
    m_button_primary = m_button_secondary = m_menu = m_haptic = XR_NULL_HANDLE;
    m_hand_paths[0] = m_hand_paths[1] = XR_NULL_PATH;
    m_created = false;
    m_runtime = nullptr;
}

void OpenXRInput::Sync(XrTime) {
    if (!m_created || m_runtime == nullptr || !m_runtime->IsSessionFocused()) {
        return;
    }
    XrActiveActionSet active{m_action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    const XrResult result = xrSyncActions(m_runtime->Session(), &sync);
    m_runtime->ObserveResult(result);
    if (XR_FAILED(result)) {
        if (!m_logged_sync_failure) {
            m_logged_sync_failure = true;
            std::ostringstream message;
            message << "xrSyncActions failed (" << result << ')';
            Log(OpenXRLogLevel::Warning, message.str());
        }
        return;
    }
    if (m_joystick == nullptr) {
        return;
    }
    auto* joystick = static_cast<SDL_Joystick*>(m_joystick);

    const auto boolean = [&](XrAction action, uint32_t hand) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = m_hand_paths[hand];
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(m_runtime->Session(), &info, &state)) &&
               state.isActive == XR_TRUE && state.currentState == XR_TRUE;
    };
    const auto scalar = [&](XrAction action, uint32_t hand) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = m_hand_paths[hand];
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_FAILED(xrGetActionStateFloat(m_runtime->Session(), &info, &state)) ||
            state.isActive != XR_TRUE) {
            return 0.0f;
        }
        return state.currentState;
    };
    const auto vector = [&](XrAction action, uint32_t hand) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = m_hand_paths[hand];
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_FAILED(xrGetActionStateVector2f(m_runtime->Session(), &info, &state)) ||
            state.isActive != XR_TRUE) {
            return XrVector2f{0.0f, 0.0f};
        }
        return state.currentState;
    };

    PollInjection();
    XrVector2f left = vector(m_thumbstick, 0);
    const XrVector2f right = vector(m_thumbstick, 1);
    if (Injected("up")) {
        left.y = 1.0f;
    } else if (Injected("down")) {
        left.y = -1.0f;
    } else if (Injected("left")) {
        left.x = -1.0f;
    } else if (Injected("right")) {
        left.x = 1.0f;
    }
    // OpenXR thumbsticks report +Y up; SDL gamepads report +Y down.
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, ToAxis(left.x));
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY, ToAxis(-left.y));
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTX, ToAxis(right.x));
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTY, ToAxis(-right.y));
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, ToTrigger(scalar(m_trigger, 0)));
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, ToTrigger(scalar(m_trigger, 1)));

    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH,
                                 boolean(m_button_primary, 1) || Injected("a"));
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_EAST,
                                 boolean(m_button_secondary, 1) || Injected("b"));
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_WEST,
                                 boolean(m_button_primary, 0) || Injected("x"));
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_NORTH,
                                 boolean(m_button_secondary, 0) || Injected("y"));
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_START, boolean(m_menu, 0) || Injected("start"));
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, boolean(m_thumbstick_click, 0));
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, boolean(m_thumbstick_click, 1));
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, scalar(m_squeeze, 0) > 0.5f);
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, scalar(m_squeeze, 1) > 0.5f);
}

void OpenXRInput::ApplyHaptic(uint32_t hand, float amplitude, XrDuration duration) {
    if (!m_created || m_runtime == nullptr || hand >= kHandCount || m_haptic == XR_NULL_HANDLE) {
        return;
    }
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    vibration.duration = duration;
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = m_haptic;
    info.subactionPath = m_hand_paths[hand];
    if (vibration.amplitude <= 0.0f) {
        xrStopHapticFeedback(m_runtime->Session(), &info);
        return;
    }
    xrApplyHapticFeedback(m_runtime->Session(), &info,
                          reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

bool OpenXRInput::Check(XrResult result, const char* operation) {
    if (m_runtime != nullptr) {
        m_runtime->ObserveResult(result);
    }
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    std::ostringstream message;
    message << operation << " failed (" << result << ')';
    m_last_error = message.str();
    Log(OpenXRLogLevel::Error, m_last_error);
    return false;
}

void OpenXRInput::Log(OpenXRLogLevel level, const std::string& message) const noexcept {
    if (!m_logger) {
        return;
    }
    try {
        m_logger(level, message);
    } catch (...) {
    }
}

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR)
