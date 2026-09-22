// SPDX-License-Identifier: GPL-3.0-or-later

#include "gx_native_wheel.h"

#include "gx_internal.h"

#include <aurora/aurora.h>

#include <cstring>
#include <vector>

namespace GxNativeWheel {
// Named rather than anonymous: runtime sources are unity-built in groups.
namespace records {

struct Header {
    const void* source;
    uint32_t size;
    float modelView[12];
};

void InvokeClear(const uint8_t*, uint32_t) { aurora_clear_native_wheel_vertices(); }

void InvokeVertices(const uint8_t* payload, uint32_t payloadBytes) {
    Header header{};
    if (payloadBytes < sizeof(header)) {
        return;
    }
    std::memcpy(&header, payload, sizeof(header));
    if (payloadBytes - sizeof(header) < header.size) {
        return;
    }
    aurora_set_native_wheel_vertices(header.source, payload + sizeof(header), header.size, header.modelView);
}

void Post(const void* source, const uint8_t* bytes, uint32_t size, const float modelView[12]) {
    if (!GxThread::Enabled()) {
        aurora_set_native_wheel_vertices(source, bytes, size, modelView);
        return;
    }
    Header header{source, size, {}};
    std::memcpy(header.modelView, modelView, sizeof(header.modelView));
    std::vector<uint8_t> payload(sizeof(header) + size);
    std::memcpy(payload.data(), &header, sizeof(header));
    std::memcpy(payload.data() + sizeof(header), bytes, size);
    GxThread::detail::PostRecord(&InvokeVertices, payload.data(), static_cast<uint32_t>(payload.size()));
}

} // namespace records

void PostClear() {
    if (!GxThread::Enabled()) {
        aurora_clear_native_wheel_vertices();
        return;
    }
    GxThread::detail::PostRecord(&records::InvokeClear, nullptr, 0);
}

bool PostVertices(uint32_t guestArray, const uint8_t* bytes, uint32_t size, const float modelView[12]) {
    if (guestArray == 0 || bytes == nullptr || size == 0 || size > 65536 || modelView == nullptr) {
        return false;
    }
    // GXSetArray keeps the guest's own (cached) address; a display list's CP
    // array base is physical and decodes the way gx_cp_decode.h does it.
    const void* sdk = GuestToHostPtr(guestArray, size);
    const void* cp = GuestToHostPtr(DecodeCpArrayBaseGuestAddress(guestArray), size);
    if (sdk == nullptr && cp == nullptr) {
        return false;
    }
    if (sdk != nullptr) {
        records::Post(sdk, bytes, size, modelView);
    }
    if (cp != nullptr && cp != sdk) {
        records::Post(cp, bytes, size, modelView);
    }
    return true;
}

uint32_t LastDrawCount() { return aurora_native_wheel_draw_count(); }

} // namespace GxNativeWheel
