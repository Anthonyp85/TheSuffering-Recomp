# Project state and coding-tool handoff

Updated: 2026-11-27. This is the shared resume document for Gemini/Antigravity, Claude, and Codex. Read `AGENTS.md` for architecture, protected game media, SDK patch workflow, and build requirements. Existing `.local/docs_private/` notes are historical and may describe older releases.

## Current work

All eight repository-review findings are fixed and verified as described below. The base is release `v1.6.1`, commit `83ed39a`; the SDK remains pinned to `cc9edda3a821a4204dc28783f429b88a27ade4f9`. No release bump, push, or publication was made.

The fixes, regression tests, and shared context files are committed together on `main` in the commit containing this update, titled `Fix eight review findings and add shared coding-tool handoff`. Use `git log -1 --format='%h %s' -- docs/PROJECT_STATE.md` to identify it. The review worktree and original checkout have matching source and patched SDK files. The original checkout's previous files were backed up under `.local/review_fix_backup_<UTC timestamp>/`, with a SHA-256 manifest.

The original checkout's Windows Release binaries have now been rebuilt from these sources. Build outputs, private assets, dependency caches, and test captures remain ignored local data; a fresh checkout still needs its own build.

## Fix record

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

Recording native mouse/keyboard input would require a new recording format and matching replay hooks. This change deliberately retains the existing controller format and makes its limitation visible.

## Validation record (2026-11-27)

- `python tools/test_review_fixes.py`: PASS. Compiles/exercises production gameplay helpers, recorder/parser and cheat/stair guards, SDK renderer/settings function bodies, and the production updater translation unit. Renderer tests use GPU-call doubles; they are not live GPU validation. Updater tests execute Windows PowerShell and robocopy against disposable folders with Unicode and shell metacharacters, verifying preservation, relaunch, cleanup, and copy-failure behavior.
- `python tools/test_input_recording.py --check-regression`: PASS. The old recording-selection/guard behavior fails the targeted checks; the final code passes, including legacy recording samples.
- Windows Release: PASS for both the host-only configuration and the final full build using a private local XBE. Built `suffering.exe` and `suffering.dll` with C++20/LLVM and the MSVC development environment.
- Linux x86_64 cross-build: PASS for the host-only configuration and the final full build, using LLVM and the existing Ubuntu 22.04 sysroot. Built the ELF host and guest library. This was a Windows-hosted cross-build, not a native Linux execution test.
- `python -u tools/run_regression.py --single release_warm`: PASS against the final Windows build and the read-only ISO via `XBE_ISO_PATH`. Clean exit after 115.891 seconds; 9,689 polls and 164 state checkpoints; measured deadzone drift `0.0` and revert-to-spawn error `0.0`; no new crash dumps. The harness verified protected media metadata before/after. This was one release replay, not the full nine-pass/debug matrix.
- `python -u tools/audit_release.py --allow-remote --allow-dirty --skip-zips --skip-linux-tar --skip-media`: PASS for history, source hygiene/SDK patch parity, and binary hygiene. Release archives were not built or audited. Media integrity was checked by the replay harness separately.
- `git diff --check`: PASS. SDK patch is LF-only and byte-identical to the initialized SDK's diff; forward/reverse applicability was checked.

The new aggregate regression command is included in both Windows and Linux CI jobs after SDK setup. Hosted CI has not been run for this change set. Live Vulkan validation layers, physical-controller recording, and interactive widget checks on both platforms remain follow-up coverage; the automated checks and live replay above do not claim those results.

Local review evidence is under `.local/review_*` and `.local/captures_m3/` in the review worktree. These ignored files are not part of the portable handoff. To reproduce elsewhere, initialize the SDK and run the aggregate test, then use the normal README/CMake presets; supply your own local ISO/XBE only for full builds and live replay. Never commit generated game code or assets.

## Original checkout verification before commit

The user requested rebuilding the original checkout, verifying it, and committing all fixes and shared handoff files. The following checks were run in the original checkout on 2026-11-27:

- `build.bat --no-prompt`: PASS. Recompiled affected production code and linked the original checkout's `out/build/release/suffering.exe` and `suffering.dll`. The normal build also staged and verified the cached vendor upscaler plugins.
- `python tools/test_review_fixes.py`: PASS for all applicable gameplay/input, recorder, renderer/settings, and Windows updater regression checks.
- Live hybrid replay of `tests/m3_full_regression.rec`: PASS using SHA-256-verified copies of those rebuilt binaries, copied plugins/settings, and fresh test save storage under `.local/original_verify_<suffix>/`. The existing `tools/run_regression.py` harness checked the full state sequence. Exit code `0`, wall time `116.656` seconds, `9,680` polls, `163` checkpoints, deadzone drift `0.0`, revert-to-spawn error `0.0`, and no new crash dumps. Original settings/save contents and metadata, executable/library hashes, and ISO metadata were unchanged by the replay.
- `python -u tools/audit_release.py --allow-remote --allow-dirty --skip-zips --skip-linux-tar`: PASS for source/history hygiene, SDK patch parity, built binary hygiene, and protected media checks.
- `git diff --check`: PASS; the SDK pin is unchanged and its full working diff exactly matches the committed patch.

Original-checkout logs are `.local/original_rebuild.log`, `.local/original_live_replay.log`, and `.local/original_audit.log`; replay results/captures are inside the isolated runtime directory. The ignored convenience runner can repeat that isolated check with `python .local/verify_original_runtime.py --repo .` on this machine. Portable verification remains the committed regression suite and replay harness; the convenience runner is not required project source.

## Resume procedure

1. Inspect `git status --short`, `git branch --show-current`, and `git worktree list`. Preserve any subsequent uncommitted changes and verify that the review-fix commit is present in the checkout being resumed.
2. Read the change and validation record below, then inspect the actual diff. Shared source and this handoff take precedence over stale tool-specific conversation summaries.
3. The SDK runtime changes are carried in `patches/xboxxberecomp.patch`, not a new submodule revision. On a fresh checkout, run `setup_sdk.bat` or `bash setup_sdk.sh`. If an older patch is already applied, preserve any independent SDK edits before updating it; do not blindly reset the SDK.
4. Check that no `suffering*` process is running before building or launching tests. Keep the user's ISO/archive and save data untouched. Use `SUFFERING_ALLOW_NO_XBE=ON` for host-only builds without game assets.
5. After further work, update this document with changes, exact reproducible validation commands, outcomes, and remaining limitations. Keep local paths, credentials, generated game code, and binary artifacts out of Git.

## Context entry points

- `AGENTS.md`: shared project guidance and invariants.
- `GEMINI.md`: Gemini context imports plus explicit instructions for Antigravity to read guidance and this state.
- `CLAUDE.md`: imports the same files for Claude.
- `README.md`: user-facing behavior and normal build instructions.

The entry points use the documented context mechanisms: [Gemini context files](https://geminicli.com/docs/cli/gemini-md/), [Antigravity rules](https://antigravity.google/docs/rules/), and [Claude project memory](https://code.claude.com/docs/en/memory). Repository files carry project context; they do not transfer private chat history or credentials.
