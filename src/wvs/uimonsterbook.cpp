#include "pch.h"
#include "hook.h"
#include "debug.h"
#include "ztl/ztl.h"
#include "wvs/uimonsterbook.h"
#include "wvs/ctrlwnd.h"
#include "wvs/wnd.h"
#include "wvs/wndman.h"

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
//   FUN_008dd380  CUIWnd::OnDestroy — actually a position-save helper,
//                 not a list-removal. Reads +0xAF9 sentinel. Calling it
//                 alone leaves the window in the wnd-manager lists, so
//                 it stays drawn after click-X. Kept here for reference.
//   FUN_009b30c0  CWndMan::RemoveWindow(CWnd*) — static.
//   FUN_009b30f0  CWndMan::RemoveUpdateWindow(CWnd*) — static.
//   FUN_009b3120  CWndMan::RemoveInvalidatedWindow(CWnd*) — static.
//   FUN_009b3180  CWndMan::UnregisterUIWindow(CUIWnd*) — instance method.
//   These four are the canonical list-removal sequence (FUN_009b0e50
//   calls them as the close-time prefs-save tail). After running them
//   the wnd manager stops drawing the window.
//   DAT_00c6f010  global CUIMonsterBook* singleton — OnMonsterBookSetCard
//                 / OnMonsterBookSetCover gate on this for null-bail.
constexpr uintptr_t kCUIWnd_ctor                          = 0x008DD680;
constexpr uintptr_t kCUIWnd_CreateUIWndPosSaved           = 0x008DD300;
constexpr uintptr_t kCWndMan_RemoveWindow                 = 0x009B30C0;
constexpr uintptr_t kCWndMan_RemoveUpdateWindow           = 0x009B30F0;
constexpr uintptr_t kCWndMan_RemoveInvalidatedWindow      = 0x009B3120;
constexpr uintptr_t kCWndMan_UnregisterUIWindow           = 0x009B3180;
constexpr uintptr_t kCWvsContext_UI_Open                  = 0x009D83F0;
constexpr uintptr_t kCWvsContext_UI_Close                 = 0x009D5370;
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

    // Local sentinel so UI_Open's `if-already-open` short-circuit and
    // UI_Close's `if-still-open` cleanup can both check the same thing
    // without racing the v95 global.
    void* g_pMonsterBookUI = nullptr;

    // Deferred-destroy slot. UI_Close removes the window from CWvsContext's
    // draw stack and stashes the buffer here; it's NOT freed inline because
    // the close X button's click handler is on the call stack at that point
    // and freeing the buffer (which contains the button) would return into
    // freed memory. The next UI_Open reaps it before allocating fresh.
    void* g_pMonsterBookPendingFree = nullptr;
}


// === Builder stubs (Phase 2 ports — TODO) ===================================
//
// Each stub records the KMST source it ports from. Offsets in the comments
// are in KMST coordinates; subtract 0x468 (KMST sizeof(CUIWnd)) and add
// 0xB08 (v95 sizeof(CUIWnd)) — net +0x6A0 — to get the v95 absolute offset.
//
// All v95 helper addresses (CCtrlButton::CCtrlButton, CCtrlTab::CCtrlTab,
// the CCtrlLPTab/CCtrlRPTab vtables, ZXString<wchar_t>::operator=, the
// CCtrlEdit::CREATEPARAM ctor/dtor, etc.) need to be looked up before the
// port can run. cuimonsterbook_xref.tsv in asdfstory tracks which are
// known and which are TBD. The plan is one builder per follow-up commit so
// each can be Windows-tested independently — fail-fast on the first one
// that crashes.

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
        auto* pBtn = new CCtrlButton();
        if (!pBtn) {
            DEBUG_MESSAGE("CCtrlButton alloc failed for id=%u", spec.id);
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
    }
}

// KMST 0x00848C8A (601 lines) — tools/decomp/cache_kmst/00848c8a.c.
// Builds the 3 LAYER child controls at v95 +0x15A0..+0x15B8 (8 bytes each).
// Each LAYER is dirty-flag-driven by UpdateUI which writes 1 to the first
// field on every refresh.
static void MonsterBook_CreateLayer(void* /*pThis*/) {
    // TODO Phase 2-port-2.
}

// KMST 0x0084988E (32 lines) — tools/decomp/cache_kmst/0084988e.c.
// Initialises the click-zone rectangles HitTest reads.
static void MonsterBook_CreateRect(void* /*pThis*/) {
    // TODO Phase 2-port-3.
}

// KMST 0x0084991F (73 lines) — tools/decomp/cache_kmst/0084991f.c.
// Allocates the 9-column ZArray<ZArray<ZRef<MonsterBookCard>>> at v95 +0x1888
// (= KMST +0x11E8) — backing store for all collected cards by tab/area.
static void MonsterBook_CreateCardTable(void* /*pThis*/) {
    // TODO Phase 2-port-4.
}

// KMST 0x00849A25 (199 lines) — tools/decomp/cache_kmst/00849a25.c.
// Pre-loads IWzFont COM ptrs into the m_aFonts ZArray (+0xE64 v95 = KMST
// +0x7C4 — the ctor zeros this slot, the builder fills it).
static void MonsterBook_CreateFontArray(void* /*pThis*/) {
    // TODO Phase 2-port-5.
}


// === Lifecycle ==============================================================

// Mirrors KMST CUIMonsterBook::CUIMonsterBook + OnCreate. Run on a fresh
// kCUIMonsterBookSize buffer.
static void MonsterBook_Construct(void* pThis) {
    // CUIWnd::CUIWnd(this, 9, 0, 0, 0, 1, 0, 0) — args verbatim from KMST.
    reinterpret_cast<void(__thiscall*)(void*, int, int, int, int, int, int, int)>(
        kCUIWnd_ctor)(pThis, 9, 0, 0, 0, 1, 0, 0);

    // CreateUIWndPosSaved(this, 0x1DB, 0x15D, 10) — 475x349 client area.
    reinterpret_cast<void(__thiscall*)(void*, int, int, int)>(
        kCUIWnd_CreateUIWndPosSaved)(pThis, 0x1DB, 0x15D, 10);

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
}

// Mirrors KMST CUIMonsterBook::~CUIMonsterBook + scalar_deleting_destructor.
// PARTIAL: cleans up the Phase 2-port-1 buttons and frees the buffer. Still
// missing: v95 CUIWnd::~CUIWnd cleanup of m_uiToolTip / m_sBackgrndUOL /
// m_abOption (~30 bytes leaked per open/close until Phase 2-port-6).
static void MonsterBook_Destroy(void* pThis) {
    // Release the ZRef<CCtrlButton> slots populated by CreateCtrl. Setting
    // each ZRef to nullptr triggers _Release on the underlying button so its
    // refcount drops to 0 and the C++ delete path frees it via ZAllocEx.
    static constexpr size_t kBtnSlotOffsets[] = {
        0x0080, 0x1578, 0x1580, 0x1588, 0x1590, 0x1598,
    };
    for (auto offset : kBtnSlotOffsets) {
        auto* pSlot = reinterpret_cast<ZRef<CCtrlButton>*>(
            static_cast<uint8_t*>(pThis) + offset);
        *pSlot = nullptr;
    }

    // TODO Phase 2-port-6: invoke v95 CUIWnd::~CUIWnd at <addr> to clean up
    // the CUIWnd-base allocations the v95 ctor made (m_uiToolTip /
    // m_sBackgrndUOL / m_abOption). Then add release loops here for the
    // builders that land (CreateLayer's LAYERs, CreateCardTable's grid, etc.).
    ZAllocEx<ZAllocAnonSelector>::s_Free(pThis);
}


// === Hooks ==================================================================

static auto CWvsContext__UI_Open = kCWvsContext_UI_Open;
static auto CWvsContext__UI_Close = kCWvsContext_UI_Close;

void __fastcall CWvsContext__UI_Open_hook(void* pThis, void* /*EDX*/,
                                          int nUIType, int nOption) {
    if (nUIType == 9) {  // MONSTERBOOK — case 9 stripped from v95 UI_Open.
        // Reap any buffer the previous UI_Close deferred. Doing it here, on
        // a fresh stack, is safe — the close X button's click handler has
        // long since unwound.
        if (g_pMonsterBookPendingFree) {
            void* p = g_pMonsterBookPendingFree;
            g_pMonsterBookPendingFree = nullptr;
            MonsterBook_Destroy(p);
        }
        if (g_pMonsterBookUI == nullptr) {
            void* pBuf = ZAllocEx<ZAllocAnonSelector>::s_Alloc(kCUIMonsterBookSize);
            if (pBuf) {
                std::memset(pBuf, 0, kCUIMonsterBookSize);
                MonsterBook_Construct(pBuf);
                g_pMonsterBookUI = pBuf;
                g_pV95MonsterBookGlobal = pBuf;
                DEBUG_MESSAGE("CUIMonsterBook open: 0x%08X", pBuf);
            } else {
                DEBUG_MESSAGE("CUIMonsterBook s_Alloc(0x%X) failed",
                              static_cast<unsigned>(kCUIMonsterBookSize));
            }
        }
        // Window already open; clicking the hotkey again is a no-op (matches
        // v95 cases that gate on `if (m_pUIxxx == 0)`).
        return;
    }
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(CWvsContext__UI_Open)(
        pThis, nUIType, nOption);
}

void __fastcall CWvsContext__UI_Close_hook(void* pCtx, void* /*EDX*/, int nUIType) {
    if (nUIType == 9) {  // MONSTERBOOK
        // X-button → CUIWnd::OnButtonClicked(1000) → CWvsContext::UI_Close(9).
        // v95's case-9 close handler gates on CWvsContext+0x3EB4 which we
        // never set, so the original handler would no-op anyway. Run our
        // teardown directly and skip the tail-call.
        if (g_pMonsterBookUI) {
            DEBUG_MESSAGE("CUIMonsterBook close: 0x%08X", g_pMonsterBookUI);
            // Pull the window out of every CWndMan list it sits in. These
            // four calls are the same sequence FUN_009b0e50 (the v95
            // case-9 close prefs-save) makes at its tail; after they run
            // the wnd manager skips our window on the next iteration.
            // Pure list-remove — no callbacks into our vtable, safe to
            // call from inside the click handler.
            reinterpret_cast<void(__cdecl*)(void*)>(kCWndMan_RemoveWindow)(
                g_pMonsterBookUI);
            reinterpret_cast<void(__cdecl*)(void*)>(kCWndMan_RemoveUpdateWindow)(
                g_pMonsterBookUI);
            reinterpret_cast<void(__cdecl*)(void*)>(kCWndMan_RemoveInvalidatedWindow)(
                g_pMonsterBookUI);
            if (auto* pWndMan = CWndMan::GetInstance()) {
                reinterpret_cast<void(__thiscall*)(void*, void*)>(
                    kCWndMan_UnregisterUIWindow)(pWndMan, g_pMonsterBookUI);
            }
            // Defer the actual buffer free until the next UI_Open. Freeing
            // here would return into the now-released close button.
            g_pMonsterBookPendingFree = g_pMonsterBookUI;
            g_pMonsterBookUI = nullptr;
            g_pV95MonsterBookGlobal = nullptr;
        }
        return;
    }
    reinterpret_cast<void(__thiscall*)(void*, int)>(CWvsContext__UI_Close)(
        pCtx, nUIType);
}


void AttachUIMonsterBook() {
    ATTACH_HOOK(CWvsContext__UI_Open, CWvsContext__UI_Open_hook);
    ATTACH_HOOK(CWvsContext__UI_Close, CWvsContext__UI_Close_hook);
}
