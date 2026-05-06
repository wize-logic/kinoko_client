#pragma once

// CUIMonsterBook restoration for the v95.1 GMS client.
//
// The v95 binary phased out CUIMonsterBook construction: the ctor / vtable /
// OnCreate / internal builders are all stripped and CWvsContext::UI_Open
// (FUN_009d83f0) case 9 (MONSTERBOOK) is an empty break. We hook UI_Open
// to restore the case so /book actually pops a window.
//
// Phase 1 (this slice): the hook fires, allocates a buffer sized to the full
// CUIMonsterBook layout (0x18E0 bytes — derived from KMST v2.330's class
// extent + the 0x6A0 CUIWnd-base shift between KMST 0x468 and v95 0xB08),
// then runs v95 CUIWnd::CUIWnd + CreateUIWndPosSaved on it. No internal
// controls, no draw output yet. Phase 2 will add OnCreate / CreateCtrl /
// CreateLayer / CreateRect / CreateCardTable / CreateFontArray ports from
// KMST, plus the dtor / DAT_00c6f010 lifecycle so MonsterBookSetCard /
// SetCover packets can stop bailing on null and actually populate the UI.
//
// Layout / construction reference:
//   asdfstory/KMST/KMST_CUIMonsterBook_notes.md
//   asdfstory/tools/cuimonsterbook_xref.tsv

void AttachUIMonsterBook();
