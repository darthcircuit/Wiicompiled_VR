// SPDX-License-Identifier: GPL-3.0-or-later
#include "vr/mkw_vr_policy.h"

#include <cstring>
#include <iostream>

using namespace mkw::vr;

int main() {
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::cerr << message << '\n';
            ++failures;
        }
    };
    MkwVRPolicyReset();
    MkwVRPolicyConfig config{};
    config.enabled = true;
    MkwVRPolicyConfigure(config);
    MkwVRPolicySetSessionActive(true);
    MkwVRPolicySetAvailableBindings(kMkwVRRequiredImmersiveBindings);
    MkwVRSceneObservation scene{VRSceneMode::Race, 1, 10};
    MkwVRCameraObservation camera{};
    camera.view_from_world = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    camera.guest_camera_address = 0x81000000;
    camera.guest_frame_index = 10;
    camera.valid = true;
    MkwVRPolicyPublishScene(scene);
    MkwVRPolicyPublishRaceCamera(camera);
    uint64_t previous_tag = 0;
    for (uint32_t players : {1u, 2u, 3u, 4u, 2u, 1u}) {
        scene.local_player_count = players;
        MkwVRPolicyPublishScene(scene);
        const auto snapshot = MkwVRPolicyGetSnapshot();
        check(snapshot.presentation == VRPresentationMode::ImmersiveRace, "1-4 players should be immersive");
        check(snapshot.content_tag != previous_tag, "layout changes must invalidate retained packets");
        previous_tag = snapshot.content_tag;
        MkwVRPolicyPublishScene(scene);
        MkwVRPolicyPublishRaceCamera(camera);
        check(MkwVRPolicyGetSnapshot().content_tag == previous_tag, "steady frames must keep the same tag");
    }
    camera.guest_frame_index = 11;
    MkwVRPolicyPublishRaceCamera(camera);
    check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::ImmersiveRace,
          "adjacent camera publication is coherent");
    camera.guest_frame_index = 12;
    MkwVRPolicyPublishRaceCamera(camera);
    check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::VirtualScreen, "stale camera fails safe");
    camera.guest_frame_index = 10;
    MkwVRPolicyPublishRaceCamera(camera);
    for (uint32_t players : {0u, 5u, UINT32_MAX}) {
        scene.local_player_count = players;
        MkwVRPolicyPublishScene(scene);
        check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::VirtualScreen, "invalid counts fail safe");
    }
    scene.local_player_count = 4;
    MkwVRPolicyPublishScene(scene);
    const uint32_t nan_bits = 0x7fc00000;
    std::memcpy(&camera.view_from_world[0], &nan_bits, sizeof(nan_bits));
    MkwVRPolicyPublishRaceCamera(camera);
    check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::VirtualScreen, "NaN camera fails under fast math");
    camera.view_from_world[0] = 1;
    MkwVRPolicyPublishRaceCamera(camera);
    MkwVRPolicySetAvailableBindings(MkwVRBindingRaceCamera);
    check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::VirtualScreen, "partial hooks fail safe");
    MkwVRPolicySetAvailableBindings(kMkwVRRequiredImmersiveBindings);
    scene.mode = VRSceneMode::FrontEnd;
    MkwVRPolicyPublishScene(scene);
    check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::VirtualScreen, "menus use virtual screen");
    scene.mode = VRSceneMode::Race;
    MkwVRPolicyPublishScene(scene);
    check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::VirtualScreen, "race entry needs fresh camera");
    MkwVRPolicyPublishRaceCamera(camera);
    check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::ImmersiveRace, "race resumes with fresh camera");
    MkwVRPolicySetSessionActive(false);
    check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::Desktop, "inactive session uses desktop");
    return failures == 0 ? 0 : 1;
}
