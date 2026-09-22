// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from heurazy's mario-kart-wii-VR-port (GPL-3.0-or-later).
//
// The VR first-person camera animates the local vehicle's steering wheel by
// handing Aurora a rotated copy of one of the vehicle's position arrays. The
// copy applies only to a draw that binds that exact array *and* carries the
// local vehicle's model-view matrix, so an opponent sharing the asset keeps
// the original vertices. Everything here runs on the GX (command processor)
// thread; the set/clear entry points are posted there by the runtime.
#pragma once
#include "gx.hpp"
#include <algorithm>
#include <vector>
#include <cstring>
#include <cmath>
#include <atomic>
#include <aurora/native_wheel_match.hpp>
namespace aurora::gx {
struct NativeWheelArray {
    const void* source{};
    std::vector<uint8_t> bytes;
    std::array<float,12> modelView{};
    gfx::Range uploaded{};
    // Element stride of `uploaded`, which differs from the array's when the upload is padded.
    uint32_t uploadedStride=0;
};
inline std::vector<NativeWheelArray> nativeWheelArrays;
inline uint32_t nativeWheelMatches=0;
inline std::atomic<uint32_t> nativeWheelLastMatches{0};

// For the host log: why draws of the replaced arrays did or did not take them.
struct NativeWheelDiagnostics {
    uint32_t sets=0;          // replacement sets cleared since the last report
    uint32_t boundDraws=0;    // draws that bound a replacement's source array
    uint32_t oversizeDraws=0; // ... whose bound range was larger than the replacement
    uint32_t outsideDraws=0;  // draws that bound a cleared set's source while no set was active
    uint32_t matchedDraws=0;
    float bestError=INFINITY; // smallest largest-element difference of a position matrix
    bool bestIndexed=false;
    std::array<float,12> bestMatrix{};
    std::array<float,12> expected{};
};
inline NativeWheelDiagnostics nativeWheelDiagnostics;
inline std::vector<const void*> nativeWheelPreviousSources;
inline uint32_t nativeWheelClears=0;
inline uint32_t nativeWheelReports=0;

inline void native_wheel_note_outside(const void* source) {
    for(const void* previous:nativeWheelPreviousSources)
        if(previous==source) { ++nativeWheelDiagnostics.outsideDraws;return; }
}

inline void native_wheel_note_bound(const NativeWheelArray& replacement,bool indexedMatrix) {
    auto& diagnostics=nativeWheelDiagnostics;
    ++diagnostics.boundDraws;
    for(uint32_t slot=0;slot<MaxPnMtx;++slot) {
        if(!indexedMatrix && slot!=g_gxState.currentPnMtx) continue;
        const auto* matrix=reinterpret_cast<const float*>(&g_gxState.pnMtx[slot].pos);
        float error=0;
        for(int i=0;i<12;++i) error=std::max(error,std::abs(matrix[i]-replacement.modelView[i]));
        if(error<diagnostics.bestError) {
            diagnostics.bestError=error;
            diagnostics.bestIndexed=indexedMatrix;
            std::memcpy(diagnostics.bestMatrix.data(),matrix,sizeof(float)*12);
            diagnostics.expected=replacement.modelView;
        }
    }
}

// Called as a set is cleared (GXAurora.cpp): one host log line at about half a
// second, five seconds and a minute of sets.
void native_wheel_report();
inline bool native_wheel_source(const void* source) {
    for(const auto& replacement:nativeWheelArrays) if(source==replacement.source) return true;
    return false;
}
inline NativeWheelArray* native_wheel_array(const AttrArray& array,const uint8_t* vertices,
    uint32_t vertexBytes,uint32_t vertexStride,uint32_t positionOffset) {
    const bool indexedMatrix=g_gxState.vtxDesc[GX_VA_PNMTXIDX]==GX_DIRECT;
    if(!indexedMatrix && g_gxState.currentPnMtx>=MaxPnMtx) return nullptr;
    for(auto& replacement:nativeWheelArrays) {
        if(array.data!=replacement.source) continue;
        if(array.size>replacement.bytes.size()) { ++nativeWheelDiagnostics.oversizeDraws;continue; }
        native_wheel_note_bound(replacement,indexedMatrix);
        // A matching asset alone would also animate an opponent. Multi-joint
        // models need a per-position ownership check, not a blanket exclusion.
        uint16_t matching=0;
        for(uint32_t slot=0;slot<MaxPnMtx;++slot) {
            if(!indexedMatrix && slot!=g_gxState.currentPnMtx) continue;
            const auto* matrix=reinterpret_cast<const float*>(&g_gxState.pnMtx[slot].pos);
            bool matches=true;
            for(int i=0;i<12;++i) {
                uint32_t bits;std::memcpy(&bits,&matrix[i],4);
                if((bits&0x7f800000u)==0x7f800000u ||
                    std::abs(matrix[i]-replacement.modelView[i])>(i%4==3?0.1f:0.002f)) { matches=false;break; }
            }
            if(matches) matching|=uint16_t(1u<<slot);
        }
        if(!matching) continue;
        if(indexedMatrix && !NativeWheelDrawMatches(
            {static_cast<const uint8_t*>(array.data),array.size},
            {replacement.bytes.data(),array.size},array.stride,
            {vertices,vertexBytes},vertexStride,positionOffset,
            g_gxState.vtxDesc[GX_VA_POS]==GX_INDEX8?1:2,matching)) continue;
        ++nativeWheelMatches;++nativeWheelDiagnostics.matchedDraws;return &replacement;
    }
    return nullptr;
}
}
