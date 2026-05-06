#include "pch.h"
#include "hook.h"
#include "debug.h"
#include "ztl/ztl.h"
#include "ztl/zcom.h"
#include "wvs/uimonsterbook.h"
#include "wvs/ctrlwnd.h"
#include "wvs/util.h"
#include "wvs/wnd.h"

#include <cstring>


// === Layout / sizing ========================================================
//
// v95.1 CUIMonsterBook full-layout size. KMST's class extends to MOBINFO end
// at +0x1240 (KMST coords); v95's CUIWnd is 0x6A0 bytes larger so the same
// layout reaches +0x18E0 here. Calling the v95 CUIWnd ctor on an under-sized
// buffer would corrupt adjacent heap blocks — keep the alloc full-fat.
constexpr size_t kCUIMonsterBookSize = 0x18E0;


// === v95 entry points =======================================================
//
//   FUN_008dd680  CUIWnd::CUIWnd(this, nUIType, x, y, w, ?, ?, ?)
//                 — stamps the IGObj / IUIMsgHandler / ZRefCounted vtables
//                   at *this+0/+4/+8 and seats m_nUIType at +0xAD0.
//   FUN_008dd300  CUIWnd::CreateUIWndPosSaved(this, w, h, ?)
//   FUN_009d83f0  CWvsContext::UI_Open(int nUIType, int nOption)
//                 — case 9 (MONSTERBOOK) was stripped to `break`.
//   FUN_009d5370  CWvsContext::UI_Close(int nUIType)
//                 — case 9 IS implemented (gates on this+0x3EB4) but the
//                   slot it gates on is never set in stock v95, so the
//                   handler always no-ops in practice.
//   CWvsContext+0x3EB4  the per-UI window pointer slot for case 9
//                 (canonical close mechanism — every CUIWnd lives in a
//                 ZRef-style 8-byte slot in CWvsContext at +0x3E30+8*N;
//                 case 9's slot is at +0x3EB0/+0x3EB4). v95 UI_Close
//                 reads this, fires FUN_009d1ae0 to clear it, which
//                 Releases the window — refcount→0, virtual dtor runs,
//                 ~CUIWnd → ~CWnd unregisters from wnd-manager. v95
//                 stripped only the OPEN-side population of this slot,
//                 not the close path. Confirmed via case-9 close asm at
//                 0x009D5681: `mov ecx,[esi+0x3eb4]; test ecx,ecx; jz
//                 break_path; call FUN_009b0e50; push 0; lea ecx,
//                 [esi+0x3eb0]; call FUN_009d1ae0`.
//   DAT_00c6f010  global CUIMonsterBook* singleton — OnMonsterBookSetCard
//                 / OnMonsterBookSetCover gate on this for null-bail.
constexpr uintptr_t kCUIWnd_ctor                          = 0x008DD680;
constexpr uintptr_t kCUIWnd_CreateUIWndPosSaved           = 0x008DD300;
constexpr uintptr_t kCWvsContext_UI_Open                  = 0x009D83F0;
constexpr uintptr_t kCWvsContext_UI_Close                 = 0x009D5370;
constexpr uintptr_t kCWvsContext_MonsterBookSlotPtr_Off   = 0x3EB4;
constexpr uintptr_t kZRefCounted_RefField_Off             = 0xC;
constexpr uintptr_t kDAT_MonsterBookGlobal                = 0x00C6F010;


// === Mod-side state =========================================================

namespace {
    // Mirror v95's DAT_00c6f010 — set on open, cleared on close. Once the
    // pointer is non-null OnMonsterBookSetCard / OnMonsterBookSetCover
    // packet handlers will write into our buffer; until the builders below
    // initialise the AREA / CardTable members the writes land in zeroed
    // memory and the handlers' inner ZArray ops should still work, but card
    // SPRITES won't render until CreateLayer / CreateCardTable land.
    void*& g_pV95MonsterBookGlobal =
        *reinterpret_cast<void**>(kDAT_MonsterBookGlobal);

    // Local mirror of the v95 slot pointer for debug-log convenience —
    // UI_Open's "already open" gate reads the slot directly so this is
    // just a friendly handle for the close-side log line.
    void* g_pMonsterBookUI = nullptr;

    // Bg-layer ownership. KMST builds 3 child layers but no full-window
    // bg layer — the bg image normally lives in CWnd's draw chain via
    // m_sBackgrndUOL+m_pCanvas mechanics (which our wnd doesn't have
    // populated since we never went through the standard Open path).
    // Instead we attach a 4th overlay layer parented to CWnd::GetLayer,
    // sized to the full 475x349 wnd, with the bg WZ canvas copied in.
    //
    // MonsterBook is a singleton (DAT_00c6f010 gates open/close) so a
    // file-scope smart-ptr is safe — single open instance at a time. The
    // smart-ptr's Release fires on Construct's overwrite (next open) or
    // explicitly in PreDestroy.
    IWzGr2DLayerPtr g_pBgLayer;
}


// === Builders ===============================================================
//
// Each builder records the KMST source it ports from. Offsets in the
// comments are in KMST coordinates; subtract 0x468 (KMST sizeof(CUIWnd))
// and add 0xB08 (v95 sizeof(CUIWnd)) — net +0x6A0 — to get the v95
// absolute offset.
//
// Status (2026-05-06 late-late): CreateCtrl + CreateLayer + CreateRect
// are LANDED. CreateCardTable / CreateFontArray remain DEFERRED with
// rationale in their function-level comments (gated on either a v95
// enumeration helper that doesn't exist for cards-by-tab, or
// StringPool IDs lost in Ghidra decomp for the font slots).
//
// Per-builder logging: every builder emits DEBUG_MESSAGE markers on
// entry/exit and around each significant call. With AttachDebugConsole
// active in _DEBUG builds, the next Windows test transcript will show
// exactly which builder ran how far.

// KMST 0x008486DE (217 lines) — tools/decomp/cache_kmst/008486de.c.
// Allocates 9 controls into v95-coord member offsets:
//   +0x1560  ZRef<CCtrlLPTab>   id=0x7D6   pos rect (-7, 25, 50, 305)
//   +0x1568  ZRef<CCtrlRPTab>   id=0x7D7   pos rect (439, 25, 506, 220)
//   +0x80    ZRef<CCtrlButton>  id=1000    pos (0x1AD,8)   — close X (CUIWnd::m_pBtClose)
//   +0x1578  ZRef<CCtrlButton>  id=2000    pos (0xAF,0x1D)
//   +0x1580  ZRef<CCtrlButton>  id=0x7D1   pos (0x30,0x11D)
//   +0x1588  ZRef<CCtrlButton>  id=0x7D2   pos (0xB9,0x11D)
//   +0x1590  ZRef<CCtrlButton>  id=0x7D3   pos (0x10E,0x11D)
//   +0x1598  ZRef<CCtrlButton>  id=0x7D4   pos (0x197,0x11D)
//   +0x1570  ZRef<CCtrlEdit>    id=0x7D5   rect (0x31, 0x1E, 0x78, 0xF)
//
// Phase 2-port-1 (retry): the previous attempt passed empty sUOL and crashed
// in RESMAN.DLL. v95 CCtrlButton::CreateCtrl unconditionally loads the UOL
// from CREATEPARAM, so each button needs a real UIWindow.img path. The four
// UOLs (BtClose, BtSearch, arrowLeft, arrowRight) come from the v95
// UI.wz/UIWindow.img/MonsterBook tree (verified via `wz_search tree UI.wz
// UIWindow.img/MonsterBook`). KMST resolves the same four via StringPool
// keys 0xAEF/0xAF0/0xAF1/0xAF2; arrowLeft and arrowRight repeat across the
// two scroll-pair offsets.
static void MonsterBook_CreateCtrl(void* pThis) {
    DEBUG_MESSAGE("MonsterBook_CreateCtrl: enter pThis=0x%08X", pThis);

    struct ButtonSpec {
        size_t         offset;   // v95-coord offset of the ZRef<CCtrlButton> slot
        uint32_t       id;
        int32_t        x;
        int32_t        y;
        const wchar_t* sUOL;
    };
    static const ButtonSpec kButtons[] = {
        { 0x0080, 1000,    0x1AD, 8,     L"UI/UIWindow.img/MonsterBook/BtClose"    },
        { 0x1578, 2000,    0x0AF, 0x1D,  L"UI/UIWindow.img/MonsterBook/BtSearch"   },
        { 0x1580, 0x07D1,  0x030, 0x11D, L"UI/UIWindow.img/MonsterBook/arrowLeft"  },
        { 0x1588, 0x07D2,  0x0B9, 0x11D, L"UI/UIWindow.img/MonsterBook/arrowRight" },
        { 0x1590, 0x07D3,  0x10E, 0x11D, L"UI/UIWindow.img/MonsterBook/arrowLeft"  },
        { 0x1598, 0x07D4,  0x197, 0x11D, L"UI/UIWindow.img/MonsterBook/arrowRight" },
    };

    auto* pParent = reinterpret_cast<CWnd*>(pThis);
    for (const auto& spec : kButtons) {
        DEBUG_MESSAGE("  button id=%u (0x%X) at +0x%X pos=(%d,%d) UOL=%S",
                      spec.id, spec.id, static_cast<unsigned>(spec.offset),
                      spec.x, spec.y, spec.sUOL);
        auto* pBtn = new CCtrlButton();
        if (!pBtn) {
            DEBUG_MESSAGE("    alloc failed");
            continue;
        }
        // Fresh CREATEPARAM per button — sUOL ownership stays scoped to
        // the CreateCtrl call. KMST passes bAcceptFocus=1, others zero.
        CCtrlButton::CREATEPARAM cp;
        cp.bAcceptFocus = 1;
        cp.sUOL = spec.sUOL;

        auto* pSlot = reinterpret_cast<ZRef<CCtrlButton>*>(
            static_cast<uint8_t*>(pThis) + spec.offset);
        *pSlot = pBtn;
        pBtn->CreateCtrl(pParent, spec.id, spec.x, spec.y, 0, &cp);
        DEBUG_MESSAGE("    button ok pBtn=0x%08X stored at +0x%X",
                      pBtn, static_cast<unsigned>(spec.offset));
    }
    DEBUG_MESSAGE("MonsterBook_CreateCtrl: 6 buttons populated");
}

// KMST 0x00848C8A (601 lines) — tools/decomp/cache_kmst/00848c8a.c, full
// verbatim port. Builds the 3 LAYER child controls at v95 +0x15A0..+0x15B8
// (8 bytes each: 4-byte dirty flag + 4-byte raw IWzGr2DLayer*).
//
// KMST builds the slots in order [0], [2], [1]. Each block runs:
//   1. _g_gr->CreateLayer(left, top, 0, 0, 0, vtEmpty, dwFilter=0L)
//   2. m_aLayer[i].pLayer = newLayer (raw COM ptr + AddRef)
//   3. newLayer->origin  = parentLayerVariant      (vtbl+0x64 / IWzVector2D)
//   4. newLayer->overlay = parentLayerVariant      (vtbl+0xFC / IWzGr2DLayer)
//   5. newLayer->color   = 0xFFFFFFFF              (vtbl+0xE0)
//   6. newLayer->z       = parentLayer->z + delta  (vtbl+0xB4)
//   7. PcCreateObject<IWzCanvasPtr>(L"Canvas", &c)
//      (KMST resolves the class name via StringPool::GetStringW(0x40A);
//      verified via objdump at 0x848eac, 0x8492e6, 0x8496a2 — same key
//      for all three blocks. The temporarystat.cpp / inlink.cpp callers
//      already use L"Canvas" with the identical PcCreateObject + Create +
//      put_cx/put_cy + InsertCanvas pattern.)
//   8. canvas->Create(W, H); canvas->cx = 0; canvas->cy = 0
//   9. newLayer->InsertCanvas(canvas) — VARIANTs default to vtEmpty
//
// Block 3 (LAYER[1]) is the "card grid" layer: 240x20 layer at (240, 20)
// with a 220x290 canvas; the other two blocks share a 174x256 canvas at
// (40, 25). z-deltas: LAYER[0]=+1, LAYER[1]=+2, LAYER[2]=+2 (LAYER[1] and
// LAYER[2] share the same z; LAYER[2] is built later so it ends up on top
// per insertion order).
//
// COM smart-pointer wrappers throw _com_error on FAILED HRESULT. We catch
// at the body level so a single failure aborts the build cleanly without
// crashing the host v95 client. DEBUG_MESSAGE between every step so the
// next Windows test transcript shows exactly which call broke if any do.
static void MonsterBook_CreateLayer(void* pThis) {
    DEBUG_MESSAGE("MonsterBook_CreateLayer: enter pThis=0x%08X", pThis);

    auto* pCWnd = reinterpret_cast<CWnd*>(pThis);
    auto* pBytes = static_cast<uint8_t*>(pThis);

    // CWnd::GetLayer (v95 0x0042A270) — returns the wnd's primary IWzGr2DLayer
    // that we'll parent the 3 sub-layers under. Without it the put_origin /
    // put_overlay calls have no anchor and the children float independently.
    IWzGr2DLayerPtr pParentLayer;
    try {
        pParentLayer = pCWnd->GetLayer();
    } catch (const _com_error& e) {
        DEBUG_MESSAGE("  CWnd::GetLayer threw HRESULT 0x%08X (%s)",
                      static_cast<unsigned>(e.Error()), e.ErrorMessage());
        return;
    }
    if (!pParentLayer) {
        DEBUG_MESSAGE("  CWnd::GetLayer returned NULL — wnd has no layer yet; aborting");
        return;
    }

    // get_z on the parent — used to derive each child's absolute z.
    long parentZ = 0;
    try {
        parentZ = pParentLayer->z;
    } catch (const _com_error& e) {
        DEBUG_MESSAGE("  parentLayer->z threw HRESULT 0x%08X (%s)",
                      static_cast<unsigned>(e.Error()), e.ErrorMessage());
        return;
    }
    DEBUG_MESSAGE("  parentLayer=0x%08X parentZ=%ld",
                  pParentLayer.GetInterfacePtr(), parentZ);

    // _g_gr at v95 0x00C6F430 — the global IWzGr2D singleton. Null if COM
    // graphics not yet initialised (shouldn't happen this late in startup).
    IWzGr2DPtr& pGr = get_gr();
    if (!pGr) {
        DEBUG_MESSAGE("  get_gr() (0x00C6F430) is NULL; aborting");
        return;
    }
    DEBUG_MESSAGE("  _g_gr=0x%08X", pGr.GetInterfacePtr());

    // VARIANT wrap of the parent layer — passed to put_origin and put_overlay.
    // Ztl_variant_t(IUnknown*) AddRefs by default, so vParent owns one ref;
    // dtor calls VariantClear which Releases.
    Ztl_variant_t vParent(static_cast<IUnknown*>(pParentLayer.GetInterfacePtr()));
    // KMST passes (long)0 with VT_I4 as the dwFilter (last arg of CreateLayer).
    Ztl_variant_t vFilter(0L, VT_I4);

    struct LayerSpec {
        const char*   tag;
        size_t        dirtyOffset;     // v95 offset of the dirty flag
        size_t        slotOffset;      // v95 offset of the IWzGr2DLayer* slot
        long          layerLeft;
        long          layerTop;
        long          zOffset;         // added to parentZ
        unsigned long canvasWidth;
        unsigned long canvasHeight;
    };

    // KMST build order: [0], [2], [1]. Each entry's offsets are v95-side
    // (= KMST + 0x6A0). dirtyOffset is the int32_t flag UpdateUI re-arms;
    // slotOffset is dirtyOffset+4 (the COM-ptr storage).
    static const LayerSpec kLayers[] = {
        { "L0", 0x15A0, 0x15A4, 0x28, 0x19, 1, 0xAE, 0x100 },
        { "L2", 0x15B0, 0x15B4, 0x28, 0x19, 2, 0xAE, 0x100 },
        { "L1", 0x15A8, 0x15AC, 0xF0, 0x14, 2, 0xDC, 0x122 },
    };

    for (const auto& spec : kLayers) {
        DEBUG_MESSAGE("  [%s] begin: pos=(%ld,%ld) zDelta=%ld canvas=%lux%lu",
                      spec.tag, spec.layerLeft, spec.layerTop, spec.zOffset,
                      spec.canvasWidth, spec.canvasHeight);

        // Step 1: pGr->CreateLayer(left, top, w=0, h=0, z=0, vCanvas=vtEmpty, dwFilter=0L)
        //
        // KMST calls width=0 height=0 — the layer auto-sizes to its inserted
        // canvas. left/top are the layer's offset within the parent.
        IWzGr2DLayerPtr pNewLayer;
        try {
            pNewLayer = pGr->CreateLayer(
                spec.layerLeft, spec.layerTop,
                /*uWidth=*/0, /*uHeight=*/0,
                /*nZ=*/0, vtEmpty, vFilter);
        } catch (const _com_error& e) {
            DEBUG_MESSAGE("    pGr->CreateLayer threw HRESULT 0x%08X (%s)",
                          static_cast<unsigned>(e.Error()), e.ErrorMessage());
            return;
        }
        if (!pNewLayer) {
            DEBUG_MESSAGE("    pGr->CreateLayer returned NULL");
            return;
        }
        DEBUG_MESSAGE("    CreateLayer ok: pNewLayer=0x%08X",
                      pNewLayer.GetInterfacePtr());

        // Step 2: m_aLayer[i].pLayer = pNewLayer. The slot is a raw COM ptr
        // (4 bytes) — emulate _com_ptr_t::operator=(p) by AddRef'ing the new
        // ref before storing. Slot starts at nullptr (memset 0 in UI_Open).
        IWzGr2DLayer* pRaw = pNewLayer.GetInterfacePtr();
        pRaw->AddRef();
        auto** pSlot = reinterpret_cast<IWzGr2DLayer**>(pBytes + spec.slotOffset);
        *pSlot = pRaw;

        // Steps 3-6: set parent-relative properties.
        try {
            pNewLayer->origin = vParent;
            pNewLayer->overlay = vParent;
            pNewLayer->color = 0xFFFFFFFFUL;
            pNewLayer->z = parentZ + spec.zOffset;
        } catch (const _com_error& e) {
            DEBUG_MESSAGE("    layer property setters threw HRESULT 0x%08X (%s)",
                          static_cast<unsigned>(e.Error()), e.ErrorMessage());
            return;
        }
        DEBUG_MESSAGE("    layer configured: origin/overlay=parent color=0xFFFFFFFF z=%ld",
                      parentZ + spec.zOffset);

        // Step 7: PcCreateObject<IWzCanvasPtr>(L"Canvas", &out, NULL). Class
        // name verified via KMST asm — StringPool key 0x40A maps to L"Canvas".
        IWzCanvasPtr pCanvas;
        try {
            PcCreateObject<IWzCanvasPtr>(L"Canvas", pCanvas, nullptr);
        } catch (const _com_error& e) {
            DEBUG_MESSAGE("    PcCreateObject(L\"Canvas\") threw HRESULT 0x%08X (%s)",
                          static_cast<unsigned>(e.Error()), e.ErrorMessage());
            return;
        }
        if (!pCanvas) {
            DEBUG_MESSAGE("    PcCreateObject returned NULL canvas");
            return;
        }
        DEBUG_MESSAGE("    PcCreateObject ok: pCanvas=0x%08X",
                      pCanvas.GetInterfacePtr());

        // Step 8: canvas->Create(W, H) allocs the backing buffer; cx/cy=0
        // anchors the canvas origin at (0,0) within its layer.
        try {
            pCanvas->Create(spec.canvasWidth, spec.canvasHeight);
            pCanvas->cx = 0;
            pCanvas->cy = 0;
        } catch (const _com_error& e) {
            DEBUG_MESSAGE("    canvas->Create / cx / cy threw HRESULT 0x%08X (%s)",
                          static_cast<unsigned>(e.Error()), e.ErrorMessage());
            return;
        }
        DEBUG_MESSAGE("    canvas Created %lux%lu cx=0 cy=0",
                      spec.canvasWidth, spec.canvasHeight);

        // Step 9: pNewLayer->InsertCanvas(pCanvas) — optional VARIANTs default
        // to vtEmpty (no delay, no alpha override, no zoom override). The layer
        // takes its own AddRef on the canvas, so our local pCanvas can release
        // when the loop iteration ends.
        try {
            pNewLayer->InsertCanvas(pCanvas);
        } catch (const _com_error& e) {
            DEBUG_MESSAGE("    InsertCanvas threw HRESULT 0x%08X (%s)",
                          static_cast<unsigned>(e.Error()), e.ErrorMessage());
            return;
        }

        // Set the dirty flag last. KMST does this BEFORE step 1 for block 1
        // and AFTER step 9 for blocks 2/3 — irrelevant in practice since the
        // layer isn't drawn until the next frame, by which time everything
        // has settled.
        *reinterpret_cast<int32_t*>(pBytes + spec.dirtyOffset) = 1;
        DEBUG_MESSAGE("    [%s] done: dirty@+0x%X=1", spec.tag,
                      static_cast<unsigned>(spec.dirtyOffset));
    }

    DEBUG_MESSAGE("MonsterBook_CreateLayer: all 3 layers built ok");

    // ----- Background layer (4th overlay) ---------------------------------
    //
    // The previous test confirmed parent-layer-direct-InsertCanvas does NOT
    // render even though the canvas resolved fine, parent visible=1, and
    // InsertCanvas didn't throw. Copying into a sub-layer's canvas DID
    // render (visible as a 174x256 chunk at L0's (40,25) origin). So the
    // parent layer is an overlay container that doesn't paint its own
    // canvases — we need a sub-layer.
    //
    // Add a 4th overlay layer at (0, 0) covering the full 475x349 wnd
    // client area, parented to pParentLayer with z=parent+0 (under the
    // 3 foreground sub-layers at z=parent+1/+2). Copy the WZ-resolved bg
    // pixels into the layer's canvas. Same pattern as the 3 KMST sub-
    // layers, just full-window-sized + with real pixel data via Copy.
    //
    // Lifetime: store in g_pBgLayer (file-scope smart-ptr, single
    // MonsterBook instance via DAT_00c6f010 gating). PreDestroy clears
    // it; next Construct overwrites (which Releases the previous via
    // operator=).
    DEBUG_MESSAGE("MonsterBook_CreateLayer: building bg overlay layer");
    try {
        IWzCanvasPtr pBgCanvas = get_unknown(get_rm()->GetObjectA(
            Ztl_bstr_t(L"UI/UIWindow.img/MonsterBook/backgrnd")));
        if (!pBgCanvas) {
            DEBUG_MESSAGE("  bg WZ canvas resolved to NULL — aborting bg-layer build");
        } else {
            DEBUG_MESSAGE("  bg WZ canvas: 0x%08X w=%lu h=%lu cx=%ld cy=%ld",
                          pBgCanvas.GetInterfacePtr(),
                          static_cast<unsigned long>(pBgCanvas->width),
                          static_cast<unsigned long>(pBgCanvas->height),
                          static_cast<long>(pBgCanvas->cx),
                          static_cast<long>(pBgCanvas->cy));

            const unsigned long bgWidth  = pBgCanvas->width;
            const unsigned long bgHeight = pBgCanvas->height;

            IWzGr2DLayerPtr pBgLayer = pGr->CreateLayer(
                /*nLeft=*/0, /*nTop=*/0,
                /*uWidth=*/0, /*uHeight=*/0,
                /*nZ=*/0, vtEmpty, vFilter);
            if (!pBgLayer) {
                DEBUG_MESSAGE("  bg-layer CreateLayer returned NULL");
            } else {
                DEBUG_MESSAGE("  bg-layer CreateLayer ok: 0x%08X",
                              pBgLayer.GetInterfacePtr());

                pBgLayer->origin = vParent;
                pBgLayer->overlay = vParent;
                pBgLayer->color = 0xFFFFFFFFUL;
                pBgLayer->z = parentZ;  // parent's z — under sub-layers
                DEBUG_MESSAGE("  bg-layer configured: origin/overlay=parent z=%ld",
                              parentZ);

                IWzCanvasPtr pBgLayerCanvas;
                PcCreateObject<IWzCanvasPtr>(L"Canvas", pBgLayerCanvas, nullptr);
                pBgLayerCanvas->Create(bgWidth, bgHeight);
                pBgLayerCanvas->cx = 0;
                pBgLayerCanvas->cy = 0;
                DEBUG_MESSAGE("  bg-layer canvas: 0x%08X created %lux%lu",
                              pBgLayerCanvas.GetInterfacePtr(),
                              bgWidth, bgHeight);

                pBgLayerCanvas->Copy(0, 0, pBgCanvas);
                DEBUG_MESSAGE("  bg-layer canvas Copy(0,0,bg) ok");

                pBgLayer->InsertCanvas(pBgLayerCanvas);
                DEBUG_MESSAGE("  bg-layer InsertCanvas ok");

                // Store ref-owning smart-ptr at file scope so the layer
                // outlives this function. operator= here Releases any
                // previous bg layer (e.g. from a prior open we didn't
                // PreDestroy cleanly).
                g_pBgLayer = pBgLayer;
                DEBUG_MESSAGE("  bg-layer stored in g_pBgLayer");
            }
        }
    } catch (const _com_error& e) {
        DEBUG_MESSAGE("  bg-layer build threw HRESULT 0x%08X (%s)",
                      static_cast<unsigned>(e.Error()), e.ErrorMessage());
    }
}

// KMST 0x0084988E (32 lines) — tools/decomp/cache_kmst/0084988e.c.
//
// Two flat RECT arrays for HitTest:
//   v95 +0x15B8: 25 RECTs (5x5 grid, cell size 0x21 x 0x2D, base (8, 0x1F),
//                rect size 0x1B x 0x26)  — the per-tab card grid hit zones.
//   v95 +0x1748: 20 RECTs (4x5 grid, cell size 0x24 x 0x24, base (0x26,
//                0x3C), rect size 0x20 x 0x20) — the cover-tab card grid
//                hit zones.
// KMST calls the user32!SetRect import via DAT_00be6724; we link directly
// against ::SetRect since <windows.h> is already in this TU.
static void MonsterBook_CreateRect(void* pThis) {
    DEBUG_MESSAGE("MonsterBook_CreateRect: enter pThis=0x%08X", pThis);
    auto* pBytes = static_cast<uint8_t*>(pThis);

    auto* pTabRects = reinterpret_cast<RECT*>(pBytes + 0x15B8);
    for (int i = 0; i < 25; ++i) {
        const int x = (i % 5) * 0x21;
        const int y = (i / 5) * 0x2D;
        ::SetRect(&pTabRects[i], x + 8, y + 0x1F, x + 0x23, y + 0x45);
    }
    DEBUG_MESSAGE("  tab rects @+0x15B8: 25 zones, first=(%ld,%ld,%ld,%ld) last=(%ld,%ld,%ld,%ld)",
                  pTabRects[0].left, pTabRects[0].top,
                  pTabRects[0].right, pTabRects[0].bottom,
                  pTabRects[24].left, pTabRects[24].top,
                  pTabRects[24].right, pTabRects[24].bottom);

    auto* pCoverRects = reinterpret_cast<RECT*>(pBytes + 0x1748);
    for (int i = 0; i < 20; ++i) {
        const int x = (i % 4) * 0x24;
        const int y = (i / 4) * 0x24;
        ::SetRect(&pCoverRects[i], x + 0x26, y + 0x3C, x + 0x46, y + 0x5C);
    }
    DEBUG_MESSAGE("  cover rects @+0x1748: 20 zones, first=(%ld,%ld,%ld,%ld) last=(%ld,%ld,%ld,%ld)",
                  pCoverRects[0].left, pCoverRects[0].top,
                  pCoverRects[0].right, pCoverRects[0].bottom,
                  pCoverRects[19].left, pCoverRects[19].top,
                  pCoverRects[19].right, pCoverRects[19].bottom);
}

// KMST 0x0084991F (73 lines) — tools/decomp/cache_kmst/0084991f.c.
// Allocates the 9-column ZArray<ZArray<ZRef<MonsterBookCard>>> at v95 +0x1888
// (= KMST +0x11E8) — backing store for all collected cards by tab/area.
//
// DEFERRED Phase 2-port-4: KMST's body iterates `CMonsterBookMan::GetCard(p,
// &out, tab, idx)` (the (long, long) overload at KMST 0x006823F1) to walk
// each tab's card list. v95 stripped that overload — the v95 names cache
// shows only the by-cardId GetCard at 0x00662930 (`?GetCard@CMonsterBookMan@@QBE
// ?AV?$ZRef@UMonsterBookCard@@@@J@Z`, single `J` arg). Without the (tab, idx)
// helper the population step has no enumeration source. v95's surviving
// CUIMonsterBook::GetCard_1 (0x00808FB0) reads `*(this+0x1888 + tabIdx*4)`;
// when the slot is zero (our memset state) it returns null cards and
// downstream LoadMobInfo / draw paths render no sprites. That is the
// current visual behaviour and it is non-crashing.
//
// To populate later we'd need to either find a v95-side enumeration helper
// (ZMap iteration via GetHeadPosition+GetNext on the cardId hashmap exists,
// but cards aren't tagged by tab in MonsterBookCard so that gives all-cards
// ungrouped), or rebuild the per-tab arrays inside CMonsterBookMan ourselves
// from WZ data at boot. Both paths are larger than this batch.
static void MonsterBook_CreateCardTable(void* /*pThis*/) {
    // No-op: zero-initialised buffer is a valid "no cards loaded" state.
    // GetCard_1 null-checks the per-tab slot before dereferencing, so empty
    // tables are safe.
}

// KMST 0x00849A25 (199 lines) — tools/decomp/cache_kmst/00849a25.c.
// Pre-loads IWzFont COM ptrs into the m_aFonts ZArray (+0xE64 v95 = KMST
// +0x7C4 — the ctor zeros this slot, the builder fills it).
//
// DEFERRED Phase 2-port-5: 8-slot IWzFont com_ptr array sourced from
// StringPool::GetStringW(<id>) → PcCreateObject for slots 0-1, then
// IWzFont::Create with StringPool::GetBSTR-derived font names + sizes
// for slots 2-7. Ghidra's signature inference loses the literal StringPool
// IDs (they show as stack-pointer casts because GetStringW's first int
// arg is misclassified). Without the IDs we can't replay the right
// StringPool keys against v95's pool, so the fonts would resolve wrong.
//
// Safe to leave empty until CreateLayer lands: the font array is read by
// LAYER draw paths to render labels; with no LAYERs constructed nothing
// reads from this slot.
static void MonsterBook_CreateFontArray(void* /*pThis*/) {
    // TODO Phase 2-port-5.
}


// === Lifecycle ==============================================================

// Mirrors KMST CUIMonsterBook::CUIMonsterBook + OnCreate. Run on a fresh
// kCUIMonsterBookSize buffer.
static void MonsterBook_Construct(void* pThis) {
    DEBUG_MESSAGE("MonsterBook_Construct: enter pThis=0x%08X size=0x%X",
                  pThis, static_cast<unsigned>(kCUIMonsterBookSize));

    // CUIWnd::CUIWnd(this, 9, 0, 0, 0, 1, 0, 0) — args verbatim from KMST.
    DEBUG_MESSAGE("  -> CUIWnd::CUIWnd(this, 9, 0, 0, 0, 1, 0, 0) at 0x%08X",
                  static_cast<unsigned>(kCUIWnd_ctor));
    reinterpret_cast<void(__thiscall*)(void*, int, int, int, int, int, int, int)>(
        kCUIWnd_ctor)(pThis, 9, 0, 0, 0, 1, 0, 0);
    DEBUG_MESSAGE("  <- CUIWnd::CUIWnd ok");

    // CreateUIWndPosSaved(this, 0x1DB, 0x15D, 10) — 475x349 client area.
    DEBUG_MESSAGE("  -> CreateUIWndPosSaved(this, 0x1DB, 0x15D, 10) at 0x%08X",
                  static_cast<unsigned>(kCUIWnd_CreateUIWndPosSaved));
    reinterpret_cast<void(__thiscall*)(void*, int, int, int)>(
        kCUIWnd_CreateUIWndPosSaved)(pThis, 0x1DB, 0x15D, 10);
    DEBUG_MESSAGE("  <- CreateUIWndPosSaved ok");

    // KMST's OnCreate runs the five builders here, then calls
    // CMonsterBookMan::GetCard(cover) and either CCtrlTab::SetTab(9) +
    // SetRightTab(0) (no cover) or SelectCard(cover). All five builders are
    // stubs in Phase 2-sketch; the cover-card / tab-setup logic is also
    // skipped because it reads fields the builders own.
    MonsterBook_CreateCtrl(pThis);
    MonsterBook_CreateLayer(pThis);
    MonsterBook_CreateRect(pThis);
    MonsterBook_CreateCardTable(pThis);
    MonsterBook_CreateFontArray(pThis);
    DEBUG_MESSAGE("MonsterBook_Construct: builders done");
}

// Pre-destroy hook — runs in our UI_Close hook BEFORE we tail-call v95's
// stock UI_Close. v95's case-9 close path runs Release → virtual dtor →
// ~CUIWnd → ~CWnd which cleans the CUIWnd-base members (m_pBtClose at
// +0x80, m_uiToolTip, m_abOption, m_sBackgrndUOL) and unregisters the
// window from the wnd manager. It does NOT know about derived members,
// so any ZRef / heap allocations the builders added need to be released
// here:
//   - 5 nav buttons populated by CreateCtrl at +0x1578..+0x1598
//     (m_pBtClose at +0x80 belongs to the base — leave it for ~CUIWnd).
//   - 3 IWzGr2DLayer raw COM ptrs populated by CreateLayer at
//     +0x15A4 / +0x15AC / +0x15B4. Each holds one ref via our manual
//     AddRef in CreateLayer; Release here drops it to zero and the
//     COM dtor reclaims the layer + its inserted canvas.
//
// Setting each ZRef<CCtrlButton> slot to nullptr triggers ZRef::operator=
// (nullptr_t) which Releases the underlying button. The IWzGr2DLayer slots
// are raw COM ptrs (not ZRef) so we Release manually.
//
// Do NOT s_Free the buffer here — v95's scalar_deleting_dtor (reached
// through the vtable Release call) does that, and an extra s_Free here
// would double-free.
static void MonsterBook_PreDestroy(void* pThis) {
    DEBUG_MESSAGE("MonsterBook_PreDestroy: enter pThis=0x%08X", pThis);
    auto* pBytes = static_cast<uint8_t*>(pThis);

    // Release the file-scope bg-layer first — it's parented to the wnd's
    // parent layer via overlay, so dropping our ref now Disconnects it
    // before the wnd dtor tears down the parent.
    if (g_pBgLayer) {
        DEBUG_MESSAGE("  releasing g_pBgLayer ptr=0x%08X",
                      g_pBgLayer.GetInterfacePtr());
        g_pBgLayer = nullptr;
    }

    static constexpr size_t kLayerSlotOffsets[] = { 0x15A4, 0x15AC, 0x15B4 };
    for (auto offset : kLayerSlotOffsets) {
        auto** pSlot = reinterpret_cast<IWzGr2DLayer**>(pBytes + offset);
        if (*pSlot != nullptr) {
            DEBUG_MESSAGE("  releasing IWzGr2DLayer @+0x%X ptr=0x%08X",
                          static_cast<unsigned>(offset), *pSlot);
            (*pSlot)->Release();
            *pSlot = nullptr;
        }
    }

    static constexpr size_t kNavBtnSlotOffsets[] = {
        0x1578, 0x1580, 0x1588, 0x1590, 0x1598,
    };
    for (auto offset : kNavBtnSlotOffsets) {
        auto* pSlot = reinterpret_cast<ZRef<CCtrlButton>*>(pBytes + offset);
        DEBUG_MESSAGE("  releasing ZRef<CCtrlButton> @+0x%X",
                      static_cast<unsigned>(offset));
        *pSlot = nullptr;
    }
    DEBUG_MESSAGE("MonsterBook_PreDestroy: done");
}


// === Hooks ==================================================================

static auto CWvsContext__UI_Open = kCWvsContext_UI_Open;
static auto CWvsContext__UI_Close = kCWvsContext_UI_Close;

void __fastcall CWvsContext__UI_Open_hook(void* pCtx, void* /*EDX*/,
                                          int nUIType, int nOption) {
    if (nUIType == 9) {  // MONSTERBOOK — case 9 stripped from v95 UI_Open.
        DEBUG_MESSAGE("UI_Open(9, %d): hook fired pCtx=0x%08X", nOption, pCtx);

        auto** pSlot = reinterpret_cast<void**>(
            static_cast<uint8_t*>(pCtx) + kCWvsContext_MonsterBookSlotPtr_Off);
        if (*pSlot != nullptr) {
            // Slot still set — window is already open. v95's other cases
            // gate the same way; matching the convention.
            DEBUG_MESSAGE("  already open: pSlot=0x%08X *pSlot=0x%08X — bailing",
                          pSlot, *pSlot);
            return;
        }
        void* pBuf = ZAllocEx<ZAllocAnonSelector>::s_Alloc(kCUIMonsterBookSize);
        if (!pBuf) {
            DEBUG_MESSAGE("  s_Alloc(0x%X) failed",
                          static_cast<unsigned>(kCUIMonsterBookSize));
            return;
        }
        DEBUG_MESSAGE("  s_Alloc(0x%X) -> 0x%08X",
                      static_cast<unsigned>(kCUIMonsterBookSize), pBuf);
        std::memset(pBuf, 0, kCUIMonsterBookSize);
        MonsterBook_Construct(pBuf);

        // Hook the window into v95's canonical close machinery: bump the
        // ZRefCounted refcount to 1 (mirroring what FUN_009d1750 / sibling
        // setters do for non-stripped cases) then write the pointer into
        // the case-9 slot at CWvsContext+0x3EB4. v95's stock UI_Close
        // case 9 will read this slot, run FUN_009b0e50 (no-op for us
        // since m_dwWndKey stays 0) and FUN_009d1ae0 to clear the slot.
        // The slot-clear decrements our refcount back to 0 and fires the
        // virtual dtor through the v95-stamped vtable, which removes the
        // window from the wnd manager and frees the buffer.
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(
            static_cast<uint8_t*>(pBuf) + kZRefCounted_RefField_Off));
        *pSlot = pBuf;
        g_pV95MonsterBookGlobal = pBuf;
        g_pMonsterBookUI = pBuf;
        DEBUG_MESSAGE("UI_Open(9): wired up — pBuf=0x%08X slot@0x%08X DAT_00c6f010=0x%08X",
                      pBuf, pSlot, pBuf);
        return;
    }
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(CWvsContext__UI_Open)(
        pCtx, nUIType, nOption);
}

void __fastcall CWvsContext__UI_Close_hook(void* pCtx, void* /*EDX*/, int nUIType) {
    if (nUIType == 9) {  // MONSTERBOOK
        // Read the live slot rather than g_pMonsterBookUI: v95 may have
        // cleared the latter through paths we don't observe, but the slot
        // is the source of truth that v95's case-9 close path reads next.
        auto** pSlot = reinterpret_cast<void**>(
            static_cast<uint8_t*>(pCtx) + kCWvsContext_MonsterBookSlotPtr_Off);
        void* pUI = *pSlot;
        DEBUG_MESSAGE("UI_Close(9): hook fired pCtx=0x%08X *slot=0x%08X g_pUI=0x%08X",
                      pCtx, pUI, g_pMonsterBookUI);

        // Release builder-allocated members BEFORE the v95 dtor path runs.
        // ~CUIWnd doesn't know about derived members (nav buttons, layers).
        if (pUI != nullptr) {
            MonsterBook_PreDestroy(pUI);
        }

        // Clear DAT_00c6f010 BEFORE the slot-clear runs the dtor — once
        // the buffer is freed, OnMonsterBookSetCard / SetCover would
        // deref freed memory if the global still pointed at it. Local
        // sentinel cleared too so the next /book sees a clean state.
        g_pV95MonsterBookGlobal = nullptr;
        g_pMonsterBookUI = nullptr;
        DEBUG_MESSAGE("UI_Close(9): pre-destroy done; tail-calling v95 close");
    }
    // Tail-call: v95's case-9 close reads the slot we populated, runs
    // FUN_009b0e50 + FUN_009d1ae0, fires Release → virtual dtor → ~CUIWnd
    // → ~CWnd unregisters from the wnd manager. The window vanishes.
    reinterpret_cast<void(__thiscall*)(void*, int)>(CWvsContext__UI_Close)(
        pCtx, nUIType);
}


void AttachUIMonsterBook() {
    ATTACH_HOOK(CWvsContext__UI_Open, CWvsContext__UI_Open_hook);
    ATTACH_HOOK(CWvsContext__UI_Close, CWvsContext__UI_Close_hook);
}
