// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from heurazy's mario-kart-wii-VR-port (GPL-3.0-or-later).
#pragma once
#include "vr/mkw_vr_first_person.h"
#include <vector>

namespace mkw::vr {
// A number of MKW karts bake the steering wheel into their single body bone.
// Find its thin disc around the authored hand targets, including the hub and
// spokes, and rotate only that disc. Work on a render copy, never guest assets.
inline unsigned RotateNativeWheelVertices(std::vector<detail::Vec3>& points,
        detail::Vec3 center,float radius,float angle,const Mtx34* bodyCorrection=nullptr) {
    if (!(radius>4 && radius<100) || !detail::IsFiniteFloat(&angle)) return 0;
    if(bodyCorrection && !detail::IsFiniteMtx34(*bodyCorrection)) return 0;
    float meanY=0,meanZ=0; unsigned count=0;
    const auto candidate=[&](const detail::Vec3& p) {
        return std::abs(p.x-center.x)<radius*1.5f && std::abs(p.y-center.y)<radius*1.5f &&
            std::abs(p.z-center.z)<radius*0.9f;
    };
    for(const auto& p:points) if(candidate(p)) { meanY+=p.y; meanZ+=p.z; ++count; }
    if(count<8) return 0;
    meanY/=count; meanZ/=count;
    float yy=0,yz=0;
    for(const auto& p:points) if(candidate(p)) { yy+=(p.y-meanY)*(p.y-meanY); yz+=(p.y-meanY)*(p.z-meanZ); }
    if(yy<radius*radius) return 0;
    const float slope=std::clamp(yz/yy,-1.0f,1.0f);
    center.z=meanZ+slope*(center.y-meanY);
    const float inv=1/std::sqrt(1+slope*slope);
    const detail::Vec3 up{0,inv,slope*inv},normal{0,-slope*inv,inv};
    const float c=std::cos(angle),s=std::sin(angle);
    unsigned changed=0;
    for(auto& p:points) {
        const detail::Vec3 delta{p.x-center.x,p.y-center.y,p.z-center.z};
        const float x=delta.x,y=detail::Dot(delta,up),z=detail::Dot(delta,normal);
        if(x*x+y*y>radius*radius*2.25f || std::abs(z)>radius*0.30f) continue;
        const float rx=c*x-s*y,ry=s*x+c*y;
        p={center.x+rx,center.y+up.y*ry+normal.y*z,center.z+up.z*ry+normal.z*z};
        // The body may spin during tricks/damage while the seated reference
        // stays level. Compensate only the wheel, leaving chassis animation intact.
        if(bodyCorrection) p=detail::TransformPoint(*bodyCorrection,p.x,p.y,p.z);
        ++changed;
    }
    return changed;
}
} // namespace mkw::vr
