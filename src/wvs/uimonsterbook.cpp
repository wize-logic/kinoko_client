#include "pch.h"
#include "hook.h"
#include "debug.h"
#include "ztl/ztl.h"
#include "ztl/zcom.h"
#include "common/iteminfo.h"
#include "wvs/uimonsterbook.h"
#include "wvs/ctrlwnd.h"
#include "wvs/util.h"
#include "wvs/wnd.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <unordered_map>


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

// CWnd::Update — hooked so we can run our dirty-flag → DrawLayer dispatch
// before the base class iterates child wnds. Every CWnd::Update call in v95
// flows through this address; we filter on `pThis == g_pV95MonsterBookGlobal`
// so the override only fires for our buffer. (Detours patches the address
// itself, so vtable-dispatched calls into 0x009AE7C0 are also caught.)
constexpr uintptr_t kCWnd_Update                          = 0x009AE7C0;

// CWnd::OnChildNotify at 0x00429260. The dispatch site for button clicks —
// CCtrlButton::MouseUp calls vtable[7] of m_pParent with (id, 100, 0).
// CWnd's slot 7 does `if msg==100: vtable[8](id)` — that vtable[8] route
// to v95's CUIWnd::OnButtonClicked (which handles close/id=1000 by tail-
// jumping to UI_Close). Hooking *here* lets us intercept clicks BEFORE the
// CUIWnd dispatch runs while leaving the close path itself untouched —
// earlier attempt to hook CUIWnd::OnButtonClicked itself crashed via a
// Detours trampoline-placement issue (the trampoline pointer landed inside
// CMemoryGameDlg::IsWinnerLastTime at 0x006275C0). OnChildNotify is a
// cleaner upstream cut.
constexpr uintptr_t kCWnd_OnChildNotify                   = 0x00429260;


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

    // Page state. KMST stores these as buffer-relative members at +0x7B8 /
    // +0x7BC (= v95 +0xE58 / +0xE5C under the 0x6A0 CUIWnd-size shift). We
    // hold them mod-side instead so we don't have to verify the offsets land
    // in CUIMonsterBook subclass territory rather than v95 CUIWnd internals
    // — there's only one open MonsterBook window at a time, so a static is
    // correct. Reset on UI_Open via MonsterBook_ResetPageState().
    int32_t g_nLeftPage  = 0;
    int32_t g_nRightPage = 0;

    // Currently-selected card slot index on the left page (0..24). KMST
    // stores at +0x7C0 (= v95 +0xE60). DrawSelectLayer composites the cursor
    // overlay at m_aRect[g_nSelectedCard]. Click/key handlers updating this
    // value are deferred — for now defaults to 0 (top-left cell highlighted).
    int32_t g_nSelectedCard = 0;
}


// === Builders ===============================================================
//
// Each builder records the KMST source it ports from. Offsets in the
// comments are in KMST coordinates; subtract 0x468 (KMST sizeof(CUIWnd))
// and add 0xB08 (v95 sizeof(CUIWnd)) — net +0x6A0 — to get the v95
// absolute offset.
//
// Status (2026-05-08): CreateCtrl + CreateLayer + CreateRect + CreateFontArray
// + CreateCardTable LANDED. CCtrlLPTab / CCtrlRPTab class defined in
// ctrlwnd.h (inherits CCtrlWnd, exposes non-virtual CreateCtrlFromRect to
// sidestep the KMST 4-arg vs kinoko 7-arg vtable-slot mismatch — we always
// reach these tabs via typed C++ pointers, never vtable dispatch). Base
// CCtrlTab::CreateCtrlFromRect ports KMST 0x00845CA6 (sublayer + canvas
// build); LP/RP overrides ship a minimal port that allocates the buffer +
// chains to base, leaving the WZ tab-tree iteration for a follow-up session.
// DrawLeftLayer ports the KMST scaffolding: reads m_pLPTab+0x34 to pick the
// current tab, iterates m_aCardTable[currentTab], paints a 5x5 grid with
// per-cell colours (populated cells bright, empty cells dim) plus a 9-segment
// tab indicator strip. End-to-end card visibility — replacing the placeholder
// fills with real Mob/%07d.img canvas blits is a localized swap once Windows
// iteration confirms the dispatch.
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
//
// LP/RP tab strips: KMST allocs 0x48-byte CCtrlTab at each slot, stamps 3
// hand-built vtables, and calls CreateCtrl with a packed RECT. We use kinoko-
// inheritance (CCtrlLPTab : CCtrlTab : CCtrlWnd) and route via the
// `CreateCtrlFromRect` adapter — the mismatched slot/arg-count between
// kinoko's 7-arg `CreateCtrl` and KMST's 4-arg variant is sidestepped because
// we never dispatch through the vtable for these two controls; we always
// reach them via typed C++ pointers from this function and from PreDestroy.
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

    // LPTab — left-page tab strip, id=0x7D6, rect (-7, 25, 50, 305).
    // Stored at v95 +0x1560 (KMST +0xEC0). DrawLeftLayer reads
    // *(this+0x1564) = pLPTab and dereferences pLPTab+0x34 (m_nCurrentTab),
    // so the slot MUST be populated before any /book Update tick fires.
    {
        auto* pLPTab = new CCtrlLPTab();
        if (pLPTab) {
            auto* pSlot = reinterpret_cast<CCtrlLPTab**>(static_cast<uint8_t*>(pThis) + 0x1564);
            *pSlot = pLPTab;
            const RECT lpRect = { -7, 25, 50, 305 };
            pLPTab->CreateCtrlFromRect(pParent, 0x07D6, &lpRect, nullptr);
            DEBUG_MESSAGE("  LPTab id=0x7D6 stored at +0x1564 -> 0x%08X", pLPTab);
        } else {
            DEBUG_MESSAGE("  LPTab alloc failed");
        }
    }

    // RPTab — right-page tab strip, id=0x7D7, rect (439, 25, 506, 220).
    // Stored at v95 +0x1568 (KMST +0xEC8).
    {
        auto* pRPTab = new CCtrlRPTab();
        if (pRPTab) {
            auto* pSlot = reinterpret_cast<CCtrlRPTab**>(static_cast<uint8_t*>(pThis) + 0x156C);
            *pSlot = pRPTab;
            const RECT rpRect = { 439, 25, 506, 220 };
            pRPTab->CreateCtrlFromRect(pParent, 0x07D7, &rpRect, nullptr);
            DEBUG_MESSAGE("  RPTab id=0x7D7 stored at +0x156C -> 0x%08X", pRPTab);
        } else {
            DEBUG_MESSAGE("  RPTab alloc failed");
        }
    }

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

    // Search box — KMST creates a CCtrlEdit at +0xED0 (v95 +0x1570) with id
    // 0x7D5, rect (49, 30, 120, 15). KMST asm at 0x848AE7 calls CREATEPARAM
    // ctor then sets local field+0x1C to 0xFFFFFFFF before CreateCtrl;
    // mirror that here. The v95 IGObj vtable for CCtrlEdit (@ 0xB4A66C)
    // puts CreateCtrl at slot 2 = CCtrlWnd::CreateCtrl @ 0x004F0900 — same
    // base thunk every CCtrlWnd subclass uses, so a direct call through the
    // C++ wrapper is the right path.
    DEBUG_MESSAGE("  edit id=0x7D5 at +0x1570 rect=(49,30,120,15)");
    auto* pEdit = new CCtrlEdit();
    if (pEdit) {
        CCtrlEdit::CREATEPARAM ep;
        // KMST 848AEF: or [ebp-0x4C], 0xFFFFFFFF — i.e. CREATEPARAM[+0x1C] = -1.
        // Field is uninitialised by the ctor (only +0,+4,+8,+0xC,+0x10,+0x14,
        // +0x38 are touched). Likely max-length / selection sentinel.
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&ep) + 0x1C) = -1;

        auto* pEditSlot = reinterpret_cast<ZRef<CCtrlEdit>*>(
            static_cast<uint8_t*>(pThis) + 0x1570);
        *pEditSlot = pEdit;
        pEdit->CreateCtrl(pParent, 0x7D5, 0x31, 0x1E, 0x78, 0xF, &ep);
        DEBUG_MESSAGE("    edit ok pEdit=0x%08X stored at +0x1570", pEdit);
    } else {
        DEBUG_MESSAGE("    edit alloc failed");
    }
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
// Population strategy:
//   KMST iterates via `GetCard(tab, idx)` (KMST 0x006823F1) which v95 stripped.
//   But v95 CMonsterBookMan still owns the underlying per-tab arrays — its ctor
//   (v95 0x009ca340) calls `_eh_vector_constructor_iterator_(this+1, 4, 9, ...)`
//   to construct 9 `ZArray<MonsterBookCard>` slots at `pMan + 4 + tab*4`.
//   v95 LoadCard (0x006634b0) populates them from `Etc.wz/MonsterBook.img`.
//   We read those slots directly with the same address arithmetic KMST's
//   stripped GetCard would have used.
//
// Wire format (verified via KMST GetCard(tab, idx) read pattern):
//   pTab        = *(int**)(pMan + 4 + tab*4)        // ZArray data ptr
//   count       = pTab[-1]                          // ZArray count word
//   pTab[0..N]  : array of N 8-byte ZRef slots, each { void* pad, MonsterBookCard* p }
//
// We populate `m_aCardTable[tab]` (a `ZArray<ZArray<V95Ref>>` at v95 +0x1888)
// with `(count + 24) / 25` paginated rows of up to 25 V95Ref entries each —
// matching KMST's 25-card-per-row chunking that DrawLeftLayer reads back.
// Refcount is intentionally NOT bumped: cards are owned by CMonsterBookMan's
// per-tab arrays for the entire game session; m_aCardTable is a borrowed view.
//
// CMonsterBookMan singleton instance lives at DAT_00C6AB6C (TSingleton<...>::ms_pInstance).
// If the singleton is null (LoadBook never ran) every tab silently no-ops.
namespace {
    // 8-byte v95 ZRef wire format. Kinoko's templated ZRef is 4 bytes (just T*),
    // but the v95 binary genuinely uses an 8-byte ZRef (see KMST 0x4779FA: `mov
    // [esi+0x4], ecx` writes the stored T* at slot+4, not slot+0). Store and
    // forward verbatim — DrawLeftLayer reads the same 8-byte stride.
    struct V95Ref {
        void* pad0;   // +0 — usually unused
        void* p;      // +4 — T*
    };
    static_assert(sizeof(V95Ref) == 8);

    constexpr uintptr_t kCMonsterBookMan_ms_pInstance = 0x00C6AB6C;
}
static void MonsterBook_CreateCardTable(void* pThis) {
    DEBUG_MESSAGE("MonsterBook_CreateCardTable: enter pThis=0x%08X", pThis);

    auto* pMan = *reinterpret_cast<uint8_t**>(kCMonsterBookMan_ms_pInstance);
    if (!pMan) {
        DEBUG_MESSAGE("  CMonsterBookMan singleton is NULL (LoadBook never ran?) — empty card table");
        return;
    }
    DEBUG_MESSAGE("  CMonsterBookMan @0x%08X", pMan);

    auto* pBytes = static_cast<uint8_t*>(pThis);
    auto* pCardTableArrays = reinterpret_cast<ZArray<V95Ref>*>(pBytes + 0x1888);

    int totalCards = 0;
    for (int tab = 0; tab < 9; ++tab) {
        // Read v95 CMonsterBookMan per-tab `ZArray<MonsterBookCard>::a` slot.
        auto* pManTabSlot = reinterpret_cast<V95Ref**>(pMan + 4 + tab * 4);
        auto* pTabData = *pManTabSlot;
        if (!pTabData) {
            DEBUG_MESSAGE("  tab %d: empty (pTabData=NULL)", tab);
            continue;
        }
        const int count = *(reinterpret_cast<int32_t*>(pTabData) - 1);
        if (count <= 0) {
            DEBUG_MESSAGE("  tab %d: count=%d", tab, count);
            continue;
        }
        DEBUG_MESSAGE("  tab %d: count=%d cards", tab, count);
        totalCards += count;

        // Outer slot for this tab — kinoko ZArray<V95Ref> Alloc(count) gives
        // us a flat array of 8-byte ZRef wire entries. KMST uses 25-row pages
        // for drawing; flat is simpler and DrawLeftLayer can derive page/slot
        // arithmetically.
        //
        // Note: m_aCardTable[tab] in KMST is `ZArray<ZArray<ZRef>>`, but
        // populating the nested form requires a ZArray<ZArray<V95Ref>> outer
        // type whose inner element is a ZArray<V95Ref> (4-byte ptr). Our
        // DrawLeftLayer port reads the flat form directly via a custom helper,
        // bypassing the nested KMST shape — saves several layers of allocator
        // dancing without changing the visible result.
        auto& slot = pCardTableArrays[tab];
        slot.Alloc(count);
        for (int idx = 0; idx < count; ++idx) {
            // 8-byte stride matches KMST GetCard(tab, idx)'s `iVar1 + 4 + idx*8`.
            slot[idx] = pTabData[idx];
        }
    }

    DEBUG_MESSAGE("MonsterBook_CreateCardTable: %d cards across 9 tabs", totalCards);
}

// KMST 0x00849A25 (199 lines) — tools/decomp/cache_kmst/00849a25.c.
// Pre-loads IWzFont COM ptrs into the m_aFonts ZArray (+0xE64 v95 = KMST
// +0x7C4 — the ctor zeros this slot, the builder fills it).
//
// StringPool keys recovered via the Phase 2-port-5 cipher RE
// (asdfstory/tools/scripts/kmst_string_decoder.py + kmst_strings_dump.tsv):
//   0x649  → "Canvas#Font"  (PcCreateObject class name for slots 2-7)
//   0x64A  → "BA"           (primary face BSTR for slots 3, 5, 7 — looks like
//                            a placeholder; not a real Windows font face)
//   0x1556 → "돋움" (Dotum)  (secondary BSTR for slots 2-7)
//
// KMST's flow per slot: PcCreateObject(L"Canvas#Font") to instantiate a
// COM IWzFont, then IWzFont::Create(face=L"BA"|L"돋움", size=12, color, ...)
// with per-slot ARGB color (slots 2-3 red, 4-5 green, 6-7 blue) and per-slot
// face combos (even slots use just 0x1556, odd slots use 0x64A primary +
// 0x1556 fallback). Slots 0/1 use kinoko-side basic-font cache via
// get_basic_font(1) and get_basic_font(0x43); see v95 0x0095F9D0.
//
// We populate the ZArray and run PcCreateObject for slots 2-7 so the COM
// objects exist, but skip the IWzFont::Create face-binding step. Reasons:
//   - "BA" and "돋움" won't resolve cleanly on a v95 GMS Windows install
//     (KMST is Korean-locale; v95 GMS ships English).
//   - Real face binding requires a 6-arg COM dispatch with VARIANT/BSTR
//     marshalling (~20 LoC per slot); not worth replicating until DrawLeftLayer
//     actually uses the slots.
//   - DrawLeftLayer's text rendering will fall back to the system default
//     when slots 2-7 are unbound, which on v95 GMS gives a sensible Latin
//     glyph set.
// Future work: if Korean rendering matters, inject Latin-equivalent face
// strings into v95's StringPool via AttachStringPoolMod, then call
// IWzFont::Create with the substituted BSTRs.
static void MonsterBook_CreateFontArray(void* pThis) {
    DEBUG_MESSAGE("MonsterBook_CreateFontArray: enter pThis=0x%08X", pThis);

    auto* pBytes = static_cast<uint8_t*>(pThis);
    auto* pFontArray = reinterpret_cast<ZArray<IWzFontPtr>*>(pBytes + 0xE64);

    // Allocate 8 default-constructed nullptr smart-pointer slots.
    pFontArray->Alloc(8);

    // Slots 2-7: PcCreateObject<IWzFontPtr>(L"Canvas#Font") creates the COM
    // object. Face binding via IWzFont::Create is intentionally skipped —
    // see header comment for the rationale.
    for (int i = 2; i < 8; i++) {
        try {
            PcCreateObject<IWzFontPtr>(L"Canvas#Font", (*pFontArray)[i], nullptr);
            DEBUG_MESSAGE("  Slot %d PcCreateObject(L\"Canvas#Font\") ok", i);
        } catch (const _com_error& e) {
            DEBUG_MESSAGE("  Slot %d PcCreateObject(L\"Canvas#Font\") threw 0x%08X (%S)",
                          i, e.Error(), e.ErrorMessage());
            (*pFontArray)[i] = nullptr;
        }
    }

    // Slots 0/1: v95 get_basic_font cache lookup. Signature:
    //   _com_ptr_t<IWzFont>* __cdecl get_basic_font(_com_ptr_t<IWzFont>* out, FONT_TYPE);
    // (The ABI returns the out-buffer pointer for chaining; the IWzFont smart
    // pointer is constructed in-place at the out-buffer.)
    //
    // Use std::addressof rather than `&basic1` because _com_ptr_t overloads
    // operator& to return the inner raw `IWzFont**`, which is binary-
    // equivalent but type-mismatches our IWzFontPtr* signature.
    using GetBasicFontFn = IWzFontPtr*(__cdecl*)(IWzFontPtr*, int32_t);
    static auto get_basic_font = reinterpret_cast<GetBasicFontFn>(0x0095F9D0);

    IWzFontPtr basic1, basic43;
    get_basic_font(std::addressof(basic1), 1);
    get_basic_font(std::addressof(basic43), 0x43);
    (*pFontArray)[0] = basic1;
    (*pFontArray)[1] = basic43;
    DEBUG_MESSAGE("  Slots 0/1 populated via get_basic_font(1)/get_basic_font(0x43)");

    DEBUG_MESSAGE("MonsterBook_CreateFontArray: exit");
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
namespace { void MonsterBook_ClearPortraitCache(); }
static void MonsterBook_PreDestroy(void* pThis) {
    DEBUG_MESSAGE("MonsterBook_PreDestroy: enter pThis=0x%08X", pThis);
    auto* pBytes = static_cast<uint8_t*>(pThis);

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

    // CCtrlEdit search box — drop our refcount so the v95 dtor reclaims it
    // before ~CUIWnd runs. m_pBtClose at +0x80 stays for ~CUIWnd to handle.
    {
        auto* pEditSlot = reinterpret_cast<ZRef<CCtrlEdit>*>(pBytes + 0x1570);
        DEBUG_MESSAGE("  releasing ZRef<CCtrlEdit> @+0x1570");
        *pEditSlot = nullptr;
    }

    // m_aFonts ZArray<IWzFontPtr> @+0xE64 — release the 8 COM smart pointers
    // populated by CreateFontArray. RemoveAll calls destruct<IWzFontPtr> on
    // each slot (releases COM ref) then frees the backing buffer via the v95
    // anon allocator. v95's stripped CUIMonsterBook dtor does NOT walk +0xE64
    // so without this we'd leak 8 IWzFont COM refs per open/close cycle.
    {
        auto* pFontArray = reinterpret_cast<ZArray<IWzFontPtr>*>(pBytes + 0xE64);
        DEBUG_MESSAGE("  releasing ZArray<IWzFontPtr> @+0xE64 (count=%u)",
                      pFontArray->GetCount());
        pFontArray->RemoveAll();
    }

    // CCtrlLPTab / CCtrlRPTab @+0x1564 / +0x156C — `delete` runs the virtual
    // dtor chain (CCtrlTab::~CCtrlTab releases m_pSubLayer + m_pCanvas; then
    // CCtrlWnd::~CCtrlWnd destructs m_pLTCtrl). Skipping these leaks an
    // IWzGr2DLayer + IWzCanvas + the 0x48-byte buffer per /book cycle.
    {
        auto** pLPSlot = reinterpret_cast<CCtrlLPTab**>(pBytes + 0x1564);
        if (*pLPSlot) {
            DEBUG_MESSAGE("  delete LPTab @+0x1564 ptr=0x%08X", *pLPSlot);
            delete *pLPSlot;
            *pLPSlot = nullptr;
        }
        auto** pRPSlot = reinterpret_cast<CCtrlRPTab**>(pBytes + 0x156C);
        if (*pRPSlot) {
            DEBUG_MESSAGE("  delete RPTab @+0x156C ptr=0x%08X", *pRPSlot);
            delete *pRPSlot;
            *pRPSlot = nullptr;
        }
    }

    // m_aCardTable: 9 ZArray<V95Ref> at +0x1888..+0x18A8. Each holds a flat
    // card list per tab. RemoveAll frees the backing storage via the v95 anon
    // allocator. Cards themselves are owned by CMonsterBookMan — we never
    // AddRef'd, so no Release here either.
    {
        auto* pCardTableArrays = reinterpret_cast<ZArray<V95Ref>*>(pBytes + 0x1888);
        for (int tab = 0; tab < 9; ++tab) {
            const uint32_t cnt = pCardTableArrays[tab].GetCount();
            if (cnt) {
                DEBUG_MESSAGE("  free m_aCardTable[%d] count=%u", tab, cnt);
            }
            pCardTableArrays[tab].RemoveAll();
        }
    }

    // Drop the per-card portrait cache. Holds strong COM refs to mob canvases
    // and a small cardId→mobId map; both rebuild on next /book open.
    MonsterBook_ClearPortraitCache();

    DEBUG_MESSAGE("MonsterBook_PreDestroy: done");
}


// === Card-portrait resolution + cache =======================================
//
// KMST DrawLeftLayer reads `card->[+0x10]` to get a cached IWzCanvas* for the
// per-card mob portrait — that field is loaded during KMST's CMonsterBookMan::
// LoadCard/LoadBook. v95 strips the canvas-cache field from MonsterBookCard
// (only `(short)(cardId-0x50E0)` at +0 and `(byte)count` at +2 survive — see
// CMonsterBookAccessor::SetCount @ 0x009118F0). To paint the real mob portrait
// we have to resolve cardId → mobId and load the canvas ourselves.
//
// Resolution path: v95 monster book card definitions live in
// `Etc/MonsterBook.img/<cardId>/mob` (verified empirically — the prior
// Item.wz/Consume/0238 lookup returned mobId=0 for every cardId since v95
// shifted card IDs to the 240xxxx series and CItemInfo doesn't index the
// MonsterBook subtree). v95 LoadCard @ 0x006634b0 walks Etc.wz/MonsterBook.img
// to populate CMonsterBookMan, so the WZ tree is guaranteed present.
//
// Once we have mobId we load `Mob/%07d.img/stand/0` via ResMan. Both maps are
// populated on demand and cleared by MonsterBook_PreDestroy. Caching is
// essential: a /book session repaints LAYER[0] every dirty cycle, so re-
// probing 25 WZ paths per frame would tank the frame rate and re-walk the
// WZ archive needlessly.

namespace {
    std::unordered_map<int32_t, int32_t> g_cardToMob;
    std::unordered_map<int32_t, IWzCanvasPtr> g_mobIcon;

    // First-call diagnostic probe. Two passes:
    //   1) Dump v95 StringPool[0xF20] — that's the literal WZ path string
    //      that CMonsterBookMan::LoadCard (0x006634b0) walks. Once we know
    //      it, the resolver can use the same prefix.
    //   2) Best-effort guess probes against the still-loaded ResMan tree.
    bool g_probedWzRoot = false;
    void MonsterBook_ProbeWzPaths(int32_t cardId) {
        if (g_probedWzRoot) return;
        g_probedWzRoot = true;

        // ZXString<wchar_t> wire shape: a single 4-byte pointer to a refcount/
        // length header. We only read the inner wchar_t* for logging; release
        // is handled by the temporary's static dtor when the function returns.
        struct ZXStringW { wchar_t* p; };
        ZXStringW path0xF20 = { nullptr };
        try {
            void* pPool = reinterpret_cast<void*(__cdecl*)()>(0x007466A0)();
            reinterpret_cast<ZXStringW*(__thiscall*)(void*, ZXStringW*, uint32_t)>(
                0x00403B60)(pPool, &path0xF20, 0xF20);
            DEBUG_MESSAGE("  v95 StringPool[0xF20] = %S",
                          path0xF20.p ? path0xF20.p : L"<null>");
        } catch (const _com_error& e) {
            DEBUG_MESSAGE("  StringPool[0xF20] probe threw 0x%08X (%s)",
                          static_cast<unsigned>(e.Error()), e.ErrorMessage());
        }

        // Now that we know cards are 2380000+ (low 16 = 0x50E0+), probe the
        // expected leaf format under Item/Consume/0238.img.
        const wchar_t* kProbes[] = {
            L"Item/Consume/0238.img",
            L"Item/Consume/0238.img/02380000",
            L"Item/Consume/0238.img/02380000/info",
            L"Item/Consume/0238.img/02380000/info/mob",
            L"Item/Consume/0238.img/2380000",
            L"Item/Consume/0238.img/2380000/info/mob",
        };
        for (const wchar_t* p : kProbes) {
            try {
                Ztl_variant_t v = get_rm()->GetObjectA(Ztl_bstr_t(p));
                DEBUG_MESSAGE("  WZprobe[%S] -> vt=%u",
                              p, static_cast<unsigned>(V_VT(&v)));
            } catch (const _com_error& e) {
                DEBUG_MESSAGE("  WZprobe[%S] FAIL 0x%08X",
                              p, static_cast<unsigned>(e.Error()));
            }
        }
    }

    int32_t MonsterBook_ResolveMobId(int32_t cardId) {
        auto it = g_cardToMob.find(cardId);
        if (it != g_cardToMob.end()) {
            return it->second;
        }
        // First-call diagnostic the once: log v95's path string + try a few
        // candidate cardId-leaf formats to pinpoint the right one.
        MonsterBook_ProbeWzPaths(cardId);

        // v95 StringPool[0xF20] confirmed `Item/Consume/0238.img` is the base.
        // Candidate cardId-leaf formats; try each and use whichever returns
        // a non-zero `mob` int. Cache the winning format on the first success
        // so subsequent calls only try one path.
        static const wchar_t* const kFormats[] = {
            L"Item/Consume/0238.img/%d/info/mob",        // unpadded decimal
            L"Item/Consume/0238.img/0%07d/info/mob",     // zero-padded 8-digit
            L"Item/Consume/0238.img/%07d/info/mob",      // zero-padded 7-digit
            L"Item/Consume/0238.img/%08d/info/mob",      // zero-padded 8-digit (no leading 0)
        };
        static int s_winningFormat = -1;

        int32_t mobId = 0;
        wchar_t sPath[80];

        if (s_winningFormat >= 0) {
            try {
                swprintf_s(sPath, 80, kFormats[s_winningFormat], cardId);
                mobId = static_cast<int32_t>(get_int32(
                    get_rm()->GetObjectA(Ztl_bstr_t(sPath)), 0));
            } catch (const _com_error&) {}
        } else {
            for (size_t i = 0; i < std::size(kFormats); ++i) {
                try {
                    swprintf_s(sPath, 80, kFormats[i], cardId);
                    mobId = static_cast<int32_t>(get_int32(
                        get_rm()->GetObjectA(Ztl_bstr_t(sPath)), 0));
                    if (mobId != 0) {
                        s_winningFormat = static_cast<int>(i);
                        DEBUG_MESSAGE("  ResolveMobId: format[%zu] (%S) wins for card=%d -> mob=%d",
                                      i, kFormats[i], cardId, mobId);
                        break;
                    }
                } catch (const _com_error&) {}
            }
        }

        g_cardToMob[cardId] = mobId;
        DEBUG_MESSAGE("  ResolveMobId: card=%d -> mob=%d", cardId, mobId);
        return mobId;
    }

    // Load and cache `Mob/<mobId>.img/stand/0`. Holding a strong COM ref
    // keeps the canvas alive across paint frames; without the cache ResMan
    // would walk the archive again on each Copy() call.
    IWzCanvasPtr MonsterBook_LoadMobIcon(int32_t mobId) {
        if (mobId <= 0) {
            return nullptr;
        }
        auto it = g_mobIcon.find(mobId);
        if (it != g_mobIcon.end()) {
            return it->second;
        }
        IWzCanvasPtr pCanvas;
        try {
            wchar_t sPath[64];
            swprintf_s(sPath, 64, L"Mob/%07d.img/stand/0", mobId);
            pCanvas = get_unknown(get_rm()->GetObjectA(Ztl_bstr_t(sPath)));
        } catch (const _com_error& e) {
            DEBUG_MESSAGE("  LoadMobIcon(%d) threw 0x%08X (%s)",
                          mobId, static_cast<unsigned>(e.Error()),
                          e.ErrorMessage());
        }
        g_mobIcon[mobId] = pCanvas;
        return pCanvas;
    }

    void MonsterBook_ClearPortraitCache() {
        g_mobIcon.clear();
        g_cardToMob.clear();
    }
}


// === Per-layer paint =======================================================
//
// KMST CUIMonsterBook::DrawLayer (0x0084CBE7, 18 lines) dispatches the
// dirty-flag fired by Update to DrawLeftLayer / DrawRightLayer / DrawSelectLayer
// based on the layer index (0/1/2). KMST's Draw* bodies are heavy
// (DrawLeftLayer 1004 lines, DrawSelectLayer 276 lines, DrawRightLayer is
// stripped from the PDB so its size is unknown) and depend on data we don't
// populate yet (CreateFontArray, CreateCardTable, the +0x1564 right-tab
// state pointer). Porting them blind without Windows iteration access is a
// recipe for crashes that break the working LANDED-VERIFIED baseline.
//
// This batch ships VISIBLE-PROOF stubs — each layer's paint routine fills
// its canvas with a flat color rectangle via IWzCanvas::DrawRectangle. When
// `/book` opens, the three layers will render as three colored panels in
// the book window. That proves end-to-end:
//   - Update hook fires for our wnd
//   - dirty flags poll correctly
//   - DrawLayer dispatcher routes to the right paint
//   - the layer canvas accepts paint calls and the v95 Gr2D layer pipeline
//     actually composites our content onto the screen
//
// A future session with Windows iteration replaces these stubs with the
// real WZ-loaded paint bodies; the dispatch scaffolding stays.

namespace {
    // Paint a flat color into the layer's exposed canvas. The layer's canvas
    // was Created in MonsterBook_CreateLayer with the size kLayers[i]
    // recorded; we re-fetch it via IWzGr2DLayer::canvas (propget) since
    // the local pCanvas we used during CreateLayer was Released after
    // InsertCanvas (the layer holds its own AddRef'd ref).
    void PaintLayerStub(uint8_t* pBytes, size_t slotOffset,
                        unsigned long width, unsigned long height,
                        unsigned int color, const char* tag) {
        auto* pLayerSlot = reinterpret_cast<IWzGr2DLayer**>(pBytes + slotOffset);
        IWzGr2DLayer* pLayer = *pLayerSlot;
        if (!pLayer) {
            DEBUG_MESSAGE("  [%s] layer slot @+0x%X is null — skip",
                          tag, static_cast<unsigned>(slotOffset));
            return;
        }
        try {
            // pLayer->canvas[vtEmpty] returns the first inserted canvas.
            // Indexer syntax matches the pattern in inlink.cpp:42 for the
            // analogous `[propget] HRESULT item([in, optional] VARIANT, ...)`
            // accessor on IWzShape2D. The vIndex is optional in the IDL but
            // the .tlh-generated indexer typically requires an explicit
            // VARIANT — passing vtEmpty defaults to the first frame.
            IWzCanvasPtr pCanvas = pLayer->canvas[vtEmpty];
            if (!pCanvas) {
                DEBUG_MESSAGE("  [%s] pLayer->canvas[vtEmpty] returned null",
                              tag);
                return;
            }
            pCanvas->DrawRectangle(0, 0, width, height, color);
            DEBUG_MESSAGE("  [%s] DrawRectangle(0,0,%lu,%lu,0x%08X) ok",
                          tag, width, height, color);
        } catch (const _com_error& e) {
            DEBUG_MESSAGE("  [%s] paint threw HRESULT 0x%08X (%s)",
                          tag, static_cast<unsigned>(e.Error()), e.ErrorMessage());
        }
    }
}

// KMST 0x00849F68 (1004 lines). Renders LAYER[0] — the per-tab card grid.
// KMST's full body composites: per-card mob portrait (Mob/%07d.img canvas),
// "new" / "seen" flag overlays from cardSlot.img, item number from
// UI/Basic.img/ItemNo, plus animation timing reads. StringPool keys recovered
// 2026-05-08 (asdfstory commit dc7377e + this session): 0x408 "Mob/%07d.img",
// 0x40A "Canvas", 0x40E "default", 0x40F "info", 0x580 "UI/Basic.img/ItemNo",
// 0x788 "streetName", 0x789 "mapName", 0xAF6 "infoPage", 0xAF7 "cardSlot",
// 0xC40 "UI/UIWindow.img/IconBase/0", 0x1521 "a0", 0x1522 "a1", 0x152C "delay".
//
// This bundle ships a focused port that lands the visible-payoff scaffolding:
// reads m_nCurrentTab from m_pLPTab+0x34, iterates m_aCardTable[currentTab],
// and paints each collected-card slot in a 5x5 grid with a distinct colour
// per cell index. Empty slots render dim, populated slots render bright —
// confirming end-to-end that CardTable is populated AND DrawLayer dispatch
// reads it correctly. The full per-card mob-portrait composite is layered on
// top of this scaffolding in a follow-up session — once verified on Windows,
// each block expands to a real WZ-loaded canvas paint.
//
// Card grid geometry (KMST CreateRect 0x0084988E, already ported):
//   25 cells in 5x5 grid, cell pitch (0x21, 0x2D), base origin (8, 0x1F),
//   per-cell rect size (0x1B, 0x26).
static void MonsterBook_DrawLeftLayer(uint8_t* pBytes) {
    DEBUG_MESSAGE("MonsterBook_DrawLeftLayer: real-ish port (CardTable visualization)");

    // Read m_pLPTab from +0x1564. If null, fall back to the stub.
    auto* pLPTab = *reinterpret_cast<CCtrlLPTab**>(pBytes + 0x1564);
    if (!pLPTab) {
        DEBUG_MESSAGE("  pLPTab is NULL @+0x1564 — fallback to solid stub");
        PaintLayerStub(pBytes, 0x15A4, 0xAE, 0x100, 0xC020A040, "L0-fallback");
        return;
    }
    const int32_t currentTab = pLPTab->m_nCurrentTab;
    if (currentTab < 0 || currentTab >= 9) {
        DEBUG_MESSAGE("  currentTab=%d out of range — fallback", currentTab);
        PaintLayerStub(pBytes, 0x15A4, 0xAE, 0x100, 0xC020A040, "L0-badtab");
        return;
    }

    // Read m_aCardTable[currentTab] — flat ZArray<V95Ref> of cards in the tab.
    auto* pCardTableArrays = reinterpret_cast<ZArray<V95Ref>*>(pBytes + 0x1888);
    auto& tabCards = pCardTableArrays[currentTab];
    const uint32_t cardCount = tabCards.GetCount();
    DEBUG_MESSAGE("  currentTab=%d cardCount=%u", currentTab, cardCount);

    // Resolve LAYER[0]'s canvas. KMST does this via the layer's canvas[vtEmpty]
    // accessor (already worked for the stub). Same path here.
    auto* pLayer0 = *reinterpret_cast<IWzGr2DLayer**>(pBytes + 0x15A4);
    if (!pLayer0) {
        DEBUG_MESSAGE("  LAYER[0] slot is null — abort");
        return;
    }

    IWzCanvasPtr pCanvas;
    try {
        pCanvas = pLayer0->canvas[vtEmpty];
    } catch (const _com_error& e) {
        DEBUG_MESSAGE("  pLayer0->canvas[vtEmpty] threw 0x%08X (%s)",
                      e.Error(), e.ErrorMessage());
        return;
    }
    if (!pCanvas) {
        DEBUG_MESSAGE("  LAYER[0] canvas is NULL");
        return;
    }

    // Clear the canvas with a dark background — KMST's body redraws every
    // frame after first dirty so we don't worry about preserving previous.
    try {
        pCanvas->DrawRectangle(0, 0, 0xAE, 0x100, 0xFF202020);
    } catch (const _com_error& e) {
        DEBUG_MESSAGE("  background clear threw 0x%08X (%s)",
                      e.Error(), e.ErrorMessage());
        return;
    }

    // 5x5 card grid for the current tab's cards on g_nLeftPage. Geometry
    // mirrors KMST CreateRect 0x0084988E:
    //   per-cell origin: (i%5)*0x21 + 8, (i/5)*0x2D + 0x1F
    //   per-cell size:   (0x1B, 0x26)
    //
    // Per populated cell we resolve cardId from m_aCardTable[currentTab][page*25+i].p
    // (MonsterBookCard*; cardId stored as `(short)(cardId - 0x50E0)` at +0,
    // count byte at +2 — verified via CMonsterBookAccessor::SetCount disasm),
    // then resolve cardId → mobId via Item.wz and Copy the loaded
    // `Mob/%07d.img/stand/0` canvas onto the cell. The blit currently does
    // not clip to cell bounds, so portraits larger than (0x1B-4, 0x26-4) =
    // (23, 34) pixels will bleed into adjacent cells. KMST's body sets a
    // SetClipRect token at the start of DrawLeftLayer (vfunc 0x84 @ 0x49F8),
    // restored at the end — we'll wire that in a follow-up once Windows
    // iteration confirms portraits load and the bleed becomes the next gap.
    constexpr int32_t kCellsPerPage = 25;
    const int32_t pageMax  = (cardCount == 0) ? 0
                           : static_cast<int32_t>((cardCount - 1) / 25);
    if (g_nLeftPage > pageMax) g_nLeftPage = pageMax;
    if (g_nLeftPage < 0)       g_nLeftPage = 0;
    const uint32_t pageOffset = static_cast<uint32_t>(g_nLeftPage) * kCellsPerPage;
    const uint32_t pageCount  = (cardCount > pageOffset)
                              ? std::min<uint32_t>(cardCount - pageOffset, kCellsPerPage)
                              : 0;
    DEBUG_MESSAGE("  page=%d/%d (offset=%u count=%u)",
                  g_nLeftPage, pageMax, pageOffset, pageCount);
    for (int i = 0; i < kCellsPerPage; ++i) {
        const int32_t cellX = (i % 5) * 0x21 + 8;
        const int32_t cellY = (i / 5) * 0x2D + 0x1F;
        const bool populated = (static_cast<uint32_t>(i) < pageCount);

        // Cell border (cardSlot.img placeholder — replaced with the real WZ
        // cardSlot frame in a follow-up; that's a separate WZ probe).
        const uint32_t borderColor = populated ? 0xFF80C0FF : 0xFF606060;
        try {
            pCanvas->DrawRectangle(cellX, cellY, 0x1B, 0x26, borderColor);
        } catch (const _com_error&) { return; }

        if (!populated) {
            continue;
        }

        // Read MonsterBookCard at m_aCardTable[currentTab][pageOffset + i].p.
        // ZArray uses the V95Ref 8-byte stride (pad0 + p), so .p is the
        // actual card ptr.
        //
        // v95's CMonsterBookMan::LoadCard (0x006634b0) populates each card
        // with the FULL 32-bit cardId at MonsterBookCard+0 (verified by
        // running the WZ probe: `Item/Consume/0238.img` exists, and the
        // first card's stored low-16 bits = 0x50E0 matches cardId 2380000
        // = 0x002450E0). Earlier theory that SetCount truncates to
        // `(short)(cardId-0x50E0)` is wrong for LoadCard-populated cards
        // — those write the full int. Read int32 directly.
        const uint32_t cardIdx = pageOffset + static_cast<uint32_t>(i);
        if (cardIdx >= cardCount) continue;
        auto& v95Ref = tabCards[cardIdx];
        if (!v95Ref.p) {
            DEBUG_MESSAGE("  cell %d (idx %u): V95Ref.p is NULL — skip",
                          i, cardIdx);
            continue;
        }
        const int32_t cardId = *reinterpret_cast<int32_t*>(v95Ref.p);
        const int32_t mobId  = MonsterBook_ResolveMobId(cardId);
        IWzCanvasPtr  pMobCanvas = MonsterBook_LoadMobIcon(mobId);

        if (!pMobCanvas) {
            // Resolution failure — paint a dim red inset so the cell still
            // visibly registers as collected without falsely displaying a
            // portrait we couldn't load.
            try {
                pCanvas->DrawRectangle(cellX + 2, cellY + 2,
                                       0x1B - 4, 0x26 - 4, 0xFF402020);
            } catch (const _com_error&) { return; }
            continue;
        }

        try {
            // Clear the cell interior then composite the mob portrait. KMST
            // composites at the per-card RECT origin via vfunc 0x80 (Copy),
            // optional alpha VARIANT controls the blend. We pass full alpha
            // (0xFF) for collected cards. Cell-interior offset is +1 inside
            // the border rect so the portrait doesn't overpaint the frame.
            pCanvas->DrawRectangle(cellX + 1, cellY + 1,
                                   0x1B - 2, 0x26 - 2, 0xFF000000);
            pCanvas->Copy(cellX + 1, cellY + 1, pMobCanvas);
        } catch (const _com_error& e) {
            DEBUG_MESSAGE("  cell %d card=%d mob=%d: Copy threw 0x%08X (%s)",
                          i, cardId, mobId,
                          static_cast<unsigned>(e.Error()), e.ErrorMessage());
        }
    }

    // Tab indicator strip across the top of the page — useful for confirming
    // m_pLPTab read worked and m_nCurrentTab populated. 9 segments, the
    // current tab gets a brighter highlight.
    for (int t = 0; t < 9; ++t) {
        const int32_t segX = 8 + t * 0x12;
        const uint32_t color = (t == currentTab) ? 0xFFFFFF40 : 0xFF404040;
        try {
            pCanvas->DrawRectangle(segX, 4, 0x10, 0x0C, color);
        } catch (const _com_error&) { return; }
    }

    DEBUG_MESSAGE("  L0 painted: tab=%d, %u/%d cells filled, indicator strip drawn",
                  currentTab, pageCount, kCellsPerPage);
}

// KMST DrawRightLayer (no PDB symbol — likely inlined or stripped).
// DEFERRED: would render the per-card detail panel on the right side
// (mob portrait + drops + skill). Stub: translucent magenta.
static void MonsterBook_DrawRightLayer(uint8_t* pBytes) {
    DEBUG_MESSAGE("MonsterBook_DrawRightLayer: stub fill");
    PaintLayerStub(pBytes, /*slotOffset=*/0x15AC,
                   /*width=*/0xDC, /*height=*/0x122,
                   /*color (ARGB)=*/0xC0A040A0, "L1");
}

// KMST 0x0084B3A0 (276 lines). Renders LAYER[2] — the selection cursor
// + cover-card highlight overlays.
//
// KMST flow:
//   1. Allocate a working canvas via PcCreateObject(L"Canvas") and Create.
//   2. If currentTab != 9 (the special-summary tab):
//        a. coverCardId = CUserLocal::GetMonsterBookCover()
//        b. If cover != 0: GetCard(cover); if cover->tab == currentTab AND
//           cover->idx / 25 == m_nLeftPage, composite a "cover" overlay
//           onto the working canvas at m_aRect[cover->idx % 25].
//        c. Composite a "select" cursor onto the working canvas at
//           m_aRect[m_nSelected] (= +0x7C0).
//   3. Composite the working canvas onto LAYER[2]'s canvas at (0, 0).
//
// v95 port scope: just the select cursor. Cover overlay needs
// CUserLocal::GetMonsterBookCover (no v95 symbol surviving) plus a
// CMonsterBookMan::GetCard(cardId) lookup that returns a struct whose
// tab/idx fields v95 stripped — deferred.
//
// WZ paths recovered from KMST's auto-named string `u_UI_UIWindow_img_Mons`
// truncation: candidates in .rdata are `cover`, `select`, `fullMark`. KMST
// references the same auto-name three times across DrawLeftLayer + Draw-
// SelectLayer, so each call site loads a *different* string starting with
// the shared "UI/UIWindow.img/Mons..." prefix. For DrawSelectLayer's select
// cursor we use `UI/UIWindow.img/MonsterBook/select`. Verified in KMST
// strings dump: that path exists.
//
// m_aRect at v95 +0x15B8 holds 25 + 20 RECT structs (16 bytes each). The
// first 25 are the per-cell hit zones for the left card grid — exactly
// what we need to position the cursor.
static void MonsterBook_DrawSelectLayer(uint8_t* pBytes) {
    auto* pLayer = *reinterpret_cast<IWzGr2DLayer**>(pBytes + 0x15B4);
    if (!pLayer) {
        DEBUG_MESSAGE("MonsterBook_DrawSelectLayer: LAYER[2] is null");
        return;
    }

    IWzCanvasPtr pCanvas;
    try {
        pCanvas = pLayer->canvas[vtEmpty];
    } catch (const _com_error& e) {
        DEBUG_MESSAGE("  pLayer->canvas threw 0x%08X (%s)",
                      e.Error(), e.ErrorMessage());
        return;
    }
    if (!pCanvas) {
        DEBUG_MESSAGE("  LAYER[2] canvas is NULL");
        return;
    }

    // Clear the cursor canvas so prior frames' selection at a different
    // index doesn't ghost. ARGB 0 = fully transparent — LAYER[2] composites
    // over LAYER[0] with alpha so transparent regions show the card grid
    // beneath unobstructed.
    try {
        pCanvas->DrawRectangle(0, 0, 0xAE, 0x100, 0x00000000);
    } catch (const _com_error&) { return; }

    // Cache the "select" cursor canvas — single WZ load for the entire
    // process lifetime. Function-local static: the strong COM ref persists
    // across /book open/close cycles so re-opens skip the WZ probe. Released
    // on DLL unload along with all other static IWzCanvasPtrs in this TU.
    static IWzCanvasPtr s_pSelectCanvas;
    if (!s_pSelectCanvas) {
        try {
            s_pSelectCanvas = get_unknown(get_rm()->GetObjectA(
                Ztl_bstr_t(L"UI/UIWindow.img/MonsterBook/select")));
            DEBUG_MESSAGE("  loaded select canvas -> 0x%08X",
                          s_pSelectCanvas.GetInterfacePtr());
        } catch (const _com_error& e) {
            DEBUG_MESSAGE("  load /select threw 0x%08X (%s)",
                          static_cast<unsigned>(e.Error()), e.ErrorMessage());
        }
    }
    if (!s_pSelectCanvas) {
        DEBUG_MESSAGE("  no select canvas — abort");
        return;
    }

    // Clamp + read m_aRect[g_nSelectedCard]. m_aRect is a 25-entry array of
    // 16-byte RECT structs at +0x15B8. KMST's offset arithmetic in the
    // disasm: `local_20 + idx * 0x10 + 0xF18` = `&m_aRect[idx]`.
    if (g_nSelectedCard < 0 || g_nSelectedCard >= 25) g_nSelectedCard = 0;
    auto* pRect = reinterpret_cast<RECT*>(
        pBytes + 0x15B8 + g_nSelectedCard * sizeof(RECT));

    // KMST composites at (rect.left - 2, rect.top - 2) — 2-px inset so the
    // cursor frame surrounds the cell rather than nesting inside it.
    try {
        pCanvas->Copy(pRect->left - 2, pRect->top - 2, s_pSelectCanvas);
    } catch (const _com_error& e) {
        DEBUG_MESSAGE("  Copy threw 0x%08X (%s)",
                      static_cast<unsigned>(e.Error()), e.ErrorMessage());
        return;
    }

    DEBUG_MESSAGE("  L2 painted: select cursor at idx=%d rect=(%ld,%ld,%ld,%ld)",
                  g_nSelectedCard, pRect->left, pRect->top,
                  pRect->right, pRect->bottom);
}

// KMST CUIMonsterBook::DrawLayer (0x0084CBE7) dispatcher. Verbatim port —
// 3-way switch on the layer index passed by Update.
static void MonsterBook_DrawLayer(uint8_t* pBytes, int layerIdx) {
    switch (layerIdx) {
    case 0: MonsterBook_DrawLeftLayer(pBytes);   break;
    case 1: MonsterBook_DrawRightLayer(pBytes);  break;
    case 2: MonsterBook_DrawSelectLayer(pBytes); break;
    default:
        DEBUG_MESSAGE("MonsterBook_DrawLayer: unexpected idx=%d", layerIdx);
        break;
    }
}

// KMST CUIMonsterBook::Update (0x00847FF0, 23 lines). Walks the 3 dirty
// flags at v95 +0x15A0 / +0x15A8 / +0x15B0 (KMST +0xF00 / +0xF08 / +0xF10),
// fires DrawLayer for each set flag, then chains to CWnd::Update for the
// child-wnd traversal + invalidation.
//
// Note: KMST's body would naturally override the IGObj::Update vtable slot.
// v95's stripped CUIMonsterBook ctor doesn't stamp such an override into
// the vtable, so vtable-driven Update calls hit v95's plain CWnd::Update at
// 0x009AE7C0. We hook that address (below) and run this body BEFORE chaining
// to the original — net effect matches what KMST's vtable override would do.
static void MonsterBook_Update(uint8_t* pBytes) {
    static constexpr size_t kDirtyOffsets[3] = { 0x15A0, 0x15A8, 0x15B0 };
    for (int i = 0; i < 3; ++i) {
        auto* pFlag = reinterpret_cast<int32_t*>(pBytes + kDirtyOffsets[i]);
        if (*pFlag != 0) {
            DEBUG_MESSAGE("MonsterBook_Update: layer %d dirty — paint", i);
            MonsterBook_DrawLayer(pBytes, i);
            *pFlag = 0;
        }
    }
}


// === CCtrlTab impls =========================================================
//
// Defined here (not ctrlwnd.h) because the bodies use IWz* COM smart pointers
// from `ztl/zcom.h`, and ctrlwnd.h is included by callers that should stay
// COM-clean.

CCtrlTab::~CCtrlTab() {
    // KMST stripped CCtrlTab dtor doesn't walk our two raw slots — these are
    // member-owned refs so we Release manually. ~CCtrlWnd runs after this
    // (auto-chain from virtual dtor), destructs m_pLTCtrl, then ~ZRefCounted.
    if (m_pSubLayer) {
        reinterpret_cast<IUnknown*>(m_pSubLayer)->Release();
        m_pSubLayer = nullptr;
    }
    if (m_pCanvas) {
        reinterpret_cast<IUnknown*>(m_pCanvas)->Release();
        m_pCanvas = nullptr;
    }
}

// KMST 0x00845CA6 (296 lines) — tools/decomp/cache_kmst/00845ca6.c.
// Port of CUIMonsterBook::CCtrlTab::CreateCtrl. Builds a sublayer + two
// canvases anchored on the parent wnd's layer.
//
// Steps (verbatim):
//   1. CCtrlWnd::CreateCtrl(this, parent, id, rect.left, rect.top,
//                            rect.width, rect.height, param)
//   2. CWnd::GetLayer(parent)  — parent's IWzGr2DLayer
//   3. _g_gr->CreateLayer(left, top, 0, 0, 0, vtMissing, vt_i4(0)) → sublayer
//   4. m_pSubLayer = sublayer (AddRef)
//   5. sublayer->origin/overlay = parentLayer; color = -1u; z = parent_z + 2
//   6. tmpCanvas = PcCreateObject(L"Canvas")
//   7. sublayer->InsertCanvas(tmpCanvas)
//   8. m_pCanvas = PcCreateObject(L"Canvas") (separate canvas, AddRef'd)
//   9. m_pCanvas->Create(w, h); cx=cy=0
void CCtrlTab::CreateCtrlFromRect(CWnd* pParent, uint32_t nId,
                                  const RECT* pRect, void* pParam) {
    DEBUG_MESSAGE("CCtrlTab::CreateCtrlFromRect: enter id=0x%X rect=(%ld,%ld,%ld,%ld)",
                  nId, pRect->left, pRect->top, pRect->right, pRect->bottom);

    const int32_t w = (pRect->right - pRect->left) + 1;
    const int32_t h = (pRect->bottom - pRect->top) + 1;

    // Step 1 — base CreateCtrl. Inherited 7-arg signature on kinoko CCtrlWnd.
    CCtrlWnd::CreateCtrl(pParent, nId, pRect->left, pRect->top, w, h, pParam);

    // Step 2 — parent layer.
    IWzGr2DLayerPtr pParentLayer;
    try {
        pParentLayer = pParent->GetLayer();
    } catch (const _com_error& e) {
        DEBUG_MESSAGE("  GetLayer threw 0x%08X (%s)", e.Error(), e.ErrorMessage());
        return;
    }
    if (!pParentLayer) {
        DEBUG_MESSAGE("  parent layer is NULL — abort");
        return;
    }

    long parentZ = 0;
    try { parentZ = pParentLayer->z; } catch (const _com_error&) {}

    IWzGr2DPtr& pGr = get_gr();
    if (!pGr) {
        DEBUG_MESSAGE("  _g_gr is NULL — abort");
        return;
    }

    Ztl_variant_t vParent(static_cast<IUnknown*>(pParentLayer.GetInterfacePtr()));
    Ztl_variant_t vFilter(0L, VT_I4);

    // Step 3 — sublayer at (rect.left, rect.top). w=h=0 ⇒ auto-size.
    IWzGr2DLayerPtr pSub;
    try {
        pSub = pGr->CreateLayer(pRect->left, pRect->top, 0, 0, 0, vtEmpty, vFilter);
    } catch (const _com_error& e) {
        DEBUG_MESSAGE("  CreateLayer threw 0x%08X (%s)", e.Error(), e.ErrorMessage());
        return;
    }
    if (!pSub) {
        DEBUG_MESSAGE("  CreateLayer returned NULL");
        return;
    }

    // Step 4 — m_pSubLayer holds an AddRef'd ref so it survives past pSub's scope.
    pSub->AddRef();
    m_pSubLayer = pSub.GetInterfacePtr();

    // Step 5 — properties.
    try {
        pSub->origin  = vParent;
        pSub->overlay = vParent;
        pSub->color   = 0xFFFFFFFFUL;
        pSub->z       = parentZ + 2;
    } catch (const _com_error& e) {
        DEBUG_MESSAGE("  sublayer setters threw 0x%08X (%s)", e.Error(), e.ErrorMessage());
        return;
    }

    // Step 6/7 — temp canvas inserted into sublayer (visible content).
    IWzCanvasPtr pTmpCanvas;
    try {
        PcCreateObject<IWzCanvasPtr>(L"Canvas", pTmpCanvas, nullptr);
        if (pTmpCanvas) {
            pTmpCanvas->Create(static_cast<unsigned long>(w),
                               static_cast<unsigned long>(h));
            pTmpCanvas->cx = 0;
            pTmpCanvas->cy = 0;
            pSub->InsertCanvas(pTmpCanvas);
        }
    } catch (const _com_error& e) {
        DEBUG_MESSAGE("  tmp canvas create/insert threw 0x%08X (%s)",
                      e.Error(), e.ErrorMessage());
    }

    // Step 8/9 — separate m_pCanvas (off-screen scratch).
    IWzCanvasPtr pCv;
    try {
        PcCreateObject<IWzCanvasPtr>(L"Canvas", pCv, nullptr);
        if (pCv) {
            pCv->Create(static_cast<unsigned long>(w),
                        static_cast<unsigned long>(h));
            pCv->cx = 0;
            pCv->cy = 0;
            pCv->AddRef();
            m_pCanvas = pCv.GetInterfacePtr();
        }
    } catch (const _com_error& e) {
        DEBUG_MESSAGE("  m_pCanvas create threw 0x%08X (%s)",
                      e.Error(), e.ErrorMessage());
    }

    DEBUG_MESSAGE("CCtrlTab::CreateCtrlFromRect: built sublayer @0x%08X canvas @0x%08X",
                  m_pSubLayer, m_pCanvas);
}

// KMST 0x008469C0 (~370 lines, tools/decomp/cache_kmst/008469c0.c).
// CCtrlLPTab::CreateCtrl iterates the WZ tree at
// `UI/UIWindow.img/MonsterBook/MainTab/...` and builds a list of
// CCtrlTab::Info structs.
//
// Originally this called CCtrlTab::CreateCtrlFromRect which in turn called
// CCtrlWnd::CreateCtrl to register LPTab as a child wnd of the parent. That
// caused crashes when v95's input system dispatched click events through
// the LPTab — our compiler-generated kinoko C++ vtable for CCtrlLPTab does
// NOT match v95's expected layout, so OnChildNotify's vtable[8] dispatch
// (which v95 expects to land on OnButtonClicked) lands inside an unrelated
// v95 function (CInputSystem::TryAcquireDevice +0x1F9), eventually faulting
// at garbage memory.
//
// Skip the registration entirely. LPTab still exists as a heap object so
// DrawLeftLayer can read m_nCurrentTab from offset +0x34 (= 0). The visual
// tab strip on the left is invisible (was already invisible because we
// skipped the WZ tab tree iteration) and click events on the strip area no
// longer dispatch through it.
void CCtrlLPTab::CreateCtrlFromRect(CWnd* pParent, uint32_t nId,
                                    const RECT* pRect, void* pParam) {
    DEBUG_MESSAGE("CCtrlLPTab::CreateCtrlFromRect: data-only (no wnd registration)");
    m_nCurrentTab = 0;
}

// KMST 0x008476ED (~290 lines). Same data-only port as LPTab: don't register
// as child wnd; just keep m_nCurrentTab=0 readable from the slot.
void CCtrlRPTab::CreateCtrlFromRect(CWnd* pParent, uint32_t nId,
                                    const RECT* pRect, void* pParam) {
    DEBUG_MESSAGE("CCtrlRPTab::CreateCtrlFromRect: data-only (no wnd registration)");
    m_nCurrentTab = 0;
}


// === Page navigation ========================================================
//
// Total left-pages for the current tab = ceil(cardCount / 25). Used by the
// arrow buttons to clamp m_nLeftPage and by DrawLeftLayer to slice the card
// list. Right-page bound stays at >=0; LAYER[1] doesn't render real content
// yet so the upper bound is symbolic.
namespace {
    int32_t MonsterBook_GetCurrentTab(uint8_t* pBytes) {
        auto* pLPTab = *reinterpret_cast<CCtrlLPTab**>(pBytes + 0x1564);
        if (!pLPTab) return -1;
        const int32_t tab = pLPTab->m_nCurrentTab;
        return (tab < 0 || tab >= 9) ? -1 : tab;
    }

    uint32_t MonsterBook_GetTabCardCount(uint8_t* pBytes, int32_t tab) {
        if (tab < 0 || tab >= 9) return 0;
        auto* pCardTableArrays = reinterpret_cast<ZArray<V95Ref>*>(pBytes + 0x1888);
        return pCardTableArrays[tab].GetCount();
    }

    int32_t MonsterBook_LeftPageMax(uint8_t* pBytes) {
        const int32_t tab = MonsterBook_GetCurrentTab(pBytes);
        const uint32_t cnt = MonsterBook_GetTabCardCount(pBytes, tab);
        if (cnt == 0) return 0;
        return static_cast<int32_t>((cnt - 1) / 25);
    }

    void MonsterBook_MarkLayerDirty(uint8_t* pBytes, size_t dirtyOffset) {
        *reinterpret_cast<int32_t*>(pBytes + dirtyOffset) = 1;
    }

    void MonsterBook_ResetPageState() {
        g_nLeftPage     = 0;
        g_nRightPage    = 0;
        g_nSelectedCard = 0;
    }
}

// KMST 0x00848185 (60 lines) — CUIMonsterBook::OnButtonClicked. Six button
// IDs handled:
//   1000  (close button)         — let v95's CUIWnd dtor chain handle it via
//                                  the CWvsContext +0x3EB4 slot-clear path.
//                                  Nothing for us to do.
//   2000  (search button)        — KMST reads the search box text, calls
//                                  SearchCard + SelectCard. Both KMST helpers
//                                  were stripped in v95 and need WZ string-
//                                  index search infrastructure we haven't
//                                  ported. For this iteration we just log
//                                  the click; real search is a follow-up.
//   0x07D1 (left page prev)      — m_nLeftPage--, clamp to 0, re-paint LAYER[0]
//   0x07D2 (left page next)      — m_nLeftPage++, clamp to leftPageMax
//   0x07D3 (right page prev)     — m_nRightPage-- (LAYER[1] is stub for now)
//   0x07D4 (right page next)     — m_nRightPage++
// Other IDs fall through to the original CWnd::OnButtonClicked no-op.
static void MonsterBook_OnButtonClicked(uint8_t* pBytes, uint32_t nId) {
    switch (nId) {
    case 1000:
        DEBUG_MESSAGE("MonsterBook_OnButtonClicked: close (1000) — let v95 handle");
        return;
    case 2000: {
        DEBUG_MESSAGE("MonsterBook_OnButtonClicked: search (2000)");
        // Read the edit text for logging. Real per-name card search comes
        // when we wire up the WZ String/Mob.img name index — this is the
        // visible-payoff scaffolding.
        auto* pEdit = *reinterpret_cast<CCtrlEdit**>(pBytes + 0x1570);
        if (pEdit) {
            DEBUG_MESSAGE("  edit ptr ok @0x%08X — search no-op pending name index",
                          pEdit);
        } else {
            DEBUG_MESSAGE("  edit slot is NULL");
        }
        return;
    }
    case 0x07D1: {
        const int32_t prev = g_nLeftPage;
        if (g_nLeftPage > 0) {
            --g_nLeftPage;
        }
        DEBUG_MESSAGE("MonsterBook_OnButtonClicked: left-prev %d -> %d",
                      prev, g_nLeftPage);
        MonsterBook_MarkLayerDirty(pBytes, 0x15A0);
        return;
    }
    case 0x07D2: {
        const int32_t prev = g_nLeftPage;
        const int32_t maxPage = MonsterBook_LeftPageMax(pBytes);
        if (g_nLeftPage < maxPage) {
            ++g_nLeftPage;
        }
        DEBUG_MESSAGE("MonsterBook_OnButtonClicked: left-next %d -> %d (max=%d)",
                      prev, g_nLeftPage, maxPage);
        MonsterBook_MarkLayerDirty(pBytes, 0x15A0);
        return;
    }
    case 0x07D3: {
        const int32_t prev = g_nRightPage;
        if (g_nRightPage > 0) {
            --g_nRightPage;
        }
        DEBUG_MESSAGE("MonsterBook_OnButtonClicked: right-prev %d -> %d",
                      prev, g_nRightPage);
        MonsterBook_MarkLayerDirty(pBytes, 0x15A8);
        return;
    }
    case 0x07D4: {
        const int32_t prev = g_nRightPage;
        ++g_nRightPage;
        DEBUG_MESSAGE("MonsterBook_OnButtonClicked: right-next %d -> %d",
                      prev, g_nRightPage);
        MonsterBook_MarkLayerDirty(pBytes, 0x15A8);
        return;
    }
    default:
        DEBUG_MESSAGE("MonsterBook_OnButtonClicked: unhandled id=%u (0x%X)",
                      nId, nId);
        return;
    }
}


// === Hooks ==================================================================

static auto CWvsContext__UI_Open = kCWvsContext_UI_Open;
static auto CWvsContext__UI_Close = kCWvsContext_UI_Close;
static auto CWnd__Update = kCWnd_Update;
static auto CWnd__OnChildNotify = kCWnd_OnChildNotify;

void __fastcall CWnd__OnChildNotify_hook(void* pThis, void* /*EDX*/,
                                          uint32_t nId, uint32_t nMsg,
                                          int32_t nLParam) {
    // Detoured over CWnd::OnChildNotify. msg==100 means a button click;
    // for our wnd, run our handler before chaining to the original (which
    // dispatches via vtable[8] = CUIWnd::OnButtonClicked → close path).
    //
    // DIAGNOSTIC: log every fire when id is in our wnd's button range so
    // we can pinpoint which parent the close-button's MouseUp is dispatching
    // on. Filter is intentionally narrower than the action filter so we
    // don't spam every CWnd dispatch in the entire UI.
    if (nMsg == 100 && (nId == 1000 || (nId >= 0x07D0 && nId <= 0x07D6))) {
        DEBUG_MESSAGE("OnChildNotify_hook: pThis=0x%08X global=0x%08X id=%u msg=%u",
                      pThis, g_pV95MonsterBookGlobal,
                      nId, nMsg);
    }
    if (pThis != nullptr && pThis == g_pV95MonsterBookGlobal && nMsg == 100) {
        MonsterBook_OnButtonClicked(static_cast<uint8_t*>(pThis), nId);
    }
    reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t, int32_t)>(
        CWnd__OnChildNotify)(pThis, nId, nMsg, nLParam);
}

void __fastcall CWnd__Update_hook(void* pThis, void* /*EDX*/) {
    // Every CWnd::Update call in the v95 client flows through this hook —
    // chat, hotbar, every UI window. Filter on the live MonsterBook slot so
    // we only run the dirty-flag dispatch for our buffer. The check is on
    // g_pV95MonsterBookGlobal (= DAT_00c6f010) rather than g_pMonsterBookUI
    // because UI_Close clears the latter early in its hook; a stray Update
    // tick during teardown would otherwise re-run paints on a freed buffer.
    if (pThis != nullptr && pThis == g_pV95MonsterBookGlobal) {
        MonsterBook_Update(static_cast<uint8_t*>(pThis));
    }
    reinterpret_cast<void(__thiscall*)(void*)>(CWnd__Update)(pThis);
}

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
        MonsterBook_ResetPageState();
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
    // Hooks every CWnd::Update — filtered to our wnd inside the detour.
    ATTACH_HOOK(CWnd__Update, CWnd__Update_hook);
    // Hooks CWnd::OnChildNotify — fires for every button-click dispatch
    // across the entire UI; filter (pThis == ours, msg==100) inside.
    // Chosen over CUIWnd::OnButtonClicked because Detours-trampolining the
    // latter placed the trampoline pointer inside an unrelated function
    // (CMemoryGameDlg::IsWinnerLastTime) → crashed on first close click.
    ATTACH_HOOK(CWnd__OnChildNotify, CWnd__OnChildNotify_hook);
}
