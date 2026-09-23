// gx_tev.cpp - TEV Stage Configuration
#include "gx_internal.h"
#include "runtime_log.h"

// Aurora's CHECK() on these tev array indices compiles to nothing under NDEBUG, so this
// guest->native boundary must reject out-of-range IDs itself; a bad ID here is a malformed
// display list, never legitimate traffic.
namespace {

bool GxTevIdOk(uint32_t value, uint32_t limit, const char* what) {
    if (value < limit) return true;
    RT_LOGF(RT_TAG_GX, "%s out of range: %u (max %u), ignoring\n", what, value, limit - 1u);
    return false;
}

inline bool TevStageOk(uint32_t s) { return GxTevIdOk(s, GX_MAX_TEVSTAGE, "TEV stage"); }
inline bool TevRegOk(uint32_t id) { return GxTevIdOk(id, GX_MAX_TEVREG, "TEV register"); }
inline bool TevKColorOk(uint32_t id) { return GxTevIdOk(id, GX_MAX_KCOLOR, "TEV konstant color"); }
inline bool TevSwapOk(uint32_t id) { return GxTevIdOk(id, GX_MAX_TEVSWAP, "TEV swap selector"); }

} // namespace

// ============================================================================
// TEV Stage Count and Order
// ============================================================================

static void GX__SetNumTevStages_801722a8_gx(uint32_t n) {
    // GXSetNumTevStages takes a count, not an index, so the inclusive bound is
    // GX_MAX_TEVSTAGE itself.
    if (n > GX_MAX_TEVSTAGE) {
        RT_LOGF(RT_TAG_GX, "GXSetNumTevStages: invalid count %u, ignoring\n", n);
        return;
    }
    GXSetNumTevStages((u8)n);
}
GX_DEFERRED_OVERRIDE_VOID(801722a8, GX__SetNumTevStages_801722a8, (uint32_t n), (n));

static void GX__SetTevOp_80171c4c_gx(uint32_t s, uint32_t m) {
    if (!TevStageOk(s)) return;
    GXSetTevOp((GXTevStageID)s, (GXTevMode)m);
}
GX_DEFERRED_OVERRIDE_VOID(80171c4c, GX__SetTevOp_80171c4c, (uint32_t s, uint32_t m), (s, m));

static void GX__SetTevOrder_8017214c_gx(uint32_t s, uint32_t c, uint32_t m, uint32_t col) {
    if (!TevStageOk(s)) return;
    GXSetTevOrder((GXTevStageID)s, (GXTexCoordID)c, (GXTexMapID)m, (GXChannelID)col);
}
GX_DEFERRED_OVERRIDE_VOID(8017214c, GX__SetTevOrder_8017214c, (uint32_t s, uint32_t c, uint32_t m, uint32_t col), (s, c, m, col));

// ============================================================================
// TEV Color/Alpha Inputs
// ============================================================================

static void GX__SetTevColorIn_80171ce0_gx(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (!TevStageOk(s)) return;
    GXSetTevColorIn((GXTevStageID)s, (GXTevColorArg)a, (GXTevColorArg)b, (GXTevColorArg)c, (GXTevColorArg)d);
}
GX_DEFERRED_OVERRIDE_VOID(80171ce0, GX__SetTevColorIn_80171ce0, (uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d), (s, a, b, c, d));

static void GX__SetTevAlphaIn_80171d20_gx(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (!TevStageOk(s)) return;
    GXSetTevAlphaIn((GXTevStageID)s, (GXTevAlphaArg)a, (GXTevAlphaArg)b, (GXTevAlphaArg)c, (GXTevAlphaArg)d);
}
GX_DEFERRED_OVERRIDE_VOID(80171d20, GX__SetTevAlphaIn_80171d20, (uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d), (s, a, b, c, d));

// ============================================================================
// TEV Color/Alpha Operations
// ============================================================================

static void GX__SetTevColorOp_80171d60_gx(uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_) {
    if (!TevStageOk(s) || !TevRegOk(or_)) return;
    GXSetTevColorOp((GXTevStageID)s, (GXTevOp)op, (GXTevBias)b, (GXTevScale)sc, (GXBool)cl, (GXTevRegID)or_);
}
GX_DEFERRED_OVERRIDE_VOID(80171d60, GX__SetTevColorOp_80171d60, (uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_), (s, op, b, sc, cl, or_));

static void GX__SetTevAlphaOp_80171db8_gx(uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_) {
    if (!TevStageOk(s) || !TevRegOk(or_)) return;
    GXSetTevAlphaOp((GXTevStageID)s, (GXTevOp)op, (GXTevBias)b, (GXTevScale)sc, (GXBool)cl, (GXTevRegID)or_);
}
GX_DEFERRED_OVERRIDE_VOID(80171db8, GX__SetTevAlphaOp_80171db8, (uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_), (s, op, b, sc, cl, or_));

// ============================================================================
// TEV Color Registers
// ============================================================================

void GX__SetTevColor_gx(uint32_t id, uint32_t colorWord) {
    GXSetTevColor((GXTevRegID)id, DecodeGxColor(colorWord));
}
extern "C" void GX__SetTevColor_80171e10(uint32_t id, uint32_t cp) {
    if (!TevRegOk(id)) return;
    GxThread::Post(&GX__SetTevColor_gx, id, Memory::Read32(cp));
}
PPC_NATIVE_OVERRIDE_VOID(80171e10, GX__SetTevColor_80171e10, (uint32_t id, uint32_t cp), (id, cp));

static void GX__SetTevColorS10_gx(uint32_t id, uint32_t rg, uint32_t ba) {
    GXColorS10 c;
    c.r=static_cast<s16>(rg>>16); c.g=static_cast<s16>(rg&0xFFFFu); c.b=static_cast<s16>(ba>>16); c.a=static_cast<s16>(ba&0xFFFFu);
    GXSetTevColorS10((GXTevRegID)id, c);
}
extern "C" void GX__SetTevColorS10_80171e70(uint32_t id, uint32_t cp) {
    if (!TevRegOk(id)) return;
    GxThread::Post(&GX__SetTevColorS10_gx, id, Memory::Read32(cp), Memory::Read32(cp + 4));
}
PPC_NATIVE_OVERRIDE_VOID(80171e70, GX__SetTevColorS10_80171e70, (uint32_t id, uint32_t cp), (id, cp));

void GX__SetTevKColor_gx(uint32_t id, uint32_t colorWord) {
    GXSetTevKColor((GXTevKColorID)id, DecodeGxColor(colorWord));
}
extern "C" void GX__SetTevKColor_80171ed4(uint32_t id, uint32_t cp) {
    if (!TevKColorOk(id)) return;
    GxThread::Post(&GX__SetTevKColor_gx, id, Memory::Read32(cp));
}
PPC_NATIVE_OVERRIDE_VOID(80171ed4, GX__SetTevKColor_80171ed4, (uint32_t id, uint32_t cp), (id, cp));

static void GX__SetTevKColorSel_80171f30_gx(uint32_t s, uint32_t sel) { if (!TevStageOk(s)) return; GXSetTevKColorSel((GXTevStageID)s, (GXTevKColorSel)sel); }
GX_DEFERRED_OVERRIDE_VOID(80171f30, GX__SetTevKColorSel_80171f30, (uint32_t s, uint32_t sel), (s, sel));

static void GX__SetTevKAlphaSel_80171f80_gx(uint32_t s, uint32_t sel) { if (!TevStageOk(s)) return; GXSetTevKAlphaSel((GXTevStageID)s, (GXTevKAlphaSel)sel); }
GX_DEFERRED_OVERRIDE_VOID(80171f80, GX__SetTevKAlphaSel_80171f80, (uint32_t s, uint32_t sel), (s, sel));

// ============================================================================
// TEV Swap Tables
// ============================================================================

static void GX__SetTevSwapModeTable_8017200c_gx(uint32_t id, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (!TevSwapOk(id)) return;
    GXSetTevSwapModeTable((GXTevSwapSel)id, (GXTevColorChan)r, (GXTevColorChan)g, (GXTevColorChan)b, (GXTevColorChan)a);
}
GX_DEFERRED_OVERRIDE_VOID(8017200c, GX__SetTevSwapModeTable_8017200c, (uint32_t id, uint32_t r, uint32_t g, uint32_t b, uint32_t a), (id, r, g, b, a));

static void GX__SetTevSwapMode_80171fd0_gx(uint32_t s, uint32_t rs, uint32_t ts) {
    if (!TevStageOk(s) || !TevSwapOk(rs) || !TevSwapOk(ts)) return;
    GXSetTevSwapMode((GXTevStageID)s, (GXTevSwapSel)rs, (GXTevSwapSel)ts);
}
GX_DEFERRED_OVERRIDE_VOID(80171fd0, GX__SetTevSwapMode_80171fd0, (uint32_t s, uint32_t rs, uint32_t ts), (s, rs, ts));
