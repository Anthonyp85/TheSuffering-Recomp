# Project state and coding-tool handoff

Updated: 2026-11-27. This is the shared resume document for Gemini/Antigravity, Claude, and Codex. Read `AGENTS.md` for architecture, protected game media, SDK patch workflow, and build requirements. Existing `.local/docs_private/` notes are historical and may describe older releases.

## Current status

**`v1.6.2` is merged, tagged, audited, and published on GitHub Releases.**

- **Repository:** `https://github.com/Anthonyp85/TheSuffering-Recomp` (`PUBLIC`, default branch `main`, remote `origin` restored).
- **Released commit (`v1.6.2`):** `f398fda5c1b74102dc647b51013bb47690137c31` (merge of PR #4 `https://github.com/Anthonyp85/TheSuffering-Recomp/pull/4`, preserving fix commit `5876e4c07c98724719d3d1d5d9f3d1b8d6d85b75` and version-bump commit `c721c353e0a567c64664577e8177d041862e562d`).
- **Annotated tag:** `v1.6.2` (`acbd85c0cf9fecbb229ea19b6099fa267eb89a63` -> `f398fda5c1b74102dc647b51013bb47690137c31`, untouched by this follow-up docs update).
- **Published GitHub Release:** `https://github.com/Anthonyp85/TheSuffering-Recomp/releases/tag/v1.6.2`
- **Pinned SDK:** `XboxXBERecomp` @ `cc9edda3a821a4204dc28783f429b88a27ade4f9` with exact byte-for-byte parity in `patches/xboxxberecomp.patch` (pure LF).

### Published `v1.6.2` release archives (verified against GitHub downloads)

| Archive | Size (Bytes) | SHA-256 |
| --- | ---: | --- |
| `TheSuffering-Recomp-v1.6.2-win64-launcher.zip` | `196,082,708` | `8e2f2e97bb17ac491a8da438fff95239f4c0d0017f4b7911a6552fe676379584` |
| `TheSuffering-Recomp-v1.6.2-linux-x86_64.tar.gz` | `71,800,954` | `de6dde467b521bbecf1677bca68da0cca3936a4873830d64edcd1a4a8720497c` |
| `TheSuffering-Recomp-v1.6.2-source.zip` | `705,419` | `dbcc437b1678ab98926943ee5d3911ef22c563d888c4fe597c5617e2b96676e8` |

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

## Validation record (`v1.6.2`, 2026-11-27)

- **GitHub Actions CI (PR #4 head `c721c35`):** **PASS** — `https://github.com/Anthonyp85/TheSuffering-Recomp/actions/runs/46105770915` (Linux `x86_64` Ubuntu 22.04 `140858044080` + Windows `x64` `140858044269`); Codex PR code review completed with zero findings.
- **GitHub Actions CI (merged `main` `f398fda`):** **PASS** — `https://github.com/Anthonyp85/TheSuffering-Recomp/actions/runs/46106278708` (Linux `x86_64` Ubuntu 22.04 `140859636873` + Windows `x64` `140859637241`). The published `TheSuffering-Recomp-v1.6.2-linux-x86_64.tar.gz` was sourced directly from this run's Ubuntu 22.04 artifact.
- `python -u tools/test_review_fixes.py`: **PASS**. Compiles and exercises production gameplay helpers (`ClothAccumulator`, slider commit/flush, reserved-key policy), controller-only recording/parser and cheat/stair guards, SDK D3D12/Vulkan resize & Vulkan subpass dependency & CLI-override function bodies (using GPU-call doubles, not live Vulkan GPU validation), and the production Windows updater translation unit against disposable folders with Unicode and shell metacharacters.
- `python -u tools/test_input_recording.py --check-regression`: **PASS** (`6` failures on old logic, `0` failures on final code, including legacy `#TSREC 1` samples).
- `cmd /c build.bat --no-prompt` & `cmd /c release.bat` (at `f398fda`): **PASS**. Built Windows x64 Release `suffering.exe` and `suffering.dll` (`v1.6.2`, LLVM Clang `23.1.6`, `-O3 -DNDEBUG`), verified and bundled the 4 official signed vendor upscaler DLLs + licenses into `TheSuffering-Recomp-v1.6.2-win64-launcher.zip` (`38` files, zero `suffering.dll`/`.xbe`/`.iso`).
- Clean isolated package smoke tests (Win64 zip & CI Ubuntu 22.04 Linux tarball): **PASS**. Extracted each archive to a clean temporary directory, inspected archive entries, verified `--validate-iso` (`valid=true`, `0x4D570015`) against the read-only ISO, and verified `--launcher --capture-launcher-bmp ... --launcher-exit` (`920x630`, `2,318,454` B on both Windows D3D12 and WSL2 Ubuntu 22.04 `xvfb-run` + `llvmpipe`).
- Isolated Windows live hybrid replay (`tests/m3_full_regression.rec`): **PASS** (`rc=0`, `113.969s`, `10,498` polls, `171` checkpoints, `deadzone_drift = 0.0`, `revert_err_from_spawn = 0.0`, `rumble_events = 1`, `0` crash dumps; protected media, original settings, and saves unchanged).
- Isolated Windows default-settings M&K + Menu Mouse + UI suite (`tools/test_mkb_controls.py`): **PASS** (Launcher `920x630` and in-game `F1` `1280x720` Controls captures, 1px micro-aim, 300px flick `0.0000 deg` coasting, invert-Y polarity, diagonal-normalized WASD `1.0000`, `F` flashlight, `WheelUp`/`WheelDown` pulses, `Tab`/`Esc` Archives & Pause, all 4 cursor capture states, and all 15 Menu Mouse & Click-Through checks, `0` crash dumps).
- Full 7-gate clean-tree release audit (`python -u tools/audit_release.py --allow-remote --build-dir out/build/release --build-dir out/build/linux-release` at `f398fda`): **PASS** across `gate1_git_history`, `gate2_head_hygiene`, `gate2b_binary_hygiene`, `gate3_source_zip`, `gate4_launcher_zip`, `gate4b_linux_tar_gz`, and `gate5_protected_media`.
- Published GitHub Release & updater metadata verification: **PASS**. Verified tag target (`f398fda`), `/releases/latest` JSON (`tag_name=v1.6.2`, `draft=false`, `prerelease=false`), and byte-for-byte SHA-256 parity of all 3 downloaded release assets.

### Remaining limitations & follow-up coverage

- **Live Vulkan GPU / validation-layer testing:** Renderer synchronization and resize fixes were verified with production-code unit/double harnesses and headless `xvfb-run` + `llvmpipe` launcher smoke tests; live hardware Vulkan 1.2 gameplay with Khronos validation layers remains follow-up coverage.
- **Physical-controller live recording & DualSense 5 hardware playtest:** Controller-only `#TSREC 1` recording was verified by automated regressions and legacy replay; live recording and DualSense 5 playtesting with a plugged-in physical controller remain follow-up coverage.
- **Interactive UI edge checks:** Manual interactive testing of every launcher/overlay control combination across platforms remains follow-up coverage.
- **Reddit material:** The updated `v1.6.2` `/r/decomps` Markdown draft was generated in `.local/dist/reddit_post_v1.6.2.md` following the project's established copy-paste draft convention; the live Reddit post itself was not edited directly.

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
