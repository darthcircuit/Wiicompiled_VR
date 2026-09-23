// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from heurazy's mario-kart-wii-VR-port (GPL-3.0-or-later).
#pragma once
#include <cstdint>
#include <cstring>
#include <span>

namespace aurora {
// Every referenced position that changes must belong to the local body matrix.
// Unchanged positions may use other joints in the same indexed draw (wing/engine).
inline bool NativeWheelDrawMatches(std::span<const uint8_t> original,
    std::span<const uint8_t> replacement, uint32_t positionStride,
    std::span<const uint8_t> vertices, uint32_t vertexStride, uint32_t positionOffset,
    uint32_t indexBytes, uint16_t matchingMatrices) {
    if(!positionStride || !vertexStride || (indexBytes!=1 && indexBytes!=2) ||
       positionOffset>=vertexStride || indexBytes>vertexStride-positionOffset ||
       original.size()!=replacement.size() || vertices.size()%vertexStride || !matchingMatrices) return false;
    bool changed=false;
    for(size_t start=0;start<vertices.size();start+=vertexStride) {
        const auto* vertex=vertices.data()+start;
        const uint32_t index=indexBytes==1 ? vertex[positionOffset]
            : (uint32_t(vertex[positionOffset])<<8)|vertex[positionOffset+1];
        const size_t offset=size_t(index)*positionStride;
        if(offset>original.size() || positionStride>original.size()-offset) return false;
        if(std::memcmp(original.data()+offset,replacement.data()+offset,positionStride)==0) continue;
        const uint32_t matrix=vertex[0]/3u;
        if(vertex[0]%3u || matrix>=16 || !(matchingMatrices&(1u<<matrix))) return false;
        changed=true;
    }
    return changed;
}
}
