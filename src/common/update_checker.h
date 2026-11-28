// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "common/version.h"

namespace suffering {

enum class UpdateState : uint8_t {
    Idle = 0,
    Checking,
    UpToDate,
    UpdateAvailable,
    Downloading,
    ReadyToRestart,
    Error,
};

using UpdateCheckState = UpdateState;

const char* UpdateStateName(UpdateState state);

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    bool is_prerelease = false;
    std::string prerelease;
    bool valid = false;
};

struct UpdateStatusSnapshot {
    UpdateState state = UpdateState::Idle;
    bool manual_check = false;
    std::string current_version = SUFFERING_VERSION_STR;
    std::string latest_version;
    std::string latest_tag;
    std::string release_title;
    std::string release_notes_summary;
    std::string html_url = SUFFERING_RELEASES_URL;
    std::string asset_name;
    std::string asset_url;
    std::string asset_sha256;
    uint64_t asset_size = 0;
    uint64_t downloaded_bytes = 0;
    std::string status_message;
    bool can_self_update = false;
};

// SemVer + SHA-256 + offline JSON / staging script helpers.
bool ParseSemVer(const std::string& text, SemVer& out);
int CompareSemVer(const SemVer& a, const SemVer& b);
bool IsRemoteVersionNewer(const std::string& local_ver, const std::string& remote_tag_or_ver);
std::string ComputeSha256Hex(const void* data, size_t size);
UpdateStatusSnapshot EvaluateReleaseJson(const std::string& json_text, bool manual_check = false);
// Windows: ASCII PowerShell script with encoded Unicode path literals; Linux: POSIX shell script.
std::string BuildUpdateApplyScript(const std::filesystem::path& exe_dir,
                                   const std::filesystem::path& staged_root,
                                   const std::filesystem::path& staging_dir,
                                   uint32_t wait_pid);
#ifdef _WIN32
// ASCII PowerShell command that reads a script using its literal Unicode path.
std::string BuildUpdateApplyCommand(const std::filesystem::path& script_path);
#endif

// Returns false when headless test/CI flags (--launcher-exit, --capture-launcher-bmp, --replay, --record, etc.)
// are present in GetProcessArgsUtf8(), UNLESS SUFFERING_UPDATE_MOCK_JSON is set in the environment.
bool ShouldAutoCheckForUpdates();

// Starts an asynchronous GitHub release check (debounced if already Checking or Downloading).
// When SUFFERING_UPDATE_MOCK_JSON is set, evaluates the offline JSON deterministically before returning.
void StartUpdateCheck(bool manual);

// Thread-safe copy of the latest update status, including live downloaded_bytes progress while Downloading.
UpdateStatusSnapshot GetUpdateSnapshot();

// Opens an allowlisted https://github.com/Anthonyp85/TheSuffering-Recomp URL in the default browser
// (defaults to SUFFERING_RELEASES_URL when url is empty). Never uses system().
void OpenReleasePageInBrowser(const std::string& url = "");

// Starts asynchronous download, optional SHA-256 verification, and archive extraction into <exe_dir>/update_staging/.
// Transitions state -> Downloading -> ReadyToRestart (or Error on failure).
bool StartDownloadAndStageUpdate();

// Writes update_apply.ps1 (Win32) or update_apply.sh (Linux) and spawns the detached updater helper process
// to wait for current PID exit, copy app files while preserving user data/settings/saves/ISO, clear built.txt,
// relaunch the updated executable, and clean up update_staging/. Caller should exit cleanly when this returns true.
bool ApplyStagedUpdateAndRelaunch();

// Signals any active background check/download to abort and joins the worker thread cleanly.
void ShutdownUpdateChecker();

}  // namespace suffering
