// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from heurazy's mario-kart-wii-VR-port (GPL-3.0-or-later).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>

namespace mkw::vr {
// Metres in a fixed seated frame: +X right, +Y up, -Z forward.
struct WheelHand { float x=0, y=0, z=0, squeeze=0; bool tracked=false; };
struct WheelGeometry {
    std::array<float,3> center{}, right{1,0,0}, up{0,1,0}, normal{0,0,1};
    float radius=0.18f;
    bool valid=false;
    WheelHand ToWheel(WheelHand hand) const {
        const std::array<float,3> p{hand.x-center[0],hand.y-center[1],hand.z-center[2]};
        const auto dot=[&](const auto& axis) { return p[0]*axis[0]+p[1]*axis[1]+p[2]*axis[2]; };
        hand.x=dot(right); hand.y=dot(up)-0.30f; hand.z=dot(normal)-0.42f;
        return hand;
    }
};
struct WheelState {
    float angle=0, steering=0, visualAngle=0;
    std::array<bool, 2> held{};
};
class WheelReferenceLatch {
    WheelGeometry last_{};
    uint64_t identity_=0;
    float missing_=0;
    bool bike_=false;
public:
    bool Resolve(WheelGeometry& geometry, bool enabled, bool available, bool held,
                 bool bike, uint64_t identity, float dt) {
        if(!enabled || identity_!=identity || bike_!=bike) { last_={};missing_=0; }
        identity_=identity;bike_=bike;
        if(!enabled) return false;
        if(available && geometry.valid) { last_=geometry;missing_=0;return true; }
        missing_+=std::clamp(dt,0.0f,0.05f);
        if(held && last_.valid && missing_<0.20f) { geometry=last_;return true; }
        last_={};return false;
    }
};
struct WheelTuning {
    float kartDegrees=90, bikeDegrees=45, grabDistance=0.35f, grabAssist=1, response=1, trackingGrace=0.20f;
    bool haptics=true;
};
class SteeringWheel {
public:
    static constexpr float Radius=0.18f, Height=-0.30f, Depth=-0.42f;
    WheelState Update(const std::array<WheelHand, 2>& hands, bool active, float dt, float radius=Radius, bool handlebars=false,
                      WheelTuning tuning={}) {
        const auto finite=[](float value) { uint32_t bits; std::memcpy(&bits,&value,sizeof(bits)); return (bits&0x7f800000u)!=0x7f800000u; };
        if (!finite(dt)) dt=0;
        if (!finite(radius) || radius<0.04f || radius>1.0f) { active=false; radius=Radius; }
        dt=std::clamp(dt, 0.0f, 0.05f);
        const bool previouslyHeld=state_.held[0]||state_.held[1];
        float deltaSum=0,weightSum=0;
        std::array<bool,2> moving{};
        std::array<bool,2> validHands{};
        std::array<float,2> delta{};
        const float degrees=handlebars?tuning.bikeDegrees:tuning.kartDegrees;
        const float maxAngle=(finite(degrees)?std::clamp(degrees,20.0f,180.0f):(handlebars?45.0f:90.0f))*0.01745329252f;
        const float assist=finite(tuning.grabAssist)?std::clamp(tuning.grabAssist,0.7f,2.0f):1.0f;
        const float reach=finite(tuning.grabDistance)?std::clamp(tuning.grabDistance,0.15f,0.8f):0.35f;
        const float grace=finite(tuning.trackingGrace)?std::clamp(tuning.trackingGrace,0.05f,0.5f):0.2f;
        for (int h=0; h<2; ++h) {
            const auto& p=hands[h];
            const bool valid=p.tracked&&finite(p.x)&&finite(p.y)&&finite(p.z)&&finite(p.squeeze);
            validHands[h]=valid;
            const bool down=finite(p.squeeze)&&p.squeeze > (pressed_[h] ? 0.15f : 0.55f);
            if(!valid) {
                lost_[h]+=dt;
                if(!p.tracked && down && active && state_.held[h] && lost_[h]<grace) {
                    center_[h]=true;
                } else { state_.held[h]=false; pressed_[h]=down; }
                continue;
            }
            lost_[h]=0;
            const float y=p.y-Height, z=p.z-Depth;
            const float radial=std::hypot(p.x,y);
            const float gripX=radius*std::cos(state_.angle),gripY=-radius*std::sin(state_.angle);
            const bool near_rim=std::abs(z)<reach && (handlebars
                ? std::min(std::hypot(p.x-gripX,y-gripY),std::hypot(p.x+gripX,y+gripY))<std::max(0.22f,radius*0.55f)*assist
                : radial<std::max(radius+0.16f,0.32f)*assist);
            const float angle=-std::atan2(y,p.x);
            // Arcade latch: distance only gates acquisition. Once grabbed,
            // large gestures and vehicle animation cannot release ownership.
            if (!active || !down)
                state_.held[h]=false;
            else if (!pressed_[h] && near_rim) {
                state_.held[h]=true;
                last_[h]=angle;
                center_[h]=false;
            }
            if (state_.held[h]) {
                // Angle is undefined at the hub. Keep ownership and the last
                // steering value, then rebase on exit to avoid a 180-degree jump.
                if (radial<(center_[h]?0.065f:0.045f)) center_[h]=true;
                else {
                    moving[h]=!center_[h];
                    if (moving[h]) delta[h]=std::remainder(angle-last_[h], 6.283185307f);
                    last_[h]=angle;
                    center_[h]=false;
                }
                if(moving[h]) {
                    const float weight=std::clamp(radial/0.18f,0.15f,1.0f);
                    deltaSum+=delta[h]*weight; weightSum+=weight;
                }
            }
            pressed_[h]=down;
        }
        // Two hands define one rigid control. Their relative angle ignores
        // shared translations, so leaning or moving both arms does not steer.
        const float spanX=hands[1].x-hands[0].x,spanY=hands[1].y-hands[0].y;
        // Pair orientation is defined by the span, even when one hand is near
        // the original hub after a common translation of both arms.
        const bool pair=state_.held[0]&&state_.held[1]&&validHands[0]&&validHands[1]&&
            std::hypot(spanX,spanY)>(pairValid_?0.10f:0.14f);
        const float pairAngle=pair ? -std::atan2(spanY,spanX):0;
        float change=weightSum>0 ? deltaSum/weightSum:0;
        // With both hands held, do not switch to angles about the hub when
        // their span collapses: that changes the reference frame and creates
        // false turns as the hands approach/cross each other. Hold the angle
        // through that singularity and establish a fresh pair baseline on exit.
        if(state_.held[0] && state_.held[1])
            change=pair && pairValid_ ? std::remainder(pairAngle-lastPair_,6.283185307f):0;
        pairValid_=pair; lastPair_=pairAngle;
        // Rebase a discontinuous tracking pose without sending a full-lock
        // impulse. Normal fast arcade steering remains inside this envelope.
        if(std::abs(change)>0.25f+8.0f*dt) change=0;
        const bool held=state_.held[0]||state_.held[1];
        if(held && !previouslyHeld) target_=state_.angle;
        // A single accumulated target avoids jumps when a second hand joins,
        // leaves, passes through the hub or temporarily loses tracking.
        // Keep physical overtravel. Clamping this accumulator discards motion
        // past full lock, so retracing the gesture no longer returns to centre.
        // Only the game's steering command is saturated, never the hand angle.
        target_=held ? target_+change:0;
        const float error=target_-state_.angle;
        // Quiet near a steady heading, responsive during deliberate turns.
        const float tuningResponse=finite(tuning.response)?std::clamp(tuning.response,0.5f,2.0f):1;
        const float response=held ? std::clamp((18.0f+80.0f*std::abs(error))*tuningResponse,9.0f,90.0f):12.0f;
        state_.angle += error*(1-std::exp(-dt*response));
        state_.steering=active ? std::clamp(state_.angle/maxAngle,-1.0f,1.0f) : 0;
        if (!active) { state_.angle=0;target_=0;pairValid_=false; }
        // Share the validated physical rotation with both renderer paths.
        // Handlebars keep their limited travel; a kart wheel can turn freely.
        state_.visualAngle=handlebars ? std::clamp(state_.angle,-maxAngle,maxAngle)
            : std::remainder(state_.angle,6.283185307f);
        return state_;
    }
private:
    WheelState state_{};
    std::array<bool,2> pressed_{};
    std::array<bool,2> center_{};
    std::array<float,2> last_{};
    std::array<float,2> lost_{};
    float target_=0,lastPair_=0;
    bool pairValid_=false;
};
} // namespace mkw::vr
