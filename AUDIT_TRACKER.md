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

- [x] **B2** — `walker.cpp:58-59` — `strftime(sTimeInfo, MAX_PATH, ...)` against 256-byte buffer
  - Fixed: now passes `sizeof(sTimeInfo)`.

- [x] **B3** — `resman.cpp::Dir_upDir:99-110` — `strlen(sDir) - 1` underflows to `SIZE_MAX` when string empty
  - Fixed: track length through trailing-slash trim, early return when zero.

- [x] **B4** — `system.cpp::WndProc_hook:70-79` — `WM_LBUTTONUP` `wParam` (MK_* mask) compared against `HTCLOSE`/`HTMINBUTTON` (NC hit-test codes)
  - Fixed: split `WM_LBUTTONUP` into its own case (just clears moving state); HT* checks now only run for `WM_NCLBUTTONUP`.

---

## Correctness risks (fragile but working)

- [x] **C1** — `helper.cpp:20` — `&static_cast<IWzVector2D*>(pThis->m_pvc)[-3]` silently relies on `sizeof(IWzVector2D)==4`
  - Fixed: now uses `reinterpret_cast<uint8_t*>(pThis->m_pvc) - 0xC`.

- [x] **C4** — `inlink.cpp:18-19` — `Find(L".img")` returning -1 yields `ReleaseBuffer(4)`
  - Fixed: stash `Find` result, early return when negative before `ReleaseBuffer`.

- [x] **C6** — `system.cpp::WSPGetPeerName_hook:139-147` — only restores `sin_addr`, not `sin_port`
  - Fixed: capture original `sin_port` in `WSPConnect_hook` (new `g_uOriginalPort`) and restore both fields in `WSPGetPeerName_hook`.

- [x] **C7** — `resman.cpp:64-77` — `ZException` slice on throw after in-place `CTerminateException` ctor
  - Documented in code: comment notes the handler dispatches on hr value, not RTTI.

- [x] **C8** — `temporarystat.cpp:20` — `g_tsvCooltime` vptr is C++-emitted, not the game's `0xBAEC..`
  - Documented in code: header comment on the global block notes the constraint.

- [x] **C9** — `temporarystat.cpp:95-100` — symbol named `RemoveSkillCooltimeReset` but IDB says `RemoveSkillCooltimeOver`
  - Fixed: renamed all 4 references.

---

## New findings from pseudocode

- [~] **NEW-1** — `CClientSocket::Connect` hook drops anti-tamper byte check on `Connect_inner` prologue (`004b0340.c:14-17` → `0x9c1960(8)` on mismatch). Intentional bypass — documented in code (bypass.cpp::CClientSocket__Connect_hook).
- [~] **NEW-2** — `CWvsApp::SetUp` hook drops `GetIpAddrTable`/`GetAdaptersInfo` function-pointer self-check (`009cafb0.c:236-280`). Intentional bypass — documented in code (bypass.cpp::CWvsApp__SetUp_hook).
- [-] **NEW-3** — CRC-32 table at `0xC6F740` (poly `0xdd10ec81`) skipped. **HARMLESS** — only consumers are `Crc32_GetCrc32` (`0x009C0B00`), `Crc32_GetCrc32_VMCRC` (`0x009C1160`), `Crc32_GetCrc32_VMTable` (`0x009C1500`); all called only from `CWvsApp::Run` (`0x009C5F00`) which is wholesale-replaced by `CWvsApp__Run_hook` (`bypass.cpp:212-284`). Add a one-line comment near the SetUp hook noting the dependency.
- [-] **NEW-4** — Refuted. `009ca8a0.c` has exactly two `FUN_00407350` calls (cache lines 56, 61), both already present in the hook for `m_sCSDVersion` (param_1[9] / +0x24) and `m_sCmdLine` (param_1[0xd] / +0x34). The `param_1[0xb..0xc]` slots are `m_tUpdateTime` and `m_bFirstUpdate` — plain int assignments, no ctor.

---

## Improvements

- [x] **I1** — `hook.cpp:139-175` — `Patch1`/`Patch4`/`PatchStr`/`PatchNop` ignore `VirtualProtect` failures
  - Fixed: each helper now logs via `DEBUG_MESSAGE` and bails when the RWX flip fails.
- [x] **I3** — `bypass.cpp:162` — duplicate `0x004B2300` cast; use `CConfig__ApplySysOpt` typed alias from `sysopt.cpp:105`
  - Fixed: added `CConfig::ApplySysOpt` method in `wvs/config.h` matching the `GetOpt_Int`/`SetOpt_Int` shape; bypass.cpp's SetUp hook now calls `pConfig->ApplySysOpt(nullptr, 0)`. The static raw-address pointer in sysopt.cpp stays — it's the trampoline used by the hook to invoke the original.
- [x] **I5** — `injector.cpp:53` — `_strdup` leak (process-lifetime); `static char[16]` would do
  - Fixed: copy into `static char s_sAddressBuf[16]` and point `g_sServerAddress` at it; preserves the null-pointer-means-default semantics.
- [x] **I7** — `helper.cpp:103-128` — leading-space string literals in `get_attack_speed_string`; drop the spaces and let `%s (%d)` handle spacing
  - Fixed: stripped leading space from each return value.

---

## Refuted findings (kept for the record)

- [-] **C5** — `CClientSocket::Connect` retry walking `lAddr` — original at `0x004B0340` is one-shot; no `posList` iteration exists.
- [-] **B5** — `CSkillInfo::LoadSkill` null-deref — failure paths are `__CxxThrowException` (noreturn). Result is either populated or unwinds.
- [-] **B6** — `UpdateShadowIndex` fires for every TEMPORARY_STAT — only one xref (`SetLeft+0x32`), gated by an "index changed" check.
