#include "pch.h"
#include "hook.h"
#include "debug.h"
#include "common/dbbasic.h"
#include "common/iteminfo.h"
#include "wvs/userlocal.h"
#include "wvs/util.h"
#include "ztl/ztl.h"
#include <memory>


struct GW_ItemSlotPet : GW_ItemSlotBase {
};


ZRef<GW_ItemSlotPet> GetActivePetItemSlot(CUser* pUser, int nIndex) {
    ZRef<GW_ItemSlotPet> pItemSlot;
    reinterpret_cast<ZRef<GW_ItemSlotPet>*(__thiscall*)(CUser*, ZRef<GW_ItemSlotPet>*, int)>(0x008E3030)(pUser, std::addressof(pItemSlot), nIndex);
    return pItemSlot;
}

void __fastcall CItemInfo__DrawItemIconForSlot_helper(CItemInfo* pThis, void* _EDX, GW_ItemSlotBase* pItem, IWzCanvasPtr pCanvas, int nItemID, int x, int y, int bProtectedItem, int bMag2, int bPetDead, int bHideCashIcon, int nEquipItemQuality, int bHideQualityIcon, int nMagSize) {
    int nPetIndex = -1;
    if (pItem->GetType() == 3) {
        for (int i = 0; i < 3; ++i) {
            auto pPetItemSlot = GetActivePetItemSlot(CUserLocal::GetInstance(), i);
            if (pPetItemSlot == pItem) {
                nPetIndex = i;
                break;
            }
        }
        if (nPetIndex >= 0) {
            pCanvas->DrawRectangle(x + 1, y - 31, 31, 31, 0xFFBBCCDD);
        }
    }
    pThis->DrawItemIconForSlot(pCanvas, nItemID, x, y, bProtectedItem, bMag2, bPetDead, bHideCashIcon, nEquipItemQuality, bHideQualityIcon, nMagSize);
    if (nPetIndex == 0) {
        IWzCanvasPtr pBossPetIcon = get_unknown(get_rm()->GetObjectA(L"UI/UIWindow.img/Item/bossPetIcon"));
        pCanvas->CopyEx(x - 1, y - 37, pBossPetIcon, CANVAS_ALPHATYPE::CA_REMOVEALPHA, 0, 0, 0, 0, 0, 0);
    }
}


static auto CItemInfo__DrawItemIconForSlot_jmp = 0x007CD3FB;
static auto CItemInfo__DrawItemIconForSlot_ret = 0x007CD400;
void __declspec(naked) CItemInfo__DrawItemIconForSlot_hook() {
    __asm {
        push    esi ; GW_ItemSlotBase*
        call    CItemInfo__DrawItemIconForSlot_helper
        jmp     [ CItemInfo__DrawItemIconForSlot_ret ]
    }
}

void AttachIconIconMod() {
    PatchJmp(CItemInfo__DrawItemIconForSlot_jmp, reinterpret_cast<uintptr_t>(&CItemInfo__DrawItemIconForSlot_hook)); // CUIItem::Draw
}
