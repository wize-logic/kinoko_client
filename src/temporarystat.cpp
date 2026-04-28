#include "pch.h"
#include "hook.h"
#include "ztl/ztl.h"
#include "wvs/util.h"
#include "wvs/wvsapp.h"
#include "wvs/wvscontext.h"
#include "wvs/temporarystatview.h"
#include <timeapi.h>
#include <cstdio>
#include <cstdint>

#define COOLTIME_OFFSET_HEIGHT 40


// All globals below are touched only from the game's main (UI) thread —
// every hook in this file is invoked from CWvsApp::Run's update path.
// No synchronization needed unless that assumption ever changes.
//
// g_tsvCooltime is constructed by C++, so its vptr is the toolchain-emitted one,
// NOT the game's CTemporaryStatView vftable at 0xBAEC.. — safe today only because
// nothing we hand g_tsvCooltime to ever dispatches through it virtually.
static int g_tLastUpdate = -1;
static IWzPropertyPtr g_pPropSecond;
static CTemporaryStatView g_tsvCooltime;

static auto CTemporaryStatView__Clear = reinterpret_cast<void(__thiscall*)(CTemporaryStatView*)>(0x0075C330);

void __fastcall CTemporaryStatView__Clear_hook(CTemporaryStatView* pThis, void* _EDX) {
    CTemporaryStatView__Clear(pThis);
    CTemporaryStatView__Clear(&g_tsvCooltime);
}

static auto CTemporaryStatView__Show = reinterpret_cast<void(__thiscall*)(CTemporaryStatView*)>(0x0075C6A0);

void __fastcall CTemporaryStatView__Show_hook(CTemporaryStatView* pThis, void* _EDX) {
    CTemporaryStatView__Show(pThis);
    CTemporaryStatView__Show(&g_tsvCooltime);
}

static auto CTemporaryStatView__Hide = reinterpret_cast<void(__thiscall*)(CTemporaryStatView*)>(0x0075C8C0);

void __fastcall CTemporaryStatView__Hide_hook(CTemporaryStatView* pThis, void* _EDX) {
    CTemporaryStatView__Hide(pThis);
    CTemporaryStatView__Hide(&g_tsvCooltime);
}

static auto CTemporaryStatView__Update = reinterpret_cast<void(__thiscall*)(CTemporaryStatView*)>(0x0075DC50);

void __fastcall CTemporaryStatView__Update_hook(CTemporaryStatView* pThis, void* _EDX) {
    CTemporaryStatView__Update(pThis);
    CTemporaryStatView__Update(&g_tsvCooltime);
    g_tLastUpdate = CWvsApp::GetInstance()->m_tUpdateTime;
}

static auto CTemporaryStatView__AdjustPosition = reinterpret_cast<void(__thiscall*)(CTemporaryStatView*)>(0x0075CAD0);

void AdjustPositionWithOffset(CTemporaryStatView* tsv, int32_t nOffset) {
    CTemporaryStatView__AdjustPosition(tsv);
    auto pos = tsv->m_lTemporaryStat.GetHeadPosition();
    while (pos) {
        auto pNext = ZList<ZRef<CTemporaryStatView::TEMPORARY_STAT>>::GetNext(pos);
        pNext->pLayer->RelOffset(0, nOffset);
        pNext->pLayerShadow->RelOffset(0, nOffset);
    }
}

void __fastcall CTemporaryStatView__AdjustPosition_hook(CTemporaryStatView* pThis, void* _EDX) {
    CTemporaryStatView__AdjustPosition(pThis);
    AdjustPositionWithOffset(&g_tsvCooltime, COOLTIME_OFFSET_HEIGHT);
}


static auto CWvsContext__SetSkillCooltimeOver = reinterpret_cast<void(__thiscall*)(CWvsContext*, int32_t, int32_t)>(0x009DB0D0);

void __fastcall CWvsContext__SetSkillCooltimeOver_hook(CWvsContext* pThis, void* _EDX, int32_t nSkillID, int32_t tTimeOver) {
    CWvsContext__SetSkillCooltimeOver(pThis, nSkillID, tTimeOver);
    // Battleship durability
    if (nSkillID == 5221999) {
        return;
    }
    int32_t nRemain = tTimeOver - timeGetTime();
    auto pos = g_tsvCooltime.m_lTemporaryStat.GetHeadPosition();
    while (pos) {
        auto pNext = ZList<ZRef<CTemporaryStatView::TEMPORARY_STAT>>::GetNext(pos);
        if (pNext->nID == nSkillID) {
            pNext->tLeft = nRemain;
            return;
        }
    }
    UINT128 uFlag;
    // Each cooldown entry is keyed by (TSV_SKILL, nSkillID) inside m_lTemporaryStat,
    // so the per-entry flag bit doesn't need to be unique — every overlay can share bit 1.
    // (The previous (nSkillID % 127)+1 scheme collided whenever skill IDs were 127 apart.)
    uFlag.setBitNumber(1, 1);
    g_tsvCooltime.SetTemporary(TSV_SKILL, nSkillID, nRemain, uFlag, ZXString<char>(), 0, 0);
    AdjustPositionWithOffset(&g_tsvCooltime, COOLTIME_OFFSET_HEIGHT);
}

static auto CWvsContext__RemoveSkillCooltimeOver = reinterpret_cast<void(__thiscall*)(CWvsContext*, int32_t)>(0x009CCF80);

void __fastcall CWvsContext__RemoveSkillCooltimeOver_hook(CWvsContext* pThis, void* _EDX, int32_t nSkillID) {
    CWvsContext__RemoveSkillCooltimeOver(pThis, nSkillID);
    g_tsvCooltime.ResetTemporary(TSV_SKILL, nSkillID);
}


static auto TEMPORARY_STAT__UpdateShadowIndex = 0x0075D560;

void __fastcall TEMPORARY_STAT__UpdateShadowIndex_hook(CTemporaryStatView::TEMPORARY_STAT* pThis, void* _EDX) {
    if (pThis->bNoShadow) {
        return;
    }
    int32_t nSeconds = pThis->tLeft / 1000;
    if (nSeconds == pThis->nIndexShadow) {
        return;
    }
    pThis->nIndexShadow = nSeconds; // hijack nIndexShadow to redraw every second
    int32_t nShadowIndex = 0;
    if (pThis->tLeftUnit) {
        nShadowIndex = pThis->tLeft / pThis->tLeftUnit;
        if (nShadowIndex < 0) {
            nShadowIndex = 0;
        } else if (nShadowIndex > 15) {
            nShadowIndex = 15;
        }
    }

    // remove old canvas
    pThis->pLayerShadow->RemoveCanvas(-2);


    // resolve shadow canvas
    wchar_t sShadowProperty[256];
    swprintf_s(sShadowProperty, 256, L"UI/UIWindow.img/Skill/CoolTime/%d", nShadowIndex);
    IWzCanvasPtr pShadowCanvas = get_unknown(get_rm()->GetObjectA(Ztl_bstr_t(sShadowProperty)));

    // create copy of shadow canvas
    IWzCanvasPtr pNewCanvas;
    PcCreateObject<IWzCanvasPtr>(L"Canvas", pNewCanvas, nullptr);
    pNewCanvas->Create(32, 32);
    pNewCanvas->Copy(0, 0, pShadowCanvas);

    // draw number on canvas
    if (!g_pPropSecond) {
        g_pPropSecond = get_unknown(get_rm()->GetObjectA(Ztl_bstr_t(L"UI/Basic.img/ItemNo")));
    }
    int32_t nOffset = 2;
    if (nSeconds >= 60) {
        nSeconds = nSeconds / 60; // display minutes
    } else if (nSeconds >= 10) {
        nOffset = 15;
    } else {
        nOffset = 22;
    }
    if (nSeconds > 999 || nSeconds <= 0) {
        return;
    }
    draw_number_by_image(pNewCanvas, nOffset, 19, nSeconds, g_pPropSecond, 0);

    // insert canvas
    pThis->pLayerShadow->InsertCanvas(pNewCanvas, 500, 210, 64);
}


static auto TEMPORARY_STAT__SetLeft = reinterpret_cast<void(__thiscall*)(CTemporaryStatView::TEMPORARY_STAT*, int)>(0x0075DA00);

void __fastcall TEMPORARY_STAT__SetLeft_hook(CTemporaryStatView::TEMPORARY_STAT* pThis, void* _EDX, int tNewLeft) {
    int tDelta = g_tLastUpdate < 0 ? -30 : g_tLastUpdate - CWvsApp::GetInstance()->m_tUpdateTime;
    TEMPORARY_STAT__SetLeft(pThis, pThis->tLeft + tDelta);
}


void AttachTemporaryStatMod() {
    ATTACH_HOOK(CTemporaryStatView__Clear, CTemporaryStatView__Clear_hook);
    ATTACH_HOOK(CTemporaryStatView__Show, CTemporaryStatView__Show_hook);
    ATTACH_HOOK(CTemporaryStatView__Hide, CTemporaryStatView__Hide_hook);
    ATTACH_HOOK(CTemporaryStatView__Update, CTemporaryStatView__Update_hook);
    ATTACH_HOOK(CTemporaryStatView__AdjustPosition, CTemporaryStatView__AdjustPosition_hook);
    ATTACH_HOOK(CWvsContext__SetSkillCooltimeOver, CWvsContext__SetSkillCooltimeOver_hook);
    ATTACH_HOOK(CWvsContext__RemoveSkillCooltimeOver, CWvsContext__RemoveSkillCooltimeOver_hook);
    ATTACH_HOOK(TEMPORARY_STAT__UpdateShadowIndex, TEMPORARY_STAT__UpdateShadowIndex_hook);
    PatchCall(0x0075DCCC, reinterpret_cast<uintptr_t>(&TEMPORARY_STAT__SetLeft_hook)); // CTemporaryStatView::Update
}