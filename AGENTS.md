# AGENTS.md — The Suffering Static Recompilation

Guidance for coding agents working in this repository.

## Resume across coding tools

Read [docs/PROJECT_STATE.md](docs/PROJECT_STATE.md) before continuing work. It records the latest changes, verification commands, limitations, and remaining checks. This file is the shared project guidance for Codex, Gemini/Antigravity, and Claude; `GEMINI.md` and `CLAUDE.md` import it.

At session start, inspect `git status --short` and the current branch/worktree. Preserve existing changes. At session end, update `docs/PROJECT_STATE.md` with material changes and actual test results, including failures or checks not run. Keep durable decisions and resume information in tracked files; `.local/docs_private/` contains older, machine-local historical notes and is not the current handoff.

Use repository-relative paths in shared documentation. Do not assume another tool can access a Codex chat, an Antigravity artifact, a Claude memory directory, or this machine's dependency caches. A different checkout must receive the code changes and these documents together before resuming.

---

## 1. Project Overview & Scope

- **Goal:** Unofficial, clean-room static recompilation of **The Suffering (USA)** — Original Xbox (`Title ID 0x4D570015` / `MW-021`, Surreal Software / Midway Games, Riot Engine, XDK `1.0.5788.1`, sole executable `/Default.xbe`) into native 64-bit PC executables:
  - **Windows x64:** Direct3D 12, XAudio2, SDL3 (`HIDAPI` + WGI), Win32 Raw Input, Dear ImGui, 6 built-in HLSL upscalers + 3 dynamic vendor temporal plugins (DLSS 4, FSR 3.1/4, XeSS).
  - **Native Linux `x86_64` & Steam Deck (SteamOS 3.x):** Vulkan 1.2, SDL3 Video/Audio/Input, ELF64 (`glibc 2.35` / `GLIBCXX_3.4.30` baseline), 6 built-in Vulkan spatial upscalers, Gamepad/Touch XISO File Browser, Steam `shortcuts.vdf` installer.
- **SDK:** Built strictly on the pinned **XboxXBERecomp** submodule (`XboxXBERecomp/` @ `cc9edda3a821a4204dc28783f429b88a27ade4f9`, MIT) plus `patches/xboxxberecomp.patch`.
- **Architecture Rule:** Original Xbox 32-bit x86 XBE (`XBEH`) only. Never use Xbox 360 tools or formats (`rexglue`, XenonRecomp, XEX, PowerPC), and never replace static recompilation with an emulator or CPU interpreter.
- **Supported USA Retail XBE SHA-1s:**
  - `extract-xiso` (`0xEB`): `0a4c4e597da1f3b154099ab432c1326cc3dcb08c`
  - Unpatched Redump (`0x7D`): `55c902f622047484bc6c7ea4b1b1d714da2096fc`
- **Current Release & License:** `v1.6.2` (`src/common/version.h`). All original code in this repo is **`GPL-2.0-or-later`** with the Runtime Upscaler Plugin Exception (`LICENSE`). Third-party notices live in `THIRD_PARTY_NOTICES.md`.

---

## 2. Hard Safety, Legal & Privacy Invariants

1. **Source Media Is Strictly Read-Only:** Never modify, move, rename, overwrite, or delete local disc images (`.xiso.iso` / `.iso`), `.7z` archives, or companion files under `Suffering, The (USA)/`. Always open disc images read-only (`rb`). Runtime mounts the `.xiso.iso` directly in place as `D:\` without full-disc extraction; build-time tooling extracts only `default.xbe` into git-ignored `.local/xbe/default.xbe`.
2. **Zero Copyrighted, Generated, or Binary Artifacts in Git:** Never track or stage `.iso`, `.xiso`, `.xbe`, `.7z`, extracted game assets (`.vdu`, `.sdu`, `.tdu`, `.mdu`, `.adu`, `.ldu`, `.lvl`, `.odu`, `.wdu`, `.qdu`, `.xdu`, `.fdu`, `.xpu`, `.gdf`), `generated/`, `out/`, `.local/`, `cache/`, `hdd/`, `logs/`, `CrashDumps/`, `*.dll`, `*.exe`, `*.so`, `*.lib`, `*.pdb`, `*.dmp`, or Ghidra databases (`*.gpr`, `*.pe`). Max tracked blob size is `512 KiB` (`1536 KiB` for `patches/*.patch`).
3. **Zero Host/Personal Path Leaks (Source & Binaries):**
   - Never write personal usernames, user-profile paths, or absolute local checkout paths into tracked files. Use repo-relative paths or placeholders (`<path-to-iso>`).
   - Never remove or weaken the `-ffile-prefix-map` / `-ffile-compilation-dir=.` / `/Brepro` / `/PDBALTPATH:%_PDB%` / `--build-id=sha1 --strip-debug` prefix-remapping block in `CMakeLists.txt` (it must run **before** `FetchContent_MakeAvailable()`).
   - Verify both text and compiled PE/ELF/COFF binaries with `tools/audit_release.py` (`gate2_head_hygiene` + `gate2b_binary_hygiene`).
4. **Never Build or Launch While the Game Is Running:** Before compiling or running automated launch tests, verify no `suffering*` process is active (`Get-Process -Name 'suffering*' -ErrorAction SilentlyContinue` on Windows; `pgrep -f suffering` on Linux).
5. **No Unapproved Publishing:** Never run `git push`, create/modify GitHub releases, or push tags unless the user explicitly authorizes it.
6. **Authentic Behavior & Replay Determinism:** Preserve `1.00x` simulation/physics/animation speed across `30/60/90/120/165+` FPS. Gate gameplay enhancements (M&K aim, menu mouse, Main Menu Quit, Cheats, `[game_options]` persistence, staircase fixes) behind `!suffering::IsPadOnlyInputSession()`. This includes recording, replay, and autotest so both sides of the TSREC1 contract use the same guest behavior. Recording is explicitly controller-only; preserve saved input preferences and the on-screen recording notice. Keep legacy recordings compatible.

---

## 3. Repository Layout

```text
.
├── CMakeLists.txt               # Top-level build, path-privacy remapping, XBE extraction, host/game target setup
├── CMakePresets.json            # Ninja + Clang presets: debug, release, relwithdebinfo, linux-*, linux-cross-*
├── build.bat                    # Windows one-shot build (XBE extract -> setup_sdk -> xbe_symbols -> release build)
├── release.bat                  # Windows redistributable launcher zip (stages signed vendor upscalers; excludes suffering.dll)
├── release_linux.sh             # Native Linux/Steam Deck .tar.gz packager (bundles compiler/lib/*.so + $ORIGIN RPATH)
├── setup_sdk.bat / setup_sdk.sh # Idempotent XboxXBERecomp submodule init + patches/xboxxberecomp.patch application
├── config/game.toml             # Recompiler config (Title ID 0x4D570015, XDK 5788, 64 partitions, 136 symbols, 11 globals)
├── patches/xboxxberecomp.patch  # Exact LF unified diff against XboxXBERecomp @ cc9edda3
├── XboxXBERecomp/               # Pinned SDK git submodule (ignore = dirty in .gitmodules)
├── src/
│   ├── common/                  # Cross-platform host runtime glue (XBE_HOST_SOURCES)
│   │   ├── version.h            # Single source of truth for SUFFERING_VERSION_* & GitHub repo URLs
│   │   ├── game_info.cpp        # app::kGame descriptor, TOML [game_options] load/save, Optimus/PowerXpress exports
│   │   ├── launcher_ui.{h,cpp}  # 6-tab Pre-Game Graphical Launcher (Win32 D3D12 / Linux SDL_Renderer)
│   │   ├── settings_ui.{h,cpp}  # In-game F1 / F2 / BACK+L3 Dear ImGui overlay & FPS/Upscaler/Cheats HUD
│   │   ├── ui_common.{h,cpp}    # Shared panels, RAII ImGui guards, DPI fonts, hardware presets, update UI cards
│   │   ├── file_browser_ui.*    # Gamepad + Touch + Mouse Dear ImGui XISO picker (Linux / Steam Deck microSD quick-jump)
│   │   ├── mkb_input.{h,cpp}    # Native-PC Mouse & Keyboard engine (Win32 Raw Input / SDL3 relative mouse, 18 bindings)
│   │   ├── input_recorder.*     # Deterministic #TSREC 1 input recorder/replayer & state logger
│   │   └── update_checker.*     # Async GitHub Releases checker, SHA-256 verifier & staged self-updater
│   ├── game/                    # Suffering-specific strong X86_FUNC guest hooks (GAME_SOURCES; excluded from XBE_HOST_SOURCES)
│   │   ├── hooks.cpp            # M&K look, menu mouse engine, Main Menu Quit, CUserSettings sync, CClock, 60 Hz cloth, XMV/SRSC yields, Cellblocks Dallas guards
│   │   ├── cheats.{h,cpp}       # Thread-safe F2 Cheat Menu backend (9 toggles, 13 queued Guest Main Thread actions)
│   │   ├── stairs_fix.cpp       # 4a/4b Quarry & 10 Docks staircase walkability, step-up, SweepStep & anti-wedge hooks
│   │   └── guest_settings.h     # Mirror of CUserSettings (0x002DB218) persisted under [game_options]
│   ├── upscale/                 # Modern upscaling suite
│   │   ├── upscaler_common.*    # Shared quality/resolution math & names (all platforms)
│   │   ├── upscaler_manager.*   # Windows D3D12 upscaler orchestrator, Halton jitter, motion vectors, NGX fallback
│   │   ├── upscaler_spatial.cpp # Windows built-in HLSL shaders (FSR 1.0, NIS v1.0.3, CAS, Bicubic, Integer, Bilinear)
│   │   ├── upscaler_{dlss,fsr3,xess}.cpp & {ngx,ffx,xess}_abi.h # Clean-room C-ABI dynamic loaders (Windows only)
│   │   └── upscaler_vulkan.cpp  # Linux Vulkan 1.2 built-in spatial upscalers (excluded on Windows)
│   └── res/                     # suffering.rc, suffering.manifest (PerMonitorV2), suffering.ico, suffering.png
├── packaging/steamdeck/         # install-steamdeck.sh (binary shortcuts.vdf + .desktop) & .desktop template
├── tests/                       # m2_gameplay.txt, m3_full_regression.rec, m3_regression_source.txt
├── tools/                       # audit_release.py, extract_xbe.py, fetch_upscalers.py, gen_icon.py, test_*.py, run_*.py
└── .github/workflows/build.yml  # Ubuntu 22.04 (glibc 2.35) + Windows 2022 CI (no copyrighted XBE required)
```

**Git-ignored local directories (never commit):**
- `.local/` — privately extracted `.local/xbe/default.xbe`, cached vendor DLLs `.local/vendor_sdks/`, packaged archives `.local/dist/`, private notes `.local/docs_private/{PROJECT_MEMORY.md,handoff.md}`, and test captures/audits.
- `generated/` — C++ lifted from `default.xbe` by `xbe_recomp` (`generated/game/`).
- `out/` — CMake build trees (`out/build/<preset>/`).
- `Suffering, The (USA)/` — read-only user disc image.

---

## 4. XboxXBERecomp Submodule & Patch Workflow

`XboxXBERecomp/` is pinned to `cc9edda3a821a4204dc28783f429b88a27ade4f9` and patched locally via `patches/xboxxberecomp.patch`.

- **Where to put changes:** Prefer `src/game/` and `src/common/` for anything specific to *The Suffering*. Only modify `XboxXBERecomp/` for generic runtime, GPU (D3D12/Vulkan), audio, kernel, input, or recompiler improvements — and keep Windows and Linux backends in parity.
- **Bootstrap / apply patch idempotently:**
  - Windows: `setup_sdk.bat`
  - Linux: `bash setup_sdk.sh`
- **Adding new files inside `XboxXBERecomp/`:**
  1. Intent-add each new file so `git diff` includes it: `git -C XboxXBERecomp add -N <relpath>`
  2. Add the same `<relpath>` to the `git add -N` list in **both** `setup_sdk.bat` and `setup_sdk.sh`.
  3. Never leave untracked (`??`) files in `XboxXBERecomp/` — `tools/audit_release.py` Gate 2 rejects them.
- **Exporting the patch (MUST be UTF-8 LF — never use PowerShell `>` redirection):**
  ```powershell
  $patch_path = Join-Path (Get-Location) "patches/xboxxberecomp.patch"
  git -C XboxXBERecomp diff --output="$patch_path"
  ```
- **Parity Requirement:** `tools/audit_release.py` Gate 2 enforces an exact byte-for-byte match between `git -C XboxXBERecomp diff` and `patches/xboxxberecomp.patch`.

---

## 5. Build, Package & Test Commands

### Windows x64 (Clang + LLD + Ninja via VS 2022 Build Tools)
```bat
rem Full Release build (extracts .local\xbe\default.xbe read-only, applies SDK patch, builds suffering.exe + suffering.dll)
set "XBE_ISO_PATH=<path-to-Suffering, The (USA).xiso.iso>"
build.bat --no-prompt

rem Package clean Win64 launcher zip (stages pinned signed upscalers into plugins\, excludes suffering.dll & *.pdb)
release.bat
```
Direct CMake presets (inside VS x64 dev shell with `clang`/`clang++`/`lld-link`/`ninja`/`cmake`/`rc` in `PATH`):
```bat
cmake --preset release                  rem or: debug | relwithdebinfo
cmake --build out/build/release --parallel
```
CI / headers-only build without a local XBE/ISO:
```bat
call setup_sdk.bat
cmake --preset release -DSUFFERING_ALLOW_NO_XBE=ON
cmake --build out/build/release --parallel
```

### Native Linux `x86_64` & Steam Deck (Ubuntu 22.04 / `glibc 2.35` baseline)
```bash
bash setup_sdk.sh
cmake --preset linux-release -DSUFFERING_ALLOW_NO_XBE=ON   # omit -DSUFFERING_ALLOW_NO_XBE=ON if local XBE/ISO present
cmake --build out/build/linux-release --parallel

# Package self-contained Linux & Steam Deck tarball (bundles compiler/lib/*.so with $ORIGIN RPATH)
bash release_linux.sh \
  --build-dir out/build/linux-release \
  --out-tar .local/dist/TheSuffering-Recomp-v1.6.2-linux-x86_64.tar.gz

# Headless Pre-Game Launcher smoke test (Xvfb + lavapipe)
SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  timeout 90 xvfb-run -a -s "-screen 0 1280x800x24" \
  ./suffering --launcher --capture-launcher-bmp .local/ci_launcher.bmp --launcher-exit
```

### Audit & Verification Suites
Run `audit_release.py` after any code, CMake, packaging, or SDK patch change:
```powershell
# Full 6-gate audit (Git history, HEAD/working-tree hygiene, binary path-leak scan, zips, Linux tar, media)
python -u tools/audit_release.py --allow-remote

# Fast working-tree + binary hygiene check during development (before building release archives)
python -u tools/audit_release.py --allow-remote --allow-dirty --skip-zips --skip-linux-tar
```
Windows runtime verification suites (require local XISO, Release build, and zero running `suffering*` processes):
```powershell
python tools/test_review_fixes.py                      # Asset-free regressions for the eight review fixes (C++20 compiler required)
python -u tools/run_regression.py                      # 9-pass deterministic replay regression (Release warm/cold-Z + Debug)
python -u tools/test_mkb_controls.py                   # M&K 1:1 aim, menu mouse, WASD, actions, cursor capture & UI
python -u tools/test_v11_launcher_upscalers.py         # Pre-Game Launcher, 6 built-in upscalers, C-ABI mocks, cross-hardware
python -u tools/test_real_upscalers.py                 # Live DLSS 4, FSR 3.1, XeSS & NGX driver fallback
python -u tools/test_m5_launcher.py                    # XISO validator, PE resources, shortcut, release.bat & ImGui
python -u tools/run_perf_benchmark.py --phase postopt  # 11-metric frame pacing benchmark
```
Vendor upscaler plugin fetcher (stages SHA-256 & offline Authenticode-verified DLLs into `out/build/release/plugins/`):
```powershell
python -u tools/fetch_upscalers.py --cache-dir .local/vendor_sdks --stage-dir out/build/release/plugins
```

---

## 6. Coding Conventions & Hard-Won Pitfalls

### Licensing, Formatting & Line Endings
- **Mandatory SPDX Header (Gate 2 enforced):** Every `.h`, `.hpp`, `.c`, `.cpp`, `.py`, `.rc`, `.bat`, `.sh`, `.yml`, `.yaml`, `.desktop`, and `CMakeLists.txt` must contain `SPDX-License-Identifier: GPL-2.0-or-later` within its first 5 lines.
- **Line Endings (`.gitattributes`):** `*.bat` uses `CRLF`; `*.sh`, `*.desktop`, `*.yml`, `*.yaml`, and `*.patch` MUST use `LF` (Gate 4b fails if `install-steamdeck.sh` or `.desktop` contains `CRLF`).
- **Compiler Baseline:** C++20, `-march=x86-64-v2`, `-fno-strict-aliasing`, explicit `-O3 -DNDEBUG` in Release (`CMakeLists.txt` + `CMakePresets.json`). Code must build cleanly on **Ubuntu 22.04 Clang 14 / libstdc++-12** as well as modern Windows LLVM Clang (avoid incomplete-type issues in C++20 `std::vector<T>` / template instantiations).
- **Linux ABI Ceiling:** `./suffering` must link **only** LSB system libraries (`libc.so.6` <= `GLIBC_2.35`, `libm`, `libstdc++`, `libgcc_s`, `libdl`, `libpthread`, `librt`) with `-rdynamic -shared-libgcc` so cross-DSO `X86Resume` exceptions unwind cleanly and Mesa RADV `libstdc++` symbols never collide.

### Guest Hooks & Riot Engine (`src/game/`)
- **Disjoint Hook Ownership:** Keep strong `X86_FUNC(sub_XXXXXXXX)` definitions strictly non-overlapping across `src/game/hooks.cpp`, `src/game/cheats.cpp`, and `src/game/stairs_fix.cpp`.
- **Memory & FPU Safety:** Always validate guest pointers/spans with overflow-safe `ValidGuest(va, size)` before dereferencing guest memory. Always preserve x87 FPU state (`SavedX87State`) around nested `CallGuest` / `CallThiscall` calls into guest functions.
- **Thread Ownership:** Guest threads run pinned to one P-core (`single_core_guest = true`, `cs_aliases = {{0x002E1444u, 0x002E1A90u}}` in `src/common/game_info.cpp`). Never mutate guest objects from UI or render threads — enqueue actions and publish snapshots once per frame on Guest Main Thread (`sub_000BF6D0`).
- **Plot & Puzzle Protection:** Combat hooks (e.g., `OneHitKills` in `sub_000F1E80`) must exclude Torque, Dallas (`#2152`), plot-protected allies, environmental-puzzle bosses (`8b Bluff` `CHermes`, `CHorace`, `CKilljoy`), and sentinel-HP setpiece actors (`>= 5000 HP`).

### Dear ImGui, Launcher & Settings UI (`src/common/`)
- **RAII Stack Discipline:** Never call `ImGui::PopStyleColor()` / `PopStyleVar()` / `EndDisabled()` based on a mutable boolean that a widget (e.g., checkbox or "Disable All" button) could flip mid-scope. Always use RAII scope guards or snapshot `pushed` booleans before the widget call.
- **UI Hygiene:** Guard `ImGui::BeginTooltip()`, use `PushID`/`PopID` around repeated combo/button scopes, suppress `LB`/`RB` gamepad tab switching while modals or key-rebind capture are active, and use plain ASCII punctuation (no Unicode em-dashes in ProggyClean text).
- **Cross-Platform Dialogs & Sync:** On Windows, dispatch COM/shell dialogs to the STA main thread (`PostMessageW`); on Linux, use the in-app Dear ImGui `file_browser_ui` to avoid GTK/KDE portal hangs in Steam Deck Gaming Mode. Keep Launcher, `F1`/`F2` overlay, and `settings.toml` (`[controls]`, `[controls.bindings]`, `[game_options]`) synchronized 1:1.
- **Update Checker:** Restrict browser URLs to `https://github.com/Anthonyp85/TheSuffering-Recomp`, never call `system()`, verify SHA-256 before staging, preserve `settings.toml` and `hdd/` during self-update, and respect `ShouldAutoCheckForUpdates()` (overrideable via `SUFFERING_UPDATE_MOCK_JSON` in offline tests).

### Platform Source Filtering (`CMakeLists.txt`)
- `src/*/*.cpp` excluding `src/game/` forms `XBE_HOST_SOURCES`; `src/game/*.cpp` is added by `add_xbe_image(...)`.
- On Windows, `src/upscale/upscaler_vulkan.cpp` is excluded. On Linux, `src/upscale/upscaler_{manager,spatial,dlss,fsr3,xess}.cpp` are excluded. Follow this naming pattern if adding upscaler files.

---

## 7. Version Bump Checklist

Whenever bumping the release version (`X.Y.Z`), update all synchronized locations together:
1. `src/common/version.h` (`SUFFERING_VERSION_MAJOR/MINOR/PATCH/STR/TAG`)
2. `src/common/game_info.cpp` (`static_assert` version check + comment)
3. `src/res/suffering.rc` (`FILEVERSION`, `PRODUCTVERSION`, `FileVersion`, `ProductVersion`)
4. `src/res/suffering.manifest` (`assemblyIdentity version`)
5. `release_linux.sh` (`VERSION` default & `--help` text)
6. `tools/audit_release.py` (`DEFAULT_SOURCE_ZIP`, `DEFAULT_LAUNCHER_ZIP`, `DEFAULT_LINUX_TAR` & module docstring)
7. `.github/workflows/build.yml` (tarball paths & artifact names)
8. `README.md` (changelog & release archive filenames)
