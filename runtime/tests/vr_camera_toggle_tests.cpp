// SPDX-License-Identifier: GPL-3.0-or-later
//
// The right-thumbstick click that toggles the first-person camera
// (vr/camera_toggle.h).

#include "vr/camera_toggle.h"

#include <iostream>

namespace {

using mkw::vr::ClickToggle;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

} // namespace

int main() {
    ClickToggle click;
    Check(!click.Update(true, false, false), "nothing on press");
    Check(!click.Update(true, false, false), "nothing while held");
    Check(click.Update(false, false, false), "a clean click fires on release");
    Check(!click.Update(false, false, false), "fires once");

    // Both thumbsticks (the gamepad settings-panel chord), in either order.
    click.Update(true, false, false);
    click.Update(true, true, false);
    Check(!click.Update(false, true, false), "right then left: the chord, no toggle");
    click.Update(false, true, false);
    click.Update(true, true, false);
    Check(!click.Update(false, false, false), "left then right: the chord, no toggle");

    // The panel owned the controllers during the press.
    click.Update(true, false, false);
    click.Update(true, false, true);
    Check(!click.Update(false, false, false), "blocked midway: no toggle");
    click.Update(true, false, false);
    Check(!click.Update(false, false, true), "blocked at release: no toggle");

    // Recovers for the next clean click.
    click.Update(true, false, false);
    Check(click.Update(false, false, false), "a later clean click fires again");

    click.Update(true, false, false);
    click.Reset();
    Check(!click.Update(false, false, false), "reset forgets a held press");

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "vr camera toggle tests passed\n";
    return 0;
}
