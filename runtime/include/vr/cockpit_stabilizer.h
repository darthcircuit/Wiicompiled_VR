// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from heurazy's mario-kart-wii-VR-port (GPL-3.0-or-later).
#pragma once
#include "vr/mkw_vr_first_person.h"
#include <algorithm>
#include <cmath>

namespace mkw::vr {
// Simulation position and driving direction, never the animated vehicle matrix.
// Follow the simulation position exactly; stabilize only impact orientation.
class CockpitStabilizer {
public:
    Mtx34 Update(const Mtx34& simulation, bool damaged, float dt) {
        const float yaw = std::atan2(simulation[2], simulation[10]);
        const float dx = simulation[3]-position_[0], dy = simulation[7]-position_[1], dz = simulation[11]-position_[2];
        if (!valid_ || dx*dx+dy*dy+dz*dz > 1500.0f*1500.0f) {
            position_ = {simulation[3],simulation[7],simulation[11]};
            yaw_ = yaw; valid_ = true; recovering_ = false;
        }
        if (damaged) recovering_ = true;
        else if (recovering_) {
            const float alpha = 1.0f-std::exp(-8.0f*std::clamp(dt,0.0f,0.05f));
            const float delta = std::remainder(yaw-yaw_,6.283185307f);
            yaw_ += delta*alpha;
            if (std::abs(delta)<0.002f) recovering_=false;
        } else {
            position_={simulation[3],simulation[7],simulation[11]}; yaw_=yaw;
        }
        // Freezing/blending translation left the seat behind after collisions.
        // Dynamics excludes visual shake; retain its exact kart attachment.
        position_={simulation[3],simulation[7],simulation[11]};
        const float c=std::cos(yaw_),s=std::sin(yaw_);
        return {c,0,s,position_[0], 0,1,0,position_[1], -s,0,c,position_[2]};
    }
private:
    std::array<float,3> position_{};
    float yaw_=0;
    bool valid_=false,recovering_=false;
};
}
