# Project state and coding-tool handoff

Updated: 2026-11-27. This is the shared resume document for Gemini/Antigravity, Claude, and Codex. Read `AGENTS.md` for architecture, protected game media, SDK patch workflow, and build requirements. Existing `.local/docs_private/` notes are historical and may describe older releases.

## Current work

Release `v1.6.2` prepares and delivers all eight repository-review fixes on top of `v1.6.1` (`83ed39a`), while keeping the `XboxXBERecomp` SDK pinned to `cc9edda3a821a4204dc28783f429b88a27ade4f9` and carrying all SDK runtime changes in `patches/xboxxberecomp.patch`.

- Commit `5876e4c07c98724719d3d1d5d9f3d1b8d6d85b75` (*"Fix eight review findings and add shared coding-tool handoff"*) contains the eight fixes, regression tests, CI regression step additions, and shared coding-tool entry points (`AGENTS.md`, `GEMINI.md`, `CLAUDE.md`, `docs/PROJECT_STATE.md`).
- The `release/v1.6.2` branch preserves `5876e4c` and adds the synchronized `v1.6.2` version bump (`version.h`, `game_info.cpp`, `suffering.rc`, `suffering.manifest`, `release_linux.sh`, `tools/audit_release.py`, `.github/workflows/build.yml`, `AGENTS.md`, `README.md`, and this handoff).

## Fix record (`v1.6.2`)

| Finding | Result | Main code |
| --- | --- | --- |
| Vulkan attachment ordering | External render-pass dependencies make prior attachment writes visible to presentation/UI loads and ordinary color/depth pass reentry. Attachment-free passes avoid an empty dependency. | SDK `runtime/gpu/vulkan.cpp`, carried in `patches/xboxxberecomp.patch` |
| Resize leaves stale render targets | Both backends retain old render extents until `ApplySettings` can detect the change, invalidate targets, update scale, and reset history after the GPU wait. Vulkan retry keeps its live front target valid. | SDK `runtime/gpu/{d3d12,vulkan}.cpp` |
| Cloth runs too fast above 60 FPS | Each emitted 60 Hz step subtracts exactly the time sent to the guest; all cloth objects share one decision per guest frame. | `src/game/gameplay_input_state.h`, `src/game/hooks.cpp` |
| Slider drag back to its saved value persists an intermediate value | The current snapshot always follows the guest; equality with the saved value controls persistence only. | Same gameplay helper and hooks |
| Native mouse input cannot round-trip through TSREC1 | Recording explicitly uses controller-only input and shows a nonmodal HUD notice. Native mouse/keyboard, guest preference overrides, cheats, and stair assists are suppressed for recording as well as replay/autotest. Saved preferences and legacy TSREC1 recordings are preserved. | `src/common/input_recorder.*`, `mkb_input.cpp`, `settings_ui.cpp`, guest hooks/cheats/stairs |
| Explicit settings changes lose to CLI override guards | Display mode, resolution, skip-launcher edits, and Reset Defaults clear the corresponding temporary CLI guards. Unrelated saves still preserve temporary CLI behavior. | SDK settings markers and shared/launcher/settings UI |
| Reserved F2/F10 keys can be bound but never work | A shared key policy rejects host-reserved keys in both Win32 and SDL rebinding/input paths. | `src/common/mkb_input.*` |
| Windows updater breaks non-ASCII paths | An ASCII PowerShell helper decodes UTF-8 paths, uses literal filesystem operations, and launches via .NET without wildcard expansion. Existing settings, saves, media, and caches are preserved. | `src/common/update_checker.*` |

Recording native mouse/keyboard input would require a new recording format and matching replay hooks. `v1.6.2` deliberately retains the existing `#TSREC 1` controller format and makes its controller-only contract visible.

## Validation record (`v1.6.2` candidate, 2026-11-27)

- `python -u tools/test_review_fixes.py`: **PASS**. Compiles and exercises production gameplay helpers (`ClothAccumulator`, slider commit/flush, reserved-key policy), controller-only recording/parser and cheat/stair guards, SDK D3D12/Vulkan resize & Vulkan subpass dependency & CLI-override function bodies (using GPU-call doubles, not live Vulkan GPU validation), and the production Windows updater translation unit against disposable folders with Unicode and shell metacharacters.
- `python -u tools/test_input_recording.py --check-regression`: **PASS**. Confirmed old recording behavior fails (`6` failures) and current code passes (`0` failures), including legacy `#TSREC 1` samples.
- `cmd /c build.bat --no-prompt`: **PASS**. Rebuilt Windows x64 Release `out/build/release/suffering.exe` and `suffering.dll` (`v1.6.2`, LLVM Clang `23.1.6`, `-O3 -DNDEBUG`) and verified staged official vendor upscaler DLLs.
- Native Linux `x86_64` (WSL2 Ubuntu 22.04.5 LTS / Clang 14.0.0 / `glibc 2.35`): **PASS**. Configured `linux-release`, built `./suffering`, `./compiler/xbe_recomp`, and `libsuffering.so`, packaged `.local/dist/TheSuffering-Recomp-v1.6.2-linux-x86_64.tar.gz` via `release_linux.sh`, and passed the headless Pre-Game Launcher smoke test under `xvfb-run` + `llvmpipe` (`920x630` capture).
- Isolated Windows live hybrid replay (`tests/m3_full_regression.rec`): **PASS** against SHA-256-verified copies of the rebuilt `v1.6.2` binaries under `.local/original_verify_*`. Exit code `0`, wall time `113.969s`, `10,498` polls, `171` checkpoints, `deadzone_drift = 0.0`, `revert_err_from_spawn = 0.0`, `rumble_events = 1`, `0` crash dumps; protected media, original settings, and saves unchanged.
- Isolated Windows M&K + Menu Mouse + UI suite (`tools/test_mkb_controls.py` with default `settings.toml`): **PASS**. Verified Launcher (`920x630`) and in-game `F1` (`1280x720`) Controls captures, 1px micro-aim, 300px flick (`0.0000 deg` coasting), invert-Y polarity, diagonal-normalized WASD (`1.0000`), `F` flashlight, `WheelUp`/`WheelDown` pulses, `Tab`/`Esc` Archives & Pause, all 4 cursor capture states, and all 15 Menu Mouse & Click-Through Suppression checks (`0` crash dumps).
- Pre-archive hygiene & binary/tarball audit (`tools/audit_release.py` + `git diff --check`): **PASS** across `gate1_git_history`, `gate2_head_hygiene` (exact SDK patch parity, pure LF patch/scripts), `gate2b_binary_hygiene` (zero leaks across Windows & Linux build outputs), `gate4b_linux_tar_gz` (ELF64 / `GLIBC_2.35` / `$ORIGIN` RPATH), and `gate5_protected_media`.

### Remaining limitations & follow-up coverage

- **Live Vulkan GPU / validation-layer testing:** Renderer synchronization and resize fixes were tested with production-code unit/double harnesses and a headless `xvfb-run` + `llvmpipe` launcher smoke test; live hardware Vulkan 1.2 gameplay with Khronos validation layers remains follow-up coverage.
- **Physical-controller live recording & DualSense 5 hardware playtest:** Controller-only `#TSREC 1` recording was verified by automated regressions and legacy replay; live recording with a plugged-in physical controller remains follow-up coverage.
- **Interactive UI edge checks:** Manual interactive testing of every launcher/overlay control combination across platforms remains follow-up coverage.
- **Hosted GitHub Actions CI & final release archive audits:** Run via PR CI (`release/v1.6.2 -> main`), post-merge `main` CI, and final full-archive `audit_release.py` before tagging/publishing `v1.6.2`.

## Resume procedure

1. Inspect `git status --short`, `git branch --show-current`, and `git worktree list`. Preserve any uncommitted changes.
2. Read `AGENTS.md` and this handoff before making changes. Shared tracked files take precedence over stale local notes under `.local/docs_private/`.
3. SDK runtime changes live in `patches/xboxxberecomp.patch`, not a new submodule revision. On a fresh checkout, run `setup_sdk.bat` or `bash setup_sdk.sh`.
4. Verify no `suffering*` process is running before building or launching tests. Keep the user's disc image and save data untouched. Use `-DSUFFERING_ALLOW_NO_XBE=ON` for host-only builds without local game media.
5. After further work, update this document with exact commands, outcomes, and remaining limitations. Keep local paths, credentials, generated game code, and binary artifacts out of Git.

## Context entry points

- `AGENTS.md`: shared project guidance and invariants.
- `GEMINI.md`: Gemini context imports plus explicit instructions for Antigravity to read guidance and this state.
- `CLAUDE.md`: imports the same files for Claude.
- `README.md`: user-facing behavior and normal build instructions.
