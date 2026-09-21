// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/openxr_driving.h"

#include <mutex>

namespace mkw::vr {
// Named rather than anonymous: runtime sources are unity-built in groups.
namespace driving_bridge {

struct Published {
    std::mutex mutex;
    DrivingSnapshot snapshot{};
};

Published& Get() {
    static Published published;
    return published;
}

} // namespace driving_bridge

void OpenXRPublishDriving(const DrivingSnapshot& snapshot) noexcept {
    auto& published = driving_bridge::Get();
    std::lock_guard lock(published.mutex);
    published.snapshot = snapshot;
}

DrivingSnapshot OpenXRReadDriving() noexcept {
    auto& published = driving_bridge::Get();
    std::lock_guard lock(published.mutex);
    return published.snapshot;
}

} // namespace mkw::vr
