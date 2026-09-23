// SPDX-License-Identifier: GPL-3.0-or-later
// USB wheel calibration and its GameCube pad mapping (physical_wheel.h). The
// calibration and race-mapping cases are heurazy's (mario-kart-wii-VR-port).

#include "physical_wheel.h"

#include <iostream>

using namespace physical_wheel;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

void TestCalibration() {
    Check(Steering(-32768, -32768, 100, 32767, .02f) == -1, "left endpoint");
    Check(Steering(32767, -32768, 100, 32767, .02f) == 1, "right endpoint");
    Check(Steering(200, -32768, 100, 32767, .02f) == 0, "calibrated center deadzone");
    Check(Steering(-32768, 32767, 0, -32768, 0) == 1, "inverted steering");
    Check(Steering(100, 0, 0, 100, 0) == 0, "reject missing calibration");
    // A 900-degree wheel calibrated at a quarter turn each way: full lock there,
    // and clamped beyond it.
    Check(Steering(-8192, -8192, 0, 8192, 0) == -1 && Steering(-30000, -8192, 0, 8192, 0) == -1,
          "full lock at the recorded turn, clamped past it");
    Check(Pedal(-32768, 32767, -32768) == 1, "reversed pedals (released at +32767, as Logitech pedals report)");
    Check(Pedal(32767, 32767, -32768) == 0, "released reversed pedal");
    Check(Pedal(-16000, 0, 32767) == 0 && Pedal(16000, 0, -32768) == 0, "combined pedals isolate opposite direction");
    Check(Pedal(30000, 0, 10000) == 1, "pedal clamps beyond calibration");
}

void TestRace() {
    auto p = Map(.5f, 1, 0, true, true);
    Check(p.stickX == 50 && (p.button & PAD_BUTTON_A) && (p.button & PAD_TRIGGER_R) && (p.button & PAD_TRIGGER_L),
          "accelerate drift and item together");
    p = Map(-1, 1, 1, true, true);
    Check(p.stickX == -100 && (p.button & PAD_BUTTON_B) && !(p.button & (PAD_BUTTON_A | PAD_TRIGGER_R)) &&
              p.analogA == 0 && p.triggerR == 0 && (p.button & PAD_TRIGGER_L),
          "brake overrides throttle and drift while preserving item");

    Controls c;
    c.steering = .25f;
    c.throttle = 1;
    c.hat = 0x08; // left
    c.trick = true;
    c.pause = true;
    p = RacePad(c);
    Check(p.stickX == 25 && (p.button & PAD_BUTTON_A) && (p.button & PAD_BUTTON_LEFT) && (p.button & PAD_BUTTON_UP) &&
              (p.button & PAD_BUTTON_START),
          "race: steering, accelerator, D-pad trick and pause");
    c = {};
    c.brake = 1;
    c.confirm = true;
    p = RacePad(c);
    Check((p.button & PAD_BUTTON_B) && !(p.button & PAD_BUTTON_A), "the brake pedal beats the confirm button");
}

void TestMenu() {
    Controls c;
    c.steering = 1;
    c.throttle = 1;
    c.brake = 1;
    c.drift = c.item = true;
    PADStatus p = MenuPad(c);
    Check(p.button == 0 && p.stickX == 0, "menus ignore the wheel, pedals and paddles");
    c.confirm = true;
    c.back = true;
    c.pause = true;
    c.hat = 0x01 | 0x02; // up-right
    p = MenuPad(c);
    Check((p.button & PAD_BUTTON_A) && (p.button & PAD_BUTTON_B) && (p.button & PAD_BUTTON_START) &&
              (p.button & PAD_BUTTON_UP) && (p.button & PAD_BUTTON_RIGHT) && !(p.button & PAD_BUTTON_DOWN),
          "menus: confirm, back, pause and the D-pad");
    Check(HatButtons(0x04 | 0x08) == (PAD_BUTTON_DOWN | PAD_BUTTON_LEFT), "hat down-left");
}

void TestMerge() {
    // Race: the wheel owns port 0 but keeps the other source's pause and item aim.
    PADStatus port{};
    port.err = PAD_ERR_NO_CONTROLLER;
    port.button = PAD_BUTTON_START | PAD_BUTTON_A;
    port.stickX = -80;
    port.stickY = 70;
    PadFilter filter;
    Controls c;
    c.steering = .5f;
    Merge(port, RacePad(c), true, false, filter);
    Check(port.err == PAD_ERR_NONE && port.stickX == 50 && port.stickY == 70, "race: wheel steers, stick still aims");
    Check((port.button & PAD_BUTTON_START) && !(port.button & PAD_BUTTON_A), "race: other source keeps only pause");

    // Menus: the wheel's buttons join the other source's navigation.
    port = {};
    port.err = PAD_ERR_NONE;
    port.stickY = 60;
    c = {};
    c.confirm = true;
    PadFilter menus;
    Merge(port, MenuPad(c), false, false, menus);
    Check((port.button & PAD_BUTTON_A) && port.analogA == 255 && port.stickY == 60, "menus: wheel adds confirm");

    // A button held while blocked stays out until it is released.
    PadFilter held;
    port = {};
    Merge(port, MenuPad(c), false, true, held);
    Check(port.button == 0, "blocked: nothing reaches the game");
    port = {};
    Merge(port, MenuPad(c), false, false, held);
    Check(!(port.button & PAD_BUTTON_A), "still held after unblocking: suppressed");
    port = {};
    Merge(port, MenuPad(Controls{}), false, false, held);
    port = {};
    Merge(port, MenuPad(c), false, false, held);
    Check((port.button & PAD_BUTTON_A) != 0, "a fresh press after release goes through");
}

} // namespace

int main() {
    TestCalibration();
    TestRace();
    TestMenu();
    TestMerge();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "physical wheel tests passed\n";
    return 0;
}
