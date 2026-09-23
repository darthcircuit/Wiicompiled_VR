// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The VR cockpit's native steering wheel, on the GX side: the game thread
// hands Aurora a rotated copy of one of the player's vehicle position arrays,
// and Aurora substitutes it for draws that bind that array with the player's
// own model-view matrix (aurora_set_native_wheel_vertices). The copies must
// reach Aurora in order with the frame's draws, so these post to the GX thread
// when it runs and call through directly when it does not.

#include <cstdint>

namespace GxNativeWheel {

// Game thread. Drops every replacement, after the draws posted before it.
void PostClear();

// Game thread. `guestArray` is the guest address of the original array, as the
// vehicle's MDL0 holds it; `bytes` (size bytes, big-endian like the original)
// is copied now. The replacement is registered under every host pointer the
// game can bind that array through (the SDK's GXSetArray address and the
// display lists' physical CP address), so it matches whichever reaches Aurora.
// Returns false when the array does not resolve to host memory.
bool PostVertices(uint32_t guestArray, const uint8_t* bytes, uint32_t size, const float modelView[12]);

// Any thread. How many draws the most recent cleared set of replacements was
// substituted into.
uint32_t LastDrawCount();

} // namespace GxNativeWheel
