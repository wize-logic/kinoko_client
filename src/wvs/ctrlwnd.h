#pragma once
#include "hook.h"
#include "ztl/ztl.h"
#include "wvs/gobj.h"
#include "wvs/msghandler.h"
#include "wvs/wnd.h"
#include <windows.h>
#include <cstdint>


typedef int32_t FONT_TYPE;

struct DRAGCTX {
    IUIMsgHandler* pContainer;
    ZRef<IDraggable> pObj;
};

static_assert(sizeof(DRAGCTX) == 0xC);


class CCtrlWnd : public IGObj, public IUIMsgHandler, public ZRefCounted {
protected:
    uint8_t padding[0x34 - sizeof(IGObj) - sizeof(IUIMsgHandler) - sizeof(ZRefCounted)];
    MEMBER_AT(uint32_t, 0x14, m_nCtrlId)
    MEMBER_AT(IWzVector2DPtr, 0x18, m_pLTCtrl)
    MEMBER_AT(CWnd*, 0x24, m_pParent)
    MEMBER_AT(int32_t, 0x28, m_bAcceptFocus)
    MEMBER_AT(int32_t, 0x2C, m_bEnabled)
    MEMBER_AT(int32_t, 0x30, m_bShown)

public:
    CCtrlWnd() {
        m_nCtrlId = -1;
        construct(&m_pLTCtrl);
        construct(&m_pParent); // m_pParent = nullptr;
        m_bAcceptFocus = 1;
        m_bEnabled = 1;
        m_bShown = 1;
    }
    explicit CCtrlWnd(int32_t nDummy) {
        ;
    }

    virtual ~CCtrlWnd() override {
        destruct(&m_pLTCtrl);
    }
    virtual void Update() override {
        ;
    }
    virtual int32_t OnDragDrop(int32_t nState, DRAGCTX& ctx, int32_t rx, int32_t ry) {
        return 0;
    }
    virtual void CreateCtrl(CWnd* pParent, uint32_t nId, int32_t l, int32_t t, int32_t w, int32_t h, void* pData) {
        reinterpret_cast<void(__thiscall*)(CCtrlWnd*, CWnd*, uint32_t, int32_t, int32_t, int32_t, int32_t, void*)>(0x004F0900)(this, pParent, nId, l, t, w, h, pData);
    }
    virtual void Destroy() {
        reinterpret_cast<void(__thiscall*)(CCtrlWnd*)>(0x004F0220)(this);
    }
    virtual void OnCreate(void* pData) {
        ;
    }
    virtual void OnDestroy() {
        ;
    }
    virtual int32_t HitTest(uint32_t rx, uint32_t ry) {
        return reinterpret_cast<int32_t(__thiscall*)(CCtrlWnd*, uint32_t, uint32_t)>(0x004EFD30)(this, rx, ry);
    }
    virtual void GetRect(RECT* pRect) {
        reinterpret_cast<void(__thiscall*)(CCtrlWnd*, RECT*)>(0x004EFE10)(this, pRect);
    }
    virtual void SetAbove(CCtrlWnd* pCtrl) {
        reinterpret_cast<void(__thiscall*)(CCtrlWnd*, CCtrlWnd*)>(0x004F0B50)(this, pCtrl);
    }
    virtual void Draw(int32_t rx, int32_t ry, const RECT* pRect) {
        ;
    }

    virtual void OnKey(uint32_t wParam, uint32_t lParam) override {
        m_pParent->OnKey(wParam, lParam);
    }
    virtual int32_t OnSetFocus(int32_t bFocus) override {
        if (bFocus) {
            return !!m_pParent;
        } else {
            return 1;
        }
    }
    virtual void OnMouseButton(uint32_t msg, uint32_t wParam, int32_t rx, int32_t ry) override {
        ;
    }
    virtual int32_t OnMouseMove(int32_t rx, int32_t ry) override {
        return 0;
    }
    virtual void OnMouseEnter(int32_t bEnter) override {
        return reinterpret_cast<void(__thiscall*)(IUIMsgHandler*, int32_t)>(0x004EFDE0)(this, bEnter);
    }
    virtual void SetEnable(int32_t bEnable) override {
        reinterpret_cast<void(__thiscall*)(IUIMsgHandler*, int32_t)>(0x004EFF80)(this, bEnable);
    }
    virtual int32_t IsEnabled() const override {
        return m_bAcceptFocus;
    }
    virtual void SetShow(int32_t bShow) override {
        reinterpret_cast<void(__thiscall*)(IUIMsgHandler*, int32_t)>(0x004EFDA0)(this, bShow);
    }
    virtual int32_t IsShown() const override {
        return m_bEnabled;
    }
    virtual int32_t GetAbsLeft() override {
        return reinterpret_cast<int32_t(__thiscall*)(IUIMsgHandler*)>(0x00471890)(this);
    }
    virtual int32_t GetAbsTop() override {
        return reinterpret_cast<int32_t(__thiscall*)(IUIMsgHandler*)>(0x004718F0)(this);
    }
};


class CCtrlComboBox : public CCtrlWnd {
public:
    struct CREATEPARAM {
        uint8_t padding[0x54];
        MEMBER_AT(int32_t, 0x0, nBackColor)
        MEMBER_AT(int32_t, 0x4, nBackFocusedColor)
        MEMBER_AT(int32_t, 0x8, nBorderColor)

        CREATEPARAM() {
            reinterpret_cast<void(__thiscall*)(CREATEPARAM*)>(0x004894F0)(this);
        }
        ~CREATEPARAM() {
            reinterpret_cast<void(__thiscall*)(CREATEPARAM*)>(0x0048A920)(this);
        }
    };
    static_assert(sizeof(CREATEPARAM) == 0x54);

    uint8_t padding[0x110 - sizeof(CCtrlWnd)];
    MEMBER_AT(int, 0x68, m_nSelect)

    CCtrlComboBox() : CCtrlWnd(0) {
        reinterpret_cast<void(__thiscall*)(CCtrlComboBox*)>(0x004DA090)(this);
    }
    virtual void CreateCtrl(CWnd* pParent, uint32_t nId, int32_t nType, int32_t l, int32_t t, int32_t w, int32_t h, void* pData) {
        reinterpret_cast<void(__thiscall*)(CCtrlComboBox*, CWnd*, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, void*)>(0x004DA330)(this, pParent, nId, nType, l, t, w, h, pData);
    }
    void AddItem(const char* sItemName, uint32_t dwParam) {
        reinterpret_cast<void(__thiscall*)(CCtrlComboBox*, const char*, uint32_t)>(0x004DE640)(this, sItemName, dwParam);
    }
    void SetSelect(int32_t nSelect) {
        reinterpret_cast<void(__thiscall*)(CCtrlComboBox*, int32_t)>(0x004D90E0)(this, nSelect);
    }
};

static_assert(sizeof(CCtrlComboBox) == 0x110);


// CCtrlEdit — single-line edit control (search box / chat box / etc).
// v95 ctor at 0x004DF870 (0xB4-byte object). CreateCtrl is the inherited
// CCtrlWnd::CreateCtrl thunk at 0x004F0900 — the v95 IGObj vtable for
// CCtrlEdit puts it at slot 2 (verified by dumping vtable @ 0xb4a66c).
// CREATEPARAM ctor at 0x004E09E0, dtor at 0x00484890; size unknown from
// pseudocode (highest written member +0x5C) so we round to 0x70 with
// internal padding. Used by CUIMonsterBook for the search box at id 0x7D5.
class CCtrlEdit : public CCtrlWnd {
public:
    struct CREATEPARAM {
        uint8_t padding[0x70];

        CREATEPARAM() {
            reinterpret_cast<void(__thiscall*)(CREATEPARAM*)>(0x004E09E0)(this);
        }
        ~CREATEPARAM() {
            reinterpret_cast<void(__thiscall*)(CREATEPARAM*)>(0x00484890)(this);
        }
    };
    static_assert(sizeof(CREATEPARAM) == 0x70);

    uint8_t padding[0xB4 - sizeof(CCtrlWnd)];

    CCtrlEdit() : CCtrlWnd(0) {
        reinterpret_cast<void(__thiscall*)(CCtrlEdit*)>(0x004DF870)(this);
    }
};

static_assert(sizeof(CCtrlEdit) == 0xB4);


class CCtrlButton : public CCtrlWnd {
public:
    struct CREATEPARAM {
        int32_t bAcceptFocus;
        int32_t bDrawBack;
        int32_t bAnimateOnce;
        ZXString<wchar_t> sUOL;

        CREATEPARAM() : bAcceptFocus(0), bDrawBack(0), bAnimateOnce(0) {
        }
        ~CREATEPARAM() {
        }
    };
    static_assert(sizeof(CREATEPARAM) == 0x10);

    uint8_t padding[0xADC - sizeof(CCtrlWnd)];

    CCtrlButton() : CCtrlWnd(0) {
        reinterpret_cast<void(__thiscall*)(CCtrlButton*)>(0x00471740)(this);
    }
    virtual void CreateCtrl(CWnd* pParent, uint32_t nId, int32_t l, int32_t t, int32_t decClickArea, void* pData) {
        reinterpret_cast<void(__thiscall*)(CCtrlButton*, CWnd*, uint32_t, int32_t, int32_t, int32_t, void*)>(0x004D77D0)(this, pParent, nId, l, t, decClickArea, pData);
    }
};

static_assert(sizeof(CCtrlButton) == 0xADC);


class CCtrlOriginButton : public CCtrlButton {
public:
    int32_t m_bChecked;
    IWzPropertyPtr m_pPropChecked;

    CCtrlOriginButton() = default;
};

static_assert(sizeof(CCtrlOriginButton) == 0xAE4);


class CCtrlCheckBox : public CCtrlWnd {
public:
    struct CREATEPARAM {
        uint8_t padding[0x34];
        MEMBER_AT(int32_t, 0x4, nBackColor)
        MEMBER_AT(ZXString<char>, 0x8, sText)
        MEMBER_AT(int32_t, 0x1c, nWidth)
        MEMBER_AT(int32_t, 0x20, nHeight)

        CREATEPARAM() {
            reinterpret_cast<void(__thiscall*)(CREATEPARAM*)>(0x00488990)(this);
        }
        ~CREATEPARAM() {
            reinterpret_cast<void(__thiscall*)(CREATEPARAM*)>(0x00484710)(this);
        }
    };
    static_assert(sizeof(CREATEPARAM) == 0x34);

    uint8_t padding[0x74 - sizeof(CCtrlWnd)];
    MEMBER_AT(int32_t, 0x48, m_bChecked)

    CCtrlCheckBox() : CCtrlWnd(0) {
        reinterpret_cast<void(__thiscall*)(CCtrlCheckBox*)>(0x00489250)(this);
    }
    virtual void CreateCtrl(CWnd* pParent, uint32_t nId, int32_t l, int32_t t, void* pData) {
        reinterpret_cast<void(__thiscall*)(CCtrlCheckBox*, CWnd*, uint32_t, int32_t, int32_t, void*)>(0x004D6F60)(this, pParent, nId, l, t, pData);
    }
    void SetChecked(int32_t bChecked) {
        reinterpret_cast<void(__thiscall*)(CCtrlCheckBox*, int32_t)>(0x004D4570)(this, bChecked);
    }
};

static_assert(sizeof(CCtrlCheckBox) == 0x74);


// CUIMonsterBook::CCtrlTab — KMST-only inner class, port of:
//   KMST 0x00845CA6  CCtrlTab::CreateCtrl(CWnd*, uint, RECT*, void*)
//   KMST 0x008469C0  CCtrlLPTab::CreateCtrl  — overrides + WZ tab tree load
//   KMST 0x008476ED  CCtrlRPTab::CreateCtrl  — overrides + WZ region tree load
//
// 0x48-byte object that KMST stamps with 3 hand-built vtables (IGObj /
// IUIMsgHandler / ZRefCounted) at +0/+4/+8 with CreateCtrl at IGObj slot 10
// and a 4-arg signature `(CWnd*, uint, RECT*, void*)`. kinoko's CCtrlWnd has
// CreateCtrl at IGObj slot 2 with a 7-arg signature, so naive single-vtable
// inheritance here would clobber the wrong slot. Solution: inherit from
// CCtrlWnd so kinoko's vtable shape stays valid, but expose a NEW non-virtual
// `CreateCtrlFromRect` method that callers invoke directly. Callers (= our
// own MonsterBook_CreateCtrl) reach CCtrlTab via a typed C++ pointer, never
// via vtable dispatch, so the slot mismatch never fires.
//
// The DrawLeftLayer port reads `*(this+0x1564)+0x34` to get the current tab
// index — that's pLPTab->m_nCurrentTab. Layout from KMST CCtrlTab member
// writes (CreateCtrl 0x00845CA6 + LPTab 0x008469C0):
//   +0x00..+0x33  CCtrlWnd base (0x34 bytes — kinoko's existing layout)
//   +0x34         m_nCurrentTab (int)
//   +0x38         m_nVisibleTab (int) — paint cursor offset; default 0
//   +0x3C         m_pSubLayer    (IWzGr2DLayer* raw COM ptr)
//   +0x40         m_pCanvas      (IWzCanvas* raw COM ptr)
//   +0x44         m_aTabs        (ZArray<ZRef<Info>> — per-tab visual data)
// Total: 0x48 bytes ✓ (matches KMST's `new(0x48)` allocation in
// CUIMonsterBook::CreateCtrl).
class CCtrlTab : public CCtrlWnd {
public:
    int32_t  m_nCurrentTab;        // +0x34
    int32_t  m_nVisibleTab;        // +0x38
    void*    m_pSubLayer;          // +0x3C — IWzGr2DLayer* raw COM ptr (Release in dtor)
    void*    m_pCanvas;            // +0x40 — IWzCanvas* raw COM ptr (Release in dtor)
    void*    m_aTabsPtr;           // +0x44 — ZArray<ZRef<Info>>::a (T*; count is at *(a-1))

    CCtrlTab() : CCtrlWnd(0),
                 m_nCurrentTab(0), m_nVisibleTab(0),
                 m_pSubLayer(nullptr), m_pCanvas(nullptr),
                 m_aTabsPtr(nullptr) {
    }
    // dtor lives in uimonsterbook.cpp where IUnknown is in scope.
    virtual ~CCtrlTab() override;

    // Adapter on CCtrlWnd::CreateCtrl. KMST CCtrlTab's CreateCtrl calls
    // CCtrlWnd::CreateCtrl with the rect's left/top/(width-1)/(height-1)
    // unpacked then builds an IWzGr2DLayer/IWzCanvas pair anchored on the
    // parent wnd's layer. Our port lives in uimonsterbook.cpp because the
    // body uses Ztl_variant_t / IWzGr2DLayerPtr / PcCreateObject helpers
    // that the wider mod includes; ctrlwnd.h stays standalone-includeable.
    void CreateCtrlFromRect(CWnd* pParent, uint32_t nId, const RECT* pRect, void* pParam);
};

static_assert(sizeof(CCtrlTab) == 0x48);


// CUIMonsterBook::CCtrlLPTab — left-page tab strip (9 area tabs).
// Override CreateCtrlFromRect with WZ-tree iteration that loads each area's
// per-frame canvases into a `CCtrlTab::Info` struct, then chains to base.
class CCtrlLPTab : public CCtrlTab {
public:
    CCtrlLPTab() = default;
    void CreateCtrlFromRect(CWnd* pParent, uint32_t nId, const RECT* pRect, void* pParam);
};

// CUIMonsterBook::CCtrlRPTab — right-page tab strip (regions per area).
class CCtrlRPTab : public CCtrlTab {
public:
    CCtrlRPTab() = default;
    void CreateCtrlFromRect(CWnd* pParent, uint32_t nId, const RECT* pRect, void* pParam);
};