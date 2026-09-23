// gx_pixel.cpp - Pixel Processing, Blending, and Fog
#include "gx_internal.h"

// ============================================================================
// Blend Mode
// ============================================================================

static void GX__SetBlendMode_8017277c_gx(uint32_t t, uint32_t s, uint32_t d, uint32_t op) {
    GXSetBlendMode(static_cast<GXBlendMode>(t), static_cast<GXBlendFactor>(s),
                   static_cast<GXBlendFactor>(d), static_cast<GXLogicOp>(op));
}
GX_DEFERRED_OVERRIDE_VOID(8017277c, GX__SetBlendMode_8017277c, (uint32_t t, uint32_t s, uint32_t d, uint32_t op), (t, s, d, op));

static void GX__SetColorUpdate_801727cc_gx(uint32_t en) {
    GXSetColorUpdate(static_cast<GXBool>(en));
}
GX_DEFERRED_OVERRIDE_VOID(801727cc, GX__SetColorUpdate_801727cc, (uint32_t en), (en));

static void GX__SetAlphaUpdate_801727f8_gx(uint32_t en) {
    GXSetAlphaUpdate(static_cast<GXBool>(en));
}
GX_DEFERRED_OVERRIDE_VOID(801727f8, GX__SetAlphaUpdate_801727f8, (uint32_t en), (en));

// ============================================================================
// Z Buffer
// ============================================================================

static void GX__SetZMode_80172824_gx(uint32_t ce, uint32_t f, uint32_t ue) {
    GXSetZMode(static_cast<GXBool>(ce), static_cast<GXCompare>(f), static_cast<GXBool>(ue));
}
GX_DEFERRED_OVERRIDE_VOID(80172824, GX__SetZMode_80172824, (uint32_t ce, uint32_t f, uint32_t ue), (ce, f, ue));

static void GX__SetZCompLoc_80172858_gx(uint32_t bt) {
    GXSetZCompLoc(static_cast<GXBool>(bt));
}
GX_DEFERRED_OVERRIDE_VOID(80172858, GX__SetZCompLoc_80172858, (uint32_t bt), (bt));

// ============================================================================
// Pixel Format and Dither
// ============================================================================

static void GX__SetPixelFmt_80172888_gx(uint32_t pf, uint32_t zf) {
    GXSetPixelFmt(static_cast<GXPixelFmt>(pf), static_cast<GXZFmt16>(zf));
}
GX_DEFERRED_OVERRIDE_VOID(80172888, GX__SetPixelFmt_80172888, (uint32_t pf, uint32_t zf), (pf, zf));

static void GX__SetDither_80172930_gx(uint32_t d) {
    GXSetDither(static_cast<GXBool>(d));
}
GX_DEFERRED_OVERRIDE_VOID(80172930, GX__SetDither_80172930, (uint32_t d), (d));

static void GX__SetDstAlpha_8017295c_gx(uint32_t en, uint32_t a) {
    GXSetDstAlpha(static_cast<GXBool>(en), static_cast<u8>(a));
}
GX_DEFERRED_OVERRIDE_VOID(8017295c, GX__SetDstAlpha_8017295c, (uint32_t en, uint32_t a), (en, a));

// ============================================================================
// Fog and Alpha Compare
// ============================================================================

static void GX__SetFog_gx(uint32_t t, float sz, float ez, float nz, float fz, uint32_t colorWord) {
    GXSetFog((GXFogType)t, sz, ez, nz, fz, DecodeGxColor(colorWord));
}
extern "C" void GX__SetFog_801722cc(uint32_t t, float sz, float ez, float nz, float fz, uint32_t cp) {
    // The colour is copied out of guest memory at call time, as the SDK does.
    GxThread::Post(&GX__SetFog_gx, t, sz, ez, nz, fz, Memory::Read32(cp));
}
PPC_NATIVE_OVERRIDE_VOID(801722cc, GX__SetFog_801722cc, (uint32_t t, float sz, float ez, float nz, float fz, uint32_t cp), (t, sz, ez, nz, fz, cp));

static void GX__SetAlphaCompare_80172088_gx(uint32_t c0, uint32_t r0, uint32_t op, uint32_t c1, uint32_t r1) {
    g_alphaCompareValid = true;
    GXSetAlphaCompare((GXCompare)c0, (u8)r0, (GXAlphaOp)op, (GXCompare)c1, (u8)r1);
}
GX_DEFERRED_OVERRIDE_VOID(80172088, GX__SetAlphaCompare_80172088, (uint32_t c0, uint32_t r0, uint32_t op, uint32_t c1, uint32_t r1), (c0, r0, op, c1, r1));

static void GX__SetZTexture_801720c0_gx(uint32_t op, uint32_t f, uint32_t b) { GXSetZTexture((GXZTexOp)op, (GXTexFmt)f, b); }
GX_DEFERRED_OVERRIDE_VOID(801720c0, GX__SetZTexture_801720c0, (uint32_t op, uint32_t f, uint32_t b), (op, f, b));

// ============================================================================
// Culling and Clipping
// ============================================================================

static void GX__SetCullMode_8016f3b8_gx(uint32_t m) {
    GXSetCullMode(static_cast<GXCullMode>(m));
}
GX_DEFERRED_OVERRIDE_VOID(8016f3b8, GX__SetCullMode_8016f3b8, (uint32_t m), (m));

static void GX__SetCoPlanar_8016f3e0_gx(uint32_t en) { GXSetCoPlanar((GXBool)en); }
GX_DEFERRED_OVERRIDE_VOID(8016f3e0, GX__SetCoPlanar_8016f3e0, (uint32_t en), (en));

static void GX__SetClipMode_8017351c_gx(uint32_t m) { GXSetClipMode((GXClipMode)m); }
GX_DEFERRED_OVERRIDE_VOID(8017351c, GX__SetClipMode_8017351c, (uint32_t m), (m));
