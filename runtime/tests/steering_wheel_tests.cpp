// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from heurazy's mario-kart-wii-VR-port (GPL-3.0-or-later): grab, release,
// one- and two-hand steering, tracking loss and overtravel of the SteeringWheel.
#include "vr/steering_wheel.h"
#include <iostream>
#include <limits>
using namespace mkw::vr;
int failures=0;
void Check(bool ok,const char* message) { if(!ok) { std::cerr<<message<<'\n';++failures; } }
WheelHand Rim(float angle,float squeeze=1) {
    return {SteeringWheel::Radius*std::cos(angle),SteeringWheel::Height+SteeringWheel::Radius*std::sin(angle),
        SteeringWheel::Depth,squeeze,true};
}
int main() {
    constexpr float pi=3.141592653589793f,dt=1.0f/90;
    SteeringWheel wheel;
    std::array<WheelHand,2> hands{Rim(pi),Rim(0)};
    auto state=wheel.Update(hands,true,dt);
    Check(state.held[0]&&state.held[1]&&state.steering==0,"grab both hands without a steering jump");
    for(int i=1;i<=90;++i) {
        hands={Rim(pi-i*pi/180),Rim(-i*pi/180)};
        state=wheel.Update(hands,true,dt);
    }
    for(int i=0;i<30;++i) state=wheel.Update(hands,true,dt);
    Check(state.steering>0.99f,"clockwise quarter turn produces full right steering");
    hands[1].squeeze=0;
    state=wheel.Update(hands,true,dt);
    Check(state.held[0]&&!state.held[1]&&state.steering>0.98f,"one hand can release without losing the other grip");
    hands[0].tracked=false;
    state=wheel.Update(hands,true,dt);
    Check(state.held[0],"brief tracking dropout keeps the grip");
    for(int i=0;i<20;++i) state=wheel.Update(hands,true,dt);
    Check(!state.held[0]&&!state.held[1],"sustained tracking loss releases the wheel");
    hands[0].tracked=true;
    state=wheel.Update(hands,true,dt);
    Check(!state.held[0],"tracking recovery needs a fresh squeeze");
    hands[0].squeeze=0;wheel.Update(hands,true,dt);hands[0].squeeze=1;
    Check(wheel.Update(hands,true,dt).held[0],"release and squeeze allows reacquisition");
    Check(wheel.Update(hands,false,dt).steering==0,"leaving cockpit clears steering");
    wheel={};hands={Rim(pi),Rim(0)};wheel.Update(hands,true,dt);
    hands[0].z=SteeringWheel::Depth-0.70f;hands[1].z=SteeringWheel::Depth+0.70f;
    state=wheel.Update(hands,true,dt);
    Check(state.held[0]&&state.held[1],"arcade gestures allow 70cm forward and backward travel");
    hands[1].x=0;hands[1].y=SteeringWheel::Height;
    for(int i=0;i<30;++i) state=wheel.Update(hands,true,dt);
    Check(state.held[1],"hand at wheel center retains grip");
    const float before=state.angle;
    hands[1]=Rim(pi);
    state=wheel.Update(hands,true,dt);
    Check(state.held[1]&&std::abs(state.angle-before)<0.001f,"crossing center cannot flip steering 180 degrees");
    hands[0].z=SteeringWheel::Depth-0.85f;
    Check(wheel.Update(hands,true,dt).held[0],"large gestures no longer release a squeezed grip");
    hands[0].squeeze=0;
    Check(!wheel.Update(hands,true,dt).held[0],"releasing the grip button still releases immediately");
    wheel={};hands={Rim(pi,0),Rim(0,0)};
    hands[0].z=0;hands[0].squeeze=1;
    Check(!wheel.Update(hands,true,dt).held[0],"grip far from wheel does not grab it");
    hands[0]=Rim(pi);Check(!wheel.Update(hands,true,dt).held[0],"moving an already squeezed hand onto rim cannot grab");
    hands[0].squeeze=0;wheel.Update(hands,true,dt);hands[0].squeeze=1;wheel.Update(hands,true,dt);
    for(int i=1;i<=90;++i) { hands[0]=Rim(pi+i*pi/180);state=wheel.Update(hands,true,dt); }
    for(int i=0;i<30;++i) state=wheel.Update(hands,true,dt);
    Check(state.steering < -0.99f,"counterclockwise wrap through pi produces full left steering");
    hands[0].x=std::numeric_limits<float>::quiet_NaN();
    state=wheel.Update(hands,true,std::numeric_limits<float>::quiet_NaN());
    Check(!state.held[0]&&std::isfinite(state.steering),"invalid tracking/time cannot poison wheel state");
    wheel={};
    hands={WheelHand{-0.45f,SteeringWheel::Height,SteeringWheel::Depth,1,true},
           WheelHand{0.45f,SteeringWheel::Height,SteeringWheel::Depth,1,true}};
    state=wheel.Update(hands,true,dt,0.45f);
    Check(state.held[0]&&state.held[1],"wide native handlebars can be grabbed at their real radius");
    state=wheel.Update(hands,true,dt,std::numeric_limits<float>::quiet_NaN());
    Check(!state.held[0]&&!state.held[1]&&state.steering==0,"invalid native radius releases safely");
    WheelGeometry bar;
    bar.center={0,-0.4f,-0.6f};bar.up={0,0,-1};bar.normal={0,1,0};bar.radius=0.3f;bar.valid=true;
    wheel={};
    for(int i=0;i<=45;++i) {
        const float a=i*pi/180;
        for(int hand=0;hand<2;++hand) {
            const float side=hand?1.0f:-1.0f;
            hands[hand]=bar.ToWheel({side*0.3f*std::cos(a),-0.4f,-0.6f+side*0.3f*std::sin(a),1,true});
        }
        state=wheel.Update(hands,true,dt,0.3f,true);
    }
    for(int i=0;i<30;++i) state=wheel.Update(hands,true,dt,0.3f,true);
    Check(state.held[0]&&state.held[1]&&state.steering>0.99f,"bike: right hand back and left hand forward steers right at 45 degrees");
    hands[0].squeeze=0;
    Check(wheel.Update(hands,true,dt,0.3f,true).held[1],"bike can be steered with one hand");
    wheel={};hands={};
    hands[1]=bar.ToWheel({0,-0.4f,-0.9f,1,true});
    Check(!wheel.Update(hands,true,dt,0.3f,true).held[1],"bike acquisition uses handle ends, not an invisible circular rim");
    wheel={};hands={};
    hands[1]=bar.ToWheel({0.3f,-0.4f,-0.6f,1,true});
    wheel.Update(hands,true,dt,0.3f,true);
    hands[1]=bar.ToWheel({0.3f,-0.1f,-0.6f,1,true});
    state=wheel.Update(hands,true,dt,0.3f,true);
    Check(state.held[1]&&std::abs(state.steering)<0.001f,"bike vertical hand movement does not steer or lose grip");
    wheel={};hands={};
    for(int i=0;i<=45;++i) {
        const float a=-i*pi/180;
        hands[0]=bar.ToWheel({-0.3f*std::cos(a),-0.4f,-0.6f-0.3f*std::sin(a),1,true});
        state=wheel.Update(hands,true,dt,0.3f,true);
    }
    for(int i=0;i<30;++i) state=wheel.Update(hands,true,dt,0.3f,true);
    Check(state.held[0]&&state.steering < -0.99f,"bike: left hand back steers left with one hand");
    wheel={};hands={};
    hands[1]={0.25f,SteeringWheel::Height,SteeringWheel::Depth,1,true};
    Check(wheel.Update(hands,true,dt,0.05f).held[1],"tiny kart wheel has a comfortable acquisition area independent of visual radius");
    // Common arm motion must not be interpreted as rotation, on either plane.
    for(bool bike : {false,true}) {
        wheel={};hands={Rim(pi),Rim(0)};
        wheel.Update(hands,true,dt,0.18f,bike);
        wheel.Update(hands,true,dt,0.18f,bike);
        for(int i=1;i<=90;++i) {
            hands={Rim(pi),Rim(0)};
            for(auto& hand:hands) { hand.x+=0.12f*i/90;hand.y+=0.10f*i/90; }
            state=wheel.Update(hands,true,dt,0.18f,bike);
        }
        Check(std::abs(state.steering)<0.001f,"two-hand translation does not steer kart or bike");
    }
    // Joining at a different hand position must not dilute the existing turn.
    wheel={};hands={WheelHand{},Rim(0)};wheel.Update(hands,true,dt);
    for(int i=1;i<=45;++i) { hands[1]=Rim(-i*pi/180);wheel.Update(hands,true,dt); }
    for(int i=0;i<90;++i) state=wheel.Update(hands,true,dt);
    const float heldTurn=state.steering;
    hands[0]=Rim(pi);
    for(int i=0;i<60;++i) state=wheel.Update(hands,true,dt);
    Check(std::abs(state.steering-heldTurn)<0.001f,"joining second hand preserves the steering target");
    hands[1].squeeze=0;
    for(int i=0;i<60;++i) state=wheel.Update(hands,true,dt);
    Check(std::abs(state.steering-heldTurn)<0.001f,"releasing original hand preserves the steering target");
    hands[0].tracked=false;hands[0].squeeze=0;
    Check(!wheel.Update(hands,true,dt).held[0],"explicit release works even during tracking loss");
    wheel={};hands={WheelHand{},Rim(0)};wheel.Update(hands,true,dt);
    hands[1]=Rim(pi/2);
    state=wheel.Update(hands,true,dt);
    Check(std::abs(state.steering)<0.001f && state.held[1],"tracking teleport does not jerk steering or drop the grip");
    // Identical continuous gestures at different headset rates have the same result.
    float result72=0,result120=0;
    for(int hz : {72,120}) {
        wheel={};hands={WheelHand{},Rim(0)};wheel.Update(hands,true,1.0f/hz);
        for(int i=1;i<=hz;++i) { hands[1]=Rim(-0.7f*i/hz);state=wheel.Update(hands,true,1.0f/hz); }
        if(hz==72) result72=state.steering;else result120=state.steering;
    }
    Check(std::abs(result72-result120)<0.01f,"steering response is stable from 72 to 120 Hz");
    wheel={};hands={WheelHand{},Rim(0)};wheel.Update(hands,true,dt);
    float peakNoise=0;
    for(int i=0;i<180;++i) {
        hands[1]=Rim(i%2?0.004f:-0.004f);
        state=wheel.Update(hands,true,dt);
        peakNoise=std::max(peakNoise,std::abs(state.steering));
    }
    Check(peakNoise<0.001f,"small tracking tremors are damped around straight steering");
    for(int i=1;i<=45;++i) { hands[1]=Rim(-i*pi/180);state=wheel.Update(hands,true,dt); }
    hands[1].squeeze=0;wheel.Update(hands,true,dt);
    hands[1].squeeze=1;state=wheel.Update(hands,true,dt);
    const float caughtAngle=state.angle;
    for(int i=0;i<90;++i) state=wheel.Update(hands,true,dt);
    Check(std::abs(state.angle-caughtAngle)<0.001f,"grabbing during return to center arrests the return immediately");
    WheelReferenceLatch reference;
    WheelGeometry geometry;geometry.valid=true;
    Check(reference.Resolve(geometry,true,true,true,false,1,dt),"valid native reference acquired");
    geometry={};
    Check(reference.Resolve(geometry,true,false,true,false,1,dt)&&geometry.valid,"brief mesh dropout retains held reference");
    for(int i=0;i<30;++i) { geometry={};reference.Resolve(geometry,true,false,true,false,1,dt); }
    Check(!reference.Resolve(geometry,true,false,true,false,1,dt),"missing reference expires");
    geometry.valid=true;reference.Resolve(geometry,true,true,true,false,1,dt);geometry={};
    Check(!reference.Resolve(geometry,true,false,true,false,2,dt),"vehicle change never inherits old controls");
    geometry.valid=true;reference.Resolve(geometry,true,true,true,false,2,dt);
    Check(!reference.Resolve(geometry,false,true,true,false,2,dt),"explicit disable overrides grace period");
    wheel={}; hands={};hands[1]=Rim(0);
    WheelTuning tuning;tuning.kartDegrees=45;
    wheel.Update(hands,true,dt,SteeringWheel::Radius,false,tuning);
    for(int i=1;i<=45;++i) { hands[1]=Rim(-i*pi/180);wheel.Update(hands,true,dt,SteeringWheel::Radius,false,tuning); }
    for(int i=0;i<90;++i) state=wheel.Update(hands,true,dt,SteeringWheel::Radius,false,tuning);
    Check(state.steering>.99f,"custom 45 degree lock is used by input");
    tuning.kartDegrees=std::numeric_limits<float>::quiet_NaN();
    state=wheel.Update(hands,true,dt,SteeringWheel::Radius,false,tuning);
    Check(std::isfinite(state.steering),"invalid tuning cannot poison steering");

    // Retracing motion beyond full lock must return to the original centre,
    // including complete turns and atan2's +/-pi boundary in either direction.
    for(bool bike : {false,true}) for(bool twoHands : {false,true}) for(float sign : {-1.0f,1.0f}) {
        wheel={};
        const auto pose=[&](int degrees) {
            const float angle=sign*degrees*pi/180;
            return std::array<WheelHand,2>{twoHands?Rim(pi-angle):WheelHand{},Rim(-angle)};
        };
        hands=pose(0);wheel.Update(hands,true,dt,0.18f,bike);
        for(int i=1;i<=720;++i) { hands=pose(i);state=wheel.Update(hands,true,dt,0.18f,bike); }
        for(int i=0;i<90;++i) state=wheel.Update(hands,true,dt,0.18f,bike);
        Check(std::abs(state.angle-sign*4*pi)<0.003f,"physical wheel preserves two complete turns beyond full lock");
        Check(sign*state.steering>0.99f,"overtravel saturates game steering without reversing it");
        Check(bike ? std::abs(state.visualAngle-sign*pi/4)<0.003f : std::abs(state.visualAngle)<0.003f,
            "kart visual follows complete turns while bike visual retains limited travel");
        for(int i=719;i>=0;--i) { hands=pose(i);state=wheel.Update(hands,true,dt,0.18f,bike); }
        for(int i=0;i<90;++i) state=wheel.Update(hands,true,dt,0.18f,bike);
        Check(std::abs(state.angle)<0.003f && std::abs(state.steering)<0.003f,"return from overtravel preserves original centre for kart/bike and one/two hands");
    }
    // Bring both hands together away from the hub at full right lock. Once
    // their span is too short to define a rigid control, jitter must not steer.
    wheel={};hands={Rim(pi),Rim(0)};wheel.Update(hands,true,dt);
    for(int i=1;i<=120;++i) { hands={Rim(pi-i*pi/180),Rim(-i*pi/180)};wheel.Update(hands,true,dt); }
    for(int i=0;i<90;++i) state=wheel.Update(hands,true,dt);
    const float lockAngle=state.angle;
    const auto compressed=[&](float radius,float jitter=0) {
        const float a=-120*pi/180+jitter;
        return std::array<WheelHand,2>{
            WheelHand{0.1f-radius*std::cos(a),SteeringWheel::Height+0.12f-radius*std::sin(a),SteeringWheel::Depth,1,true},
            WheelHand{0.1f+radius*std::cos(a),SteeringWheel::Height+0.12f+radius*std::sin(a),SteeringWheel::Depth,1,true}};
    };
    for(int i=0;i<=90;++i) { hands=compressed(0.18f-0.16f*i/90);state=wheel.Update(hands,true,dt); }
    for(int i=0;i<90;++i) { hands=compressed(0.02f,0.7f*std::sin(i*0.1f));state=wheel.Update(hands,true,dt); }
    Check(state.held[0]&&state.held[1]&&std::abs(state.angle-lockAngle)<0.003f,"close-hand motion retains grip and cannot change steering reference");
    for(int i=0;i<=90;++i) { hands=compressed(0.02f+0.16f*i/90);state=wheel.Update(hands,true,dt); }
    for(int i=119;i>=0;--i) {
        hands={Rim(pi-i*pi/180),Rim(-i*pi/180)};
        for(auto& hand:hands) { hand.x+=0.1f;hand.y+=0.12f; }
        state=wheel.Update(hands,true,dt);
    }
    for(int i=0;i<90;++i) state=wheel.Update(hands,true,dt);
    Check(std::abs(state.steering)<0.003f,"opening hands after full lock still returns to the same centre");
    std::cout<<(failures?"FAIL":"PASS")<<": steering wheel scenarios\n";
    return failures?1:0;
}
