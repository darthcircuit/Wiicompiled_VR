// gx_indirect.cpp - Indirect Texture / Bump Mapping
#include "gx_internal.h"

// ============================================================================
// Indirect Texture Stages
// ============================================================================

static void GX__SetNumIndStages_80171b38_gx(uint32_t n) { GXSetNumIndStages((u8)n); }
GX_DEFERRED_OVERRIDE_VOID(80171b38, GX__SetNumIndStages_80171b38, (uint32_t n), (n));

static void GX__SetIndTexOrder_80171a6c_gx(uint32_t s, uint32_t c, uint32_t m) {
    GXSetIndTexOrder((GXIndTexStageID)s, (GXTexCoordID)(c==0xFFu?0:c), (GXTexMapID)(m==0xFFu?0:m));
}
GX_DEFERRED_OVERRIDE_VOID(80171a6c, GX__SetIndTexOrder_80171a6c, (uint32_t s, uint32_t c, uint32_t m), (s, c, m));

static void GX__SetIndTexCoordScale_80171968_gx(uint32_t s, uint32_t ss, uint32_t ts) {
    GXSetIndTexCoordScale((GXIndTexStageID)s, (GXIndTexScale)ss, (GXIndTexScale)ts);
}
GX_DEFERRED_OVERRIDE_VOID(80171968, GX__SetIndTexCoordScale_80171968, (uint32_t s, uint32_t ss, uint32_t ts), (s, ss, ts));

struct GxIndTexMtxSnapshot {
    float m[6];
};
static void GX__SetIndTexMtx_gx(uint32_t id, GxIndTexMtxSnapshot mtx, uint32_t se) {
    GXSetIndTexMtx((GXIndTexMtxID)id, mtx.m, (s8)se);
}
extern "C" void GX__SetIndTexMtx_80171814(uint32_t id, uint32_t ma, uint32_t se) {
    GxIndTexMtxSnapshot mtx{};
    for(int i=0; i<6; ++i) mtx.m[i]=Memory::ReadFloat32(ma+i*4);
    GxThread::Post(&GX__SetIndTexMtx_gx, id, mtx, se);
}
PPC_NATIVE_OVERRIDE_VOID(80171814, GX__SetIndTexMtx_80171814, (uint32_t id, uint32_t ma, uint32_t se), (id, ma, se));

// ============================================================================
// TEV Indirect Texture Control
// ============================================================================

static void GX__SetTevDirect_80171b58_gx(uint32_t s) { GXSetTevDirect((GXTevStageID)s); }
GX_DEFERRED_OVERRIDE_VOID(80171b58, GX__SetTevDirect_80171b58, (uint32_t s), (s));

static void GX__SetTevIndWarp_80171ba0_gx(uint32_t ts, uint32_t is, uint32_t so, uint32_t rm, uint32_t ms) {
    GXSetTevIndWarp((GXTevStageID)std::min(ts, 15u), (GXIndTexStageID)std::min(is, 3u),
        (GXBool)so, (GXBool)rm, (GXIndTexMtxID)ms);
}
GX_DEFERRED_OVERRIDE_VOID(80171ba0, GX__SetTevIndWarp_80171ba0, (uint32_t ts, uint32_t is, uint32_t so, uint32_t rm, uint32_t ms), (ts, is, so, rm, ms));

static void GX__SetTevIndirect_801717ac_gx(uint32_t ts, uint32_t is, uint32_t f, uint32_t bs, uint32_t ms, uint32_t ws, uint32_t wt, uint32_t ap, uint32_t il, uint32_t as) {
    GXSetTevIndirect((GXTevStageID)std::min(ts,15u), (GXIndTexStageID)std::min(is,3u), (GXIndTexFormat)f,
        (GXIndTexBiasSel)bs, (GXIndTexMtxID)ms, (GXIndTexWrap)ws, (GXIndTexWrap)wt,
        (GXBool)ap, (GXBool)il, (GXIndTexAlphaSel)as);
}
GX_DEFERRED_OVERRIDE_VOID(801717ac, GX__SetTevIndirect_801717ac, (uint32_t ts, uint32_t is, uint32_t f, uint32_t bs, uint32_t ms, uint32_t ws, uint32_t wt, uint32_t ap, uint32_t il, uint32_t as), (ts, is, f, bs, ms, ws, wt, ap, il, as));
