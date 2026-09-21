// SPDX-License-Identifier: GPL-3.0-or-later
//
// The first-person cockpit's pure geometry, tested without a guest: where the
// seated eye comes from, how the vehicle's wheel and handlebar land in the
// seated frame, the level seat through spins, and which of the vehicle's own
// vertices the wheel animation turns. The math is ported from heurazy's
// mario-kart-wii-VR-port.

#include "vr/cockpit_stabilizer.h"
#include "vr/mkw_vr_first_person.h"
#include "vr/native_wheel_mesh.h"

#include <cmath>
#include <iostream>
#include <vector>

namespace {

using namespace mkw::vr;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

void CheckNear(float actual, float expected, const char* what, float tolerance = 1.0e-3f) {
    if (!(std::fabs(actual - expected) <= tolerance)) {
        ++g_failures;
        std::cerr << "FAILED: " << what << " (expected " << expected << ", got " << actual << ")\n";
    }
}

Mtx34 Translation(float x, float y, float z) {
    Mtx34 m = kIdentityMtx34;
    m[3] = x;
    m[7] = y;
    m[11] = z;
    return m;
}

Mtx34 YawAt(float yaw, float x, float y, float z) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {c, 0, s, x, 0, 1, 0, y, -s, 0, c, z};
}

void TestMatrixHelpers() {
    const Mtx34 a = YawAt(0.7f, 1.0f, 2.0f, 3.0f);
    Mtx34 inverse{};
    Check(InvertMtx(a, inverse), "a rigid transform inverts");
    const Mtx34 identity = ComposeMtx(a, inverse);
    for (int i = 0; i < 12; ++i) {
        CheckNear(identity[i], kIdentityMtx34[i], "a * inverse(a) is identity", 1e-5f);
    }
    Mtx34 singular{};
    Check(!InvertMtx(singular, inverse), "a singular matrix does not invert");
    const Mtx34 scaled = ScaleModelBasis(kIdentityMtx34, {2.0f, 3.0f, 4.0f});
    CheckNear(scaled[0], 2.0f, "basis X scaled");
    CheckNear(scaled[5], 3.0f, "basis Y scaled");
    CheckNear(scaled[10], 4.0f, "basis Z scaled");
    CheckNear(scaled[3], 0.0f, "translation untouched");
}

void TestSeatHelpers() {
    CheckNear(CharacterCockpitScale(80.0f), 1.0f, "short characters keep the base scale");
    CheckNear(CharacterCockpitScale(150.0f), 1.5f, "tall characters grow the scale with eye height");
    CheckNear(CharacterCockpitScale(1000.0f), 2.5f, "the scale is capped");
    CheckNear(CharacterCockpitScale(std::nanf("")), 1.0f, "a bad eye height keeps the base scale");
    CheckNear(ValidPlayerScale(2.0f), 2.0f, "mega mushroom scale kept");
    CheckNear(ValidPlayerScale(0.0f), 1.0f, "an implausible scale is ignored");
    Check(NeutralPlayerScale({1.0f, 1.0f, 1.0f}), "unit scale is neutral");
    Check(!NeutralPlayerScale({0.5f, 0.5f, 0.5f}), "lightning scale is not neutral");
    // 100 units per metre, controls 60 units ahead: the eye stays at least 0.45 m behind.
    CheckNear(EyeBehindControls(50.0f, 60.0f, 100.0f, 0.0f), 60.0f - 45.0f, "eye pulled behind the wheel");
    CheckNear(EyeBehindControls(50.0f, 60.0f, 100.0f, 50.0f), 60.0f - 55.0f, "a wider wheel keeps more clearance");
    CheckNear(EyeBehindControls(-10.0f, 60.0f, 100.0f, 18.0f), -10.0f, "an eye already behind stays put");
}

void TestDriverEye() {
    std::array<float, 3> eye{};
    // Face bone at (0, 80, 10) in the character, placed 5 units up in the vehicle.
    const Mtx34 face = Translation(0.0f, 80.0f, 10.0f);
    const Mtx34 placement = Translation(0.0f, 5.0f, 0.0f);
    Check(ComputeDriverEyeFromBounds(face, placement, {-2, 8, 0}, {2, 12, 4}, eye), "eye from bounds");
    CheckNear(eye[1], 95.0f, "bounds centre through bind and placement (up)");
    CheckNear(eye[2], 12.0f, "bounds centre through bind and placement (forward)");
    Check(!ComputeDriverEyeFromBounds(face, placement, {2, 8, 0}, {-2, 12, 4}, eye), "inverted bounds rejected");
    Check(!ComputeDriverEyeFromBounds(Translation(0, -50, 0), placement, {0, 0, 0}, {1, 1, 1}, eye),
          "an eye below the seat is rejected");

    // The same eye through the animated world matrices: the body's own motion
    // must not leak into the seat.
    const Mtx34 body = YawAt(1.2f, 500.0f, 20.0f, -300.0f);
    const Mtx34 faceWorld = ComposeMtx(body, Translation(0.0f, 90.0f, 15.0f));
    Check(ComputeSeatedEye(faceWorld, body, {0, 0, 0}, eye), "seated eye from world matrices");
    CheckNear(eye[0], 0.0f, "seated eye right", 1e-3f);
    CheckNear(eye[1], 90.0f, "seated eye up", 1e-3f);
    CheckNear(eye[2], 15.0f, "seated eye forward", 1e-3f);

    SeatedEyeReference reference;
    for (int i = 0; i < 7; ++i) {
        reference.Observe({0, 90, 15}, true, true);
    }
    Check(!reference.valid, "seven samples are not enough");
    reference.Observe({0, 90, 15}, true, true);
    Check(reference.valid, "eight stable samples calibrate the seat");
    reference.Observe({0, 200, 15}, true, true);
    CheckNear(reference.value[1], 90.0f, "a calibrated seat is frozen");
    SeatedEyeReference interrupted;
    for (int i = 0; i < 5; ++i) {
        interrupted.Observe({0, 90, 15}, true, true);
    }
    interrupted.Observe({0, 90, 15}, false, true);
    for (int i = 0; i < 5; ++i) {
        interrupted.Observe({0, 90, 15}, true, true);
    }
    Check(!interrupted.valid, "an unsafe sample restarts calibration");
}

void TestWheelGeometry() {
    // Grip targets 20 units either side of a wheel 60 units ahead and 50 up,
    // 100 units per metre, seat frame = the vehicle frame turned to face -Z
    // (vehicle +Z forward, +X to the driver's left).
    const Mtx34 seatFromBody{-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0};
    const WheelGeometry wheel = ComputeNativeWheelGeometry(seatFromBody, {20, 50, 60}, {-20, 50, 60}, 100.0f);
    Check(wheel.valid, "wheel geometry from the grip targets");
    CheckNear(wheel.radius, 0.2f, "radius is half the grip span");
    CheckNear(wheel.center[1], 0.5f, "centre height in metres");
    CheckNear(wheel.center[2], -0.6f, "centre ahead in metres");
    CheckNear(wheel.right[0], 1.0f, "wheel right is the seated right");
    CheckNear(wheel.up[1], 1.0f, "wheel up is the vehicle's up");
    const auto swapped = ComputeNativeWheelGeometry(seatFromBody, {-20, 50, 60}, {20, 50, 60}, 100.0f);
    CheckNear(swapped.right[0], wheel.right[0], "grip order does not flip the wheel");
    Check(!ComputeNativeWheelGeometry(seatFromBody, {1, 50, 60}, {-1, 50, 60}, 100.0f).valid,
          "a wheel narrower than 4 cm is rejected");

    // A hand on the right of the rim maps onto the wheel's rim at angle zero.
    WheelHand hand{wheel.center[0] + 0.2f, wheel.center[1], wheel.center[2], 1.0f, true};
    const WheelHand local = wheel.ToWheel(hand);
    CheckNear(local.x, 0.2f, "right rim point is +radius along the wheel");
    CheckNear(local.y, SteeringWheel::Height, "wheel-local height matches the synthetic wheel");
    CheckNear(local.z, SteeringWheel::Depth, "wheel-local depth matches the synthetic wheel");

    // Handlebar: position from the (steered) handle, axes from the neutral body.
    const float steer = 0.4f, c = std::cos(steer), s = std::sin(steer);
    const Mtx34 steeredHandle{-c, 0, -s, 0, 0, 1, 0, 0, s, 0, -c, 0};
    const auto bar = ComputeNativeHandlebarGeometry(steeredHandle, seatFromBody, {20, 50, 60}, {-20, 50, 60}, 100.0f);
    Check(bar.valid, "handlebar geometry");
    CheckNear(bar.right[0], 1.0f, "handlebar axes ignore the steering already applied");
}

void TestStabilizer() {
    CockpitStabilizer stabilizer;
    const Mtx34 start = YawAt(0.5f, 10, 0, 20);
    auto seat = stabilizer.Update(start, false, 1.0f / 60.0f);
    CheckNear(seat[3], 10.0f, "position followed");
    CheckNear(std::atan2(seat[2], seat[10]), 0.5f, "heading followed");
    // Damage spins the chassis; the seat holds its heading but keeps position.
    seat = stabilizer.Update(YawAt(2.5f, 12, 0, 21), true, 1.0f / 60.0f);
    CheckNear(seat[3], 12.0f, "position exact while damaged");
    CheckNear(std::atan2(seat[2], seat[10]), 0.5f, "heading held while damaged");
    // Recovery eases back onto the real heading.
    for (int i = 0; i < 120; ++i) {
        seat = stabilizer.Update(YawAt(0.8f, 12, 0, 21), false, 1.0f / 60.0f);
    }
    CheckNear(std::atan2(seat[2], seat[10]), 0.8f, "heading recovered after damage", 5e-3f);
    CheckNear(seat[5], 1.0f, "the seat is always level");
}

void TestNativeWheelVertices() {
    // A 64-point disc of radius 20 in the vehicle's X/Y plane at z = 60, centred
    // at y = 50, plus two far vertices (the chassis) that must never move.
    std::vector<detail::Vec3> points;
    for (int i = 0; i < 64; ++i) {
        const float a = float(i) * 6.2831853f / 64.0f;
        points.push_back({20.0f * std::cos(a), 50.0f + 20.0f * std::sin(a), 60.0f});
    }
    points.push_back({100.0f, 0.0f, 0.0f});
    points.push_back({0.0f, 50.0f, 200.0f});
    const auto original = points;
    const unsigned changed = RotateNativeWheelVertices(points, {0, 50, 60}, 20.0f, 0.5f);
    Check(changed == 64, "every disc vertex turns");
    CheckNear(points[64].x, original[64].x, "chassis vertex untouched");
    CheckNear(points[65].z, original[65].z, "vertex off the disc plane untouched");
    // Rotation keeps each disc point on the rim.
    for (int i = 0; i < 64; ++i) {
        CheckNear(std::hypot(points[i].x, points[i].y - 50.0f), 20.0f, "disc vertex stays on the rim", 1e-2f);
    }
    auto sparse = std::vector<detail::Vec3>(points.begin(), points.begin() + 4);
    Check(RotateNativeWheelVertices(sparse, {0, 50, 60}, 20.0f, 0.5f) == 0, "too few candidates leaves the mesh");
    Check(RotateNativeWheelVertices(points, {0, 50, 60}, 2.0f, 0.5f) == 0, "an implausible radius leaves the mesh");
}

} // namespace

int main() {
    TestMatrixHelpers();
    TestSeatHelpers();
    TestDriverEye();
    TestWheelGeometry();
    TestStabilizer();
    TestNativeWheelVertices();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "vr cockpit tests passed\n";
    return 0;
}
