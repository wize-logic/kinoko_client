# Kinoko

Kinoko is a client-side mod for the MapleStory **v95.1** GMS executable. It ships
two binaries:

- **`Kinoko.exe`** — a launcher that starts `MapleStory.exe` with `Kinoko.dll`
  injected via Microsoft Detours.
- **`Kinoko.dll`** — an injector that hooks `WvsClient` at runtime to bypass
  obsolete startup checks (HackShield, NMCO, ad balloons, Nexon IP probes,
  etc.), patch resolution/UI, fix several v95.1 bugs, and redirect the client
  to a custom server.

Kinoko is designed to talk to the [iw2d/kinoko](https://github.com/iw2d/kinoko)
private server, but anything speaking the v95.1 protocol will work.

---

## Prerequisites

- **Windows** (the target is a 32-bit Windows binary; cross-compiling is not
  supported).
- **Visual Studio 2022** with the *Desktop development with C++* workload
  (MSVC v143 + Windows 10/11 SDK).
- **CMake ≥ 3.14** (the version bundled with VS2022 is fine).
- **Git** with submodule support.
- A copy of the original **MapleStory v95.1** client (`MapleStory.exe` and its
  data files). Kinoko does **not** redistribute Nexon binaries.

---

## Getting the source

Clone with submodules — `external/Detours`, `external/StackWalker`, and
`external/WzLib` are all required:

```bat
git clone --recursive https://github.com/wize-logic/kinoko_client.git
cd kinoko_client
```

If you already cloned without `--recursive`:

```bat
git submodule update --init --recursive
```

---

## Building

The project ships two CMake presets (Win32 only — do **not** use x64):

| Preset           | Configuration |
|------------------|---------------|
| `debug-win32`    | Debug         |
| `release-win32`  | Release       |

### Quick build (recommended)

From a *Developer Command Prompt for VS 2022* (or any shell with `cmake` on
`PATH`):

```bat
compile.bat release-win32
```

`compile.bat` runs the configure + build steps for the chosen preset. Omit
the argument to default to `debug-win32`.

### Manual build

```bat
cmake --preset release-win32
cmake --build --preset release-win32
```

### Output

After a successful build the artifacts land in:

```
build\src\Release\Kinoko.exe
build\src\Release\Kinoko.dll
```

(or `build\src\Debug\` for the debug preset).

### Auto-copy to the client directory

If the `CUSTOM_BINARY_DIR` environment variable is set, both binaries are
copied there as a post-build step. Point this at your MapleStory v95.1
install directory and you can iterate without copying files by hand:

```bat
set CUSTOM_BINARY_DIR=C:\Games\MapleStory95.1
compile.bat release-win32
```

---

## Running

1. Place `Kinoko.exe` and `Kinoko.dll` next to `MapleStory.exe`.
2. Launch `Kinoko.exe` (it requires elevation — see the embedded manifest).
3. Pass the server address and port as command-line arguments:

   ```bat
   Kinoko.exe 127.0.0.1 8484
   ```

   With no arguments, Kinoko falls back to the build-time default
   (`CONFIG_SERVER_ADDRESS` in `src/config.h`, currently `127.0.0.1`).

The launcher creates `MapleStory.exe` suspended, injects `Kinoko.dll` via
`DetourCreateProcessWithDllExA`, then resumes the main thread.

### Windows 11 note

The launcher sets `__COMPAT_LAYER=WIN7RTM` in its environment before starting
the child, which suppresses the *Program Compatibility Assistant* dialog that
otherwise force-closes v95.1 MapleStory on Win10/11.

---

## Build configuration flags

A few feature toggles live in `src/config.h`:

| Macro                  | Effect                                                         |
|------------------------|----------------------------------------------------------------|
| `CONFIG_IMAGE_LOADING` | Enables loose image (`.png`) loading from disk in `resman.cpp` |
| `CONFIG_GLOBAL_FOCUS`  | Adds the "Global Focus" sound option to the system option UI   |

Both are commented out by default. Edit `src/config.h` and rebuild to flip
them.

---

## Releases (CI)

Pushing a tag to `main` triggers `.github/workflows/release.yml`, which builds
the `release-win32` preset on `windows-latest` and attaches `Kinoko.exe` and
`Kinoko.dll` to a GitHub Release. To cut a release:

```bat
git tag v95.1.x
git push origin v95.1.x
```

---

## Repository layout

```
src/                 — launcher + injector sources
  bypass.cpp           Hooks against WvsClient startup (CWvsApp::ctor/SetUp/Run)
  hook.cpp             Detours wrappers + patch helpers
  resman.cpp           Resource manager / image loading
  stringpool.cpp       String pool fixes + replacements
  sysopt.cpp           Resolution, system option dialog, screen layout
  temporarystat.cpp    TemporaryStat fixes
  ...
  wvs/                 In-game class layouts (CWvsApp, CClientSocket, etc.)
  ztl/                 ZTL allocator/container layouts
  common/              Misc helpers
external/            — submodules (Detours, StackWalker, WzLib)
.github/workflows/   — release CI
```

---

## License

The Kinoko sources in this repo are unlicensed unless stated otherwise per
file. Submodules under `external/` retain their upstream licenses
(Detours: MIT, StackWalker: BSD-2, WzLib: see upstream).
