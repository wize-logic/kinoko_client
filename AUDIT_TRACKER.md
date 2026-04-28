# kinoko_client v95.1 Hook Audit — Findings Tracker

Audit performed 2026-04-27 against `/home/bcolb/GitHub/asdfstory/GMS_v95.1_U_DEVM.exe` IDB
using `idb_search.py` + `decomp.py`. Pseudocode evidence cached at
`/home/bcolb/GitHub/asdfstory/tools/decomp/cache/<rva>.c`.

Status legend: `[ ]` open · `[x]` fixed · `[~]` won't fix / by design · `[-]` refuted

---

## Load-bearing bugs (visible regressions)

- [x] **B1** — `CWvsContext::OnEnterField` hook drops post-enter setup
  - File: `src/bypass.cpp:351-357` · RVA: `0x009DBEC0`
  - Missing calls (none gated by anti-debug):
    - `CUIRaiseManager::RestoreWindows` (`0x83c5b0`) — minimized HUD never reappears
    - Party HP UI gated on `CConfig::GetShowPartyHP()` — never spawns
    - Party-search remocon state machine on `[this+0x37ac]` (`0x9da9a0`/`0x9daa70`/`StopPartySearch`)
    - Radio mute / `ShowUI(1)` (`0x4b2220`/`0x4b2210`/`0x4b2000`) gated by field type
    - Plus `0x4701c0`, `0x4b7ce0`, `0x009db0a0`, `0x4b2680`
  - Evidence: `cache/009dbec0.c:31-99`
  - Fixed: full reimplementation matches IDB disasm at 0x9DBFB6–0x9DC25F (anti-debug at +0x3EF8 / FUN_d62702 and the FUN_046d5XX/FUN_086dXXX fade-pair intentionally skipped). Added MEMBER_AT entries for `m_anPartyAdvertise[4]` (0x3798), `m_nPartyAdvertise_Mode` (0x37A8), `m_bPartyAdvertise_Apply` (0x37AC), `m_bRadio_Restore` (0x37B0), `m_nRadio_Volume` (0x37B4), `m_nMountKind` (0x41C5). ZRef<CUIRaiseManager> at +0x3EA8 accessed by direct deref.

- [x] **C2** — `CWvsApp` ctor hook never writes vtable `0xBAD460`
  - File: `src/bypass.cpp:31-88` · RVA: `0x009CA8A0`
  - Original first member init: `*pThis = &PTR_FUN_00bad460`
  - Safe today (no virtual dispatch on `CWvsApp*`), breaks any future virtual call
  - Fix: `*reinterpret_cast<uintptr_t*>(pThis) = 0x00BAD460;` at top of hook ✓

- [x] **C3** — `CWvsApp::SetUp` hook skips `ms_aIpAddr` table at `0xC68848`
  - File: `src/bypass.cpp:93-181` · RVA: `0x009CAFB0`
  - Original copies 0x200 bytes from `.text:0x401234` then overlays 10 dwords from `FUN_0045e2f0()` (= `InitSafeDll`, returns HINSTANCE)
  - Downstream readers couldn't be enumerated (.data-segment xrefs incomplete in IDB) — needs runtime verification
  - Evidence: `cache/009cafb0.c:127-137`
  - Fixed: `memcpy` + `InitSafeDll` overlay loop inserted right after `GetSEPrivilege`, preserving original ordering.

---

## Minor bugs

- [ ] **B2** — `walker.cpp:58-59` — `strftime(sTimeInfo, MAX_PATH, ...)` against 256-byte buffer
  - Fix: `sizeof(sTimeInfo)` instead of `MAX_PATH`

- [ ] **B3** — `resman.cpp::Dir_upDir:99-110` — `strlen(sDir) - 1` underflows to `SIZE_MAX` when string empty
  - Fix: early return when length is 0

- [ ] **B4** — `system.cpp::WndProc_hook:70-79` — `WM_LBUTTONUP` `wParam` (MK_* mask) compared against `HTCLOSE`/`HTMINBUTTON` (NC hit-test codes)
  - Fix: drop `WM_LBUTTONUP` from case or guard with `Msg == WM_NCLBUTTONUP`

---

## Correctness risks (fragile but working)

- [ ] **C1** — `helper.cpp:20` — `&static_cast<IWzVector2D*>(pThis->m_pvc)[-3]` silently relies on `sizeof(IWzVector2D)==4`
  - Fix: byte arithmetic — `reinterpret_cast<uint8_t*>(...) - 0xC`

- [ ] **C4** — `inlink.cpp:18-19` — `Find(L".img")` returning -1 yields `ReleaseBuffer(4)`
  - Fix: explicit early return on `Find < 0`

- [ ] **C6** — `system.cpp::WSPGetPeerName_hook:139-147` — only restores `sin_addr`, not `sin_port`
  - `WSPConnect_hook` rewrites both; restore should be symmetric

- [ ] **C7** — `resman.cpp:64-77` — `ZException` slice on throw after in-place `CTerminateException` ctor
  - Works because handler dispatches on hr value, not RTTI

- [ ] **C8** — `temporarystat.cpp:20` — `g_tsvCooltime` vptr is C++-emitted, not the game's `0xBAEC..`
  - Safe only because no called game code dispatches through it; add a header comment

- [ ] **C9** — `temporarystat.cpp:95-100` — symbol named `RemoveSkillCooltimeReset` but IDB says `RemoveSkillCooltimeOver`
  - Pure rename

---

## New findings from pseudocode

- [~] **NEW-1** — `CClientSocket::Connect` hook drops anti-tamper byte check on `Connect_inner` prologue (`004b0340.c:14-17` → `0x9c1960(8)` on mismatch). Intentional bypass.
- [~] **NEW-2** — `CWvsApp::SetUp` hook drops `GetIpAddrTable`/`GetAdaptersInfo` function-pointer self-check (`009cafb0.c:236-280`). Intentional bypass.
- [-] **NEW-3** — CRC-32 table at `0xC6F740` (poly `0xdd10ec81`) skipped. **HARMLESS** — only consumers are `Crc32_GetCrc32` (`0x009C0B00`), `Crc32_GetCrc32_VMCRC` (`0x009C1160`), `Crc32_GetCrc32_VMTable` (`0x009C1500`); all called only from `CWvsApp::Run` (`0x009C5F00`) which is wholesale-replaced by `CWvsApp__Run_hook` (`bypass.cpp:212-284`). Add a one-line comment near the SetUp hook noting the dependency.
- [ ] **NEW-4** — `CWvsApp` ctor hook may miss a third `ZXString` member ctor (`009ca8a0.c:56,61` — `FUN_00407350` invoked on `m_sCSDVersion`, `m_sCmdLine`, and a third slot at `param_1[0xb..0xc]`). Needs a closer read.

---

## Improvements

- [ ] **I1** — `hook.cpp:139-175` — `Patch1`/`Patch4`/`PatchStr`/`PatchNop` ignore `VirtualProtect` failures
- [ ] **I3** — `bypass.cpp:162` — duplicate `0x004B2300` cast; use `CConfig__ApplySysOpt` typed alias from `sysopt.cpp:105`
- [ ] **I5** — `injector.cpp:53` — `_strdup` leak (process-lifetime); `static char[16]` would do
- [ ] **I7** — `helper.cpp:103-128` — leading-space string literals in `get_attack_speed_string`; drop the spaces and let `%s (%d)` handle spacing

---

## Refuted findings (kept for the record)

- [-] **C5** — `CClientSocket::Connect` retry walking `lAddr` — original at `0x004B0340` is one-shot; no `posList` iteration exists.
- [-] **B5** — `CSkillInfo::LoadSkill` null-deref — failure paths are `__CxxThrowException` (noreturn). Result is either populated or unwinds.
- [-] **B6** — `UpdateShadowIndex` fires for every TEMPORARY_STAT — only one xref (`SetLeft+0x32`), gated by an "index changed" check.
