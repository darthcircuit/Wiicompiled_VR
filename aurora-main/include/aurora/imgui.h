#ifndef AURORA_IMGUI_H
#define AURORA_IMGUI_H

#include <imgui.h>

#ifdef __cplusplus
#include <cstdint>

extern "C" {
#else
#include "stdint.h"
#endif

ImTextureID aurora_imgui_add_texture(uint32_t width, uint32_t height, const void* rgba8);

// A panel shown only in the headset, drawn by the host with a Dear ImGui context of its own (for
// instance a settings menu reachable without the desktop window). It is laid over whatever the eyes
// carry: centred on the virtual screen, `widthFraction` of that screen's width, its height following
// the draw data's aspect. On a menu frame the screen is the quad the eye image is shown on; in an
// immersive race it is the 2D layer's screen (aurora_set_stereo_hud_screen's width and distance,
// whether or not the 2D layer is placed on it).
//
// The draw data belongs to the host's context and is rendered with Aurora's ImGui backend, so its
// texture ids must be views that backend can bind (aurora_imgui_add_texture). Call this from the
// producer before the frame is sealed, and leave the draw data untouched until the frame worker is
// done with that frame (aurora_wait_for_frame_worker). Null hides the panel.
void aurora_imgui_set_stereo_overlay(ImDrawData* drawData, float widthFraction);

#ifdef __cplusplus
}
#endif

#endif
