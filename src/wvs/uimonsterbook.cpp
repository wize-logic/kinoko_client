#include "pch.h"
#include "hook.h"
#include "debug.h"
#include "ztl/ztl.h"
#include "wvs/uimonsterbook.h"

#include <cstring>


// v95.1 CUIMonsterBook full-layout size. KMST's class extends to MOBINFO end
// at +0x1240 (KMST coords); v95's CUIWnd is 0x6A0 bytes larger so the same
// layout reaches +0x18E0 here. Calling the v95 CUIWnd ctor on an under-sized
// buffer would corrupt adjacent heap blocks — keep the alloc full-fat even
// though Phase 1 only initialises the CUIWnd base.
constexpr size_t kCUIMonsterBookSize = 0x18E0;

// Mirrors v95's DAT_00c6f010 (the global CUIMonsterBook* singleton that
// OnMonsterBookSetCard / OnMonsterBookSetCover gate on). Phase 2 will write
// through to that DAT once the rest of the class is wired up; leaving DAT
// null in Phase 1 keeps incoming card packets in their existing null-bail
// path so they can't crash on our half-built object.
static void* g_pMonsterBookUI = nullptr;


// FUN_009d83f0 = CWvsContext::UI_Open(int nUIType, int nOption). Verified
// __thiscall(this, int, int) — Ghidra reports stack_purge=8 and the IDB
// mangled name `?UI_Open@CWvsContext@@QAEXHH@Z` confirms two int params.
static auto CWvsContext__UI_Open = 0x009D83F0;

void __fastcall CWvsContext__UI_Open_hook(void* pThis, void* /*EDX*/,
                                          int nUIType, int nOption) {
    if (nUIType == 9) {  // MONSTERBOOK — case 9 is empty in this build.
        if (g_pMonsterBookUI == nullptr) {
            void* pBuf = ZAllocEx<ZAllocAnonSelector>::s_Alloc(kCUIMonsterBookSize);
            if (pBuf) {
                std::memset(pBuf, 0, kCUIMonsterBookSize);

                // FUN_008dd680 = CUIWnd::CUIWnd. Args mirror KMST's
                // CUIMonsterBook::CUIMonsterBook (0x847CD3) -> (this, 9,
                // 0, 0, 0, 1, 0, 0). Stamps the three multi-inheritance
                // vtables (IGObj / IUIMsgHandler / ZRefCounted) and seats
                // m_nUIType at +0xAD0.
                reinterpret_cast<void(__thiscall*)(void*, int, int, int,
                                                   int, int, int, int)>(
                    0x008DD680)(pBuf, 9, 0, 0, 0, 1, 0, 0);

                // FUN_008dd300 = CUIWnd::CreateUIWndPosSaved(this, w, h, ?).
                // Args from KMST -> (0x1DB, 0x15D, 10) -> 475x349.
                reinterpret_cast<void(__thiscall*)(void*, int, int, int)>(
                    0x008DD300)(pBuf, 0x1DB, 0x15D, 10);

                g_pMonsterBookUI = pBuf;
                DEBUG_MESSAGE("CUIMonsterBook minimal stub at 0x%08X", pBuf);
            } else {
                DEBUG_MESSAGE("CUIMonsterBook s_Alloc(0x%X) failed",
                              static_cast<unsigned>(kCUIMonsterBookSize));
            }
        }
        // Phase 2 will hook OnCreate / OnDestroy so the window actually
        // shows + closes cleanly. For Phase 1 we just confirm the hook path
        // lights up and the v95 CUIWnd constructor doesn't crash.
        return;
    }
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(CWvsContext__UI_Open)(
        pThis, nUIType, nOption);
}


void AttachUIMonsterBook() {
    ATTACH_HOOK(CWvsContext__UI_Open, CWvsContext__UI_Open_hook);
}
