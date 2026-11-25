// SPDX-License-Identifier: GPL-2.0-or-later
// Gamepad + Touch + Mouse Dear ImGui XISO File Browser modal.
// Avoids GTK/KDE desktop portal hangs inside Steam Deck Gaming Mode (Gamescope) while still offering
// an optional native "System Dialog..." button on Desktop Linux.

#include "common/file_browser_ui.h"
#include "common/ui_common.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <imgui.h>

namespace suffering {

namespace {

namespace fs = std::filesystem;

std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string FormatSize(uint64_t bytes) {
    char buf[48];
    if (bytes >= (1ull << 30)) {
        std::snprintf(buf, sizeof(buf), "%.2f GB", double(bytes) / double(1ull << 30));
    } else if (bytes >= (1ull << 20)) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", double(bytes) / double(1ull << 20));
    } else if (bytes >= (1ull << 10)) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", double(bytes) / double(1ull << 10));
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    }
    return buf;
}

bool IsIsoFilename(const std::string& lower_name) {
    return lower_name.size() >= 4 && lower_name.compare(lower_name.size() - 4, 4, ".iso") == 0;
}

bool IsArchiveFilename(const std::string& lower_name) {
    auto ends = [&](const char* suf) {
        size_t n = std::strlen(suf);
        return lower_name.size() >= n && lower_name.compare(lower_name.size() - n, n, suf) == 0;
    };
    return ends(".7z") || ends(".zip") || ends(".rar") || ends(".gz");
}

fs::path HomeDir() {
#ifdef _WIN32
    if (const char* p = std::getenv("USERPROFILE"); p && *p) return fs::path(p);
#else
    if (const char* p = std::getenv("HOME"); p && *p) return fs::path(p);
#endif
    return app::ExeDir();
}

}  // namespace

void XisoFileBrowser::Open(const fs::path& initial_dir) {
    open_ = true;
    open_popup_requested_ = true;
    selected_file_.clear();
    preview_checked_ = false;
    preview_valid_ = false;
    preview_val_ = {};
    sidebar_focused_ = false;
    focus_list_requested_ = true;

    RefreshQuickLocations();

    std::error_code ec;
    fs::path start = initial_dir;
    if (!start.empty() && fs::is_regular_file(start, ec)) start = start.parent_path();
    if (start.empty() || !fs::is_directory(start, ec)) {
        if (!app::Get().iso_path.empty()) {
            fs::path cur = fs::path(Utf8ToWideStr(app::Get().iso_path));
            if (cur.is_relative()) cur = (app::ExeDir() / cur).lexically_normal();
            if (cur.has_parent_path() && fs::is_directory(cur.parent_path(), ec)) {
                start = cur.parent_path();
            }
        }
    }
    if (start.empty() || !fs::is_directory(start, ec)) {
        start = app::ExeDir();
    }
    NavigateTo(start);
}

void XisoFileBrowser::Close() {
    open_ = false;
    open_popup_requested_ = false;
}

void XisoFileBrowser::RefreshQuickLocations() {
    quick_locations_.clear();
    std::error_code ec;
    auto add_if_dir = [&](const std::string& label, const fs::path& p) {
        if (p.empty() || !fs::is_directory(p, ec)) return;
        fs::path norm = p.lexically_normal();
        for (const auto& existing : quick_locations_) {
            if (existing.path == norm) return;
        }
        quick_locations_.push_back({label, norm});
    };

    add_if_dir("Game Folder", app::ExeDir());
    if (fs::is_directory(app::ExeDir() / "assets", ec)) {
        add_if_dir("Game assets/", app::ExeDir() / "assets");
    }

    fs::path home = HomeDir();
    add_if_dir("Home", home);
    add_if_dir("Downloads", home / "Downloads");
    add_if_dir("Desktop", home / "Desktop");

#ifndef _WIN32
    // Auto-discover Steam Deck microSD cards (/run/media/mmcblk0p1 & /run/media/<user>/*) and removable media
    if (fs::is_directory("/run/media/mmcblk0p1", ec)) {
        add_if_dir("SD Card (mmcblk0p1)", "/run/media/mmcblk0p1");
    }
    const char* user = std::getenv("USER");
    auto scan_media_root = [&](const fs::path& root, const char* prefix) {
        if (!fs::is_directory(root, ec)) return;
        for (const auto& e : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
            if (!e.is_directory(ec)) continue;
            std::string name = e.path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            // If /run/media/<user> contains per-mount subdirs, expose each mounted volume directly
            if (user && name == user) {
                for (const auto& sub :
                     fs::directory_iterator(e.path(), fs::directory_options::skip_permission_denied, ec)) {
                    if (!sub.is_directory(ec)) continue;
                    std::string sname = sub.path().filename().string();
                    if (!sname.empty() && sname[0] != '.') {
                        add_if_dir(std::string("SD / Media: ") + sname, sub.path());
                    }
                }
            } else {
                add_if_dir(std::string(prefix) + name, e.path());
            }
        }
    };
    scan_media_root("/run/media", "Media: ");
    if (user && *user) scan_media_root(fs::path("/media") / user, "Media: ");
    add_if_dir("Filesystem (/)", "/");
#else
    for (char drv = 'C'; drv <= 'Z'; ++drv) {
        std::string root = std::string(1, drv) + ":\\";
        if (fs::is_directory(root, ec)) {
            add_if_dir(std::string("Drive ") + drv + ":", root);
        }
    }
#endif
}

void XisoFileBrowser::NavigateTo(const fs::path& dir) {
    std::error_code ec;
    fs::path target = fs::weakly_canonical(dir, ec);
    if (ec || target.empty()) target = dir.lexically_normal();

    current_dir_ = target;
    entries_.clear();
    dir_error_.clear();
    selected_file_.clear();
    preview_checked_ = false;
    preview_valid_ = false;
    preview_val_ = {};

    fs::directory_iterator it(current_dir_, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        dir_error_ = "Cannot list folder: " + ec.message();
        return;
    }

    for (const auto& e : it) {
        std::error_code eec;
        std::string name = e.path().filename().string();
        if (name.empty()) continue;
        if (!show_all_files_ && name[0] == '.') continue;
        bool is_dir = e.is_directory(eec);
        if (is_dir) {
            entries_.push_back({name, e.path(), true, false, 0});
            continue;
        }
        if (!e.is_regular_file(eec)) continue;
        std::string low = LowerAscii(name);
        bool is_iso = IsIsoFilename(low);
        bool is_arc = IsArchiveFilename(low);
        if (!show_all_files_ && !is_iso && !is_arc) continue;
        uint64_t sz = e.file_size(eec);
        entries_.push_back({name, e.path(), false, is_arc, sz});
    }

    std::sort(entries_.begin(), entries_.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return LowerAscii(a.name) < LowerAscii(b.name);
    });
}

void XisoFileBrowser::SelectFileForPreview(const fs::path& file) {
    if (selected_file_ == file && preview_checked_) return;
    selected_file_ = file;
    preview_checked_ = true;
    preview_val_ = {};
    preview_valid_ = app::ValidateDiscImage(file, preview_val_);
}

bool XisoFileBrowser::Draw(float ui_scale, fs::path& out_picked,
                           const std::function<bool(fs::path&)>& system_dialog_fn) {
    if (!open_) return false;

    constexpr const char* kPopupId = "Select Xbox Disc Image (.xiso.iso / .iso)##SufferingFileBrowser";
    if (open_popup_requested_) {
        ImGui::OpenPopup(kPopupId);
        open_popup_requested_ = false;
    }

    ImGuiIO& io = ImGui::GetIO();
    float s = std::clamp(ui_scale, 1.0f, 2.25f);
    ImVec2 modal_sz(std::min(880.0f * s, io.DisplaySize.x * 0.96f),
                    std::min(560.0f * s, io.DisplaySize.y * 0.94f));
    ImGui::SetNextWindowSize(modal_sz, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));

    bool picked_ok = false;
    bool modal_open = true;
    constexpr ImGuiWindowFlags kModalFlags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::BeginPopupModal(kPopupId, &modal_open, kModalFlags)) {
        // Gamepad LB / RB toggles between Quick-Jump sidebar and file list; B goes Up or closes.
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
            sidebar_focused_ = true;
            focus_sidebar_requested_ = true;
        } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
            sidebar_focused_ = false;
            focus_list_requested_ = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            open_ = false;
            ImGui::CloseCurrentPopup();
        } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            if (current_dir_.has_parent_path() && current_dir_.parent_path() != current_dir_) {
                NavigateTo(current_dir_.parent_path());
                if (!sidebar_focused_) focus_list_requested_ = true;
            } else {
                open_ = false;
                ImGui::CloseCurrentPopup();
            }
        }

        // Top navigation bar
        float row_h = std::max(34.0f, 36.0f * s);
        if (ImGui::Button("Up (..)", ImVec2(82.0f * s, 30.0f * s))) {
            if (current_dir_.has_parent_path() && current_dir_.parent_path() != current_dir_) {
                NavigateTo(current_dir_.parent_path());
                if (!sidebar_focused_) focus_list_requested_ = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Show All Files", &show_all_files_)) {
            NavigateTo(current_dir_);
        }
        if (system_dialog_fn && !IsGamescope()) {
            ImGui::SameLine();
            if (ImGui::Button("System Dialog...", ImVec2(142.0f * s, 30.0f * s))) {
                fs::path sys_picked;
                if (system_dialog_fn(sys_picked) && !sys_picked.empty()) {
                    SelectFileForPreview(sys_picked);
                    if (preview_valid_) {
                        out_picked = sys_picked;
                        picked_ok = true;
                        open_ = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
        }
        ImGui::TextWrapped("Folder: %s", current_dir_.string().c_str());
        ImGui::Separator();

        // Main split area: Quick-Jump sidebar (left) + Directory entries (right)
        float footer_h = 104.0f * s;
        float body_h = std::max(140.0f, ImGui::GetContentRegionAvail().y - footer_h);
        float sidebar_w = std::clamp(200.0f * s, 160.0f, modal_sz.x * 0.32f);

        if (ImGui::BeginChild("##BrowserSidebar", ImVec2(sidebar_w, body_h), ImGuiChildFlags_Border)) {
            ImGui::TextDisabled("Quick Jump (LB/RB)");
            ImGui::Separator();
            if (focus_sidebar_requested_) {
                ImGui::SetKeyboardFocusHere();
                focus_sidebar_requested_ = false;
            }
            for (size_t i = 0; i < quick_locations_.size(); ++i) {
                const auto& loc = quick_locations_[i];
                bool cur = (current_dir_ == loc.path);
                std::string id = loc.label + "##ql_" + std::to_string(i);
                if (ImGui::Selectable(id.c_str(), cur, 0, ImVec2(0.0f, row_h))) {
                    NavigateTo(loc.path);
                    sidebar_focused_ = false;
                    focus_list_requested_ = true;
                }
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        if (ImGui::BeginChild("##BrowserList", ImVec2(0.0f, body_h), ImGuiChildFlags_Border)) {
            if (!dir_error_.empty()) {
                ImGui::TextColored(ImVec4(0.95f, 0.38f, 0.35f, 1.0f), "%s", dir_error_.c_str());
            } else if (entries_.empty()) {
                ImGui::TextDisabled("No folders or .iso / .xiso.iso files in this folder.");
                ImGui::TextDisabled("Enable 'Show All Files' or pick another location from Quick Jump.");
            } else {
                if (focus_list_requested_) {
                    ImGui::SetKeyboardFocusHere();
                    focus_list_requested_ = false;
                }
                fs::path pending_nav;
                for (size_t i = 0; i < entries_.size(); ++i) {
                    const DirEntry& e = entries_[i];
                    bool is_sel = (!e.is_dir && selected_file_ == e.path);
                    std::string label;
                    if (e.is_dir) {
                        label = "[DIR]  " + e.name + "/";
                    } else if (e.is_archive_hint) {
                        label = "[ARCHIVE - Extract First]  " + e.name + "  (" + FormatSize(e.size_bytes) + ")";
                    } else {
                        label = "[ISO]  " + e.name + "  (" + FormatSize(e.size_bytes) + ")";
                    }
                    label += "##entry_" + std::to_string(i);

                    const bool pushed_text_col = (e.is_dir || e.is_archive_hint);
                    if (e.is_dir) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.76f, 0.88f, 1.0f, 1.0f));
                    } else if (e.is_archive_hint) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.76f, 0.36f, 1.0f));
                    }
                    bool clicked = ImGui::Selectable(label.c_str(), is_sel, ImGuiSelectableFlags_AllowDoubleClick,
                                                     ImVec2(0.0f, row_h));
                    if (pushed_text_col) ImGui::PopStyleColor();

                    bool dbl = clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                    bool pad_or_enter =
                        ImGui::IsItemFocused() &&
                        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false));

                    if (clicked || pad_or_enter) {
                        if (e.is_dir) {
                            pending_nav = e.path;
                        } else {
                            bool was_already_selected = (selected_file_ == e.path && preview_checked_);
                            SelectFileForPreview(e.path);
                            if (preview_valid_ && (dbl || (pad_or_enter && was_already_selected))) {
                                out_picked = e.path;
                                picked_ok = true;
                                open_ = false;
                                ImGui::CloseCurrentPopup();
                            }
                        }
                    }
                }
                if (!pending_nav.empty()) {
                    NavigateTo(pending_nav);
                    focus_list_requested_ = true;
                }
            }
        }
        ImGui::EndChild();

        // Bottom live ValidateDiscImage badge + Cancel / Select buttons
        ImGui::Separator();
        if (preview_checked_) {
            if (preview_valid_) {
                char tid[16];
                std::snprintf(tid, sizeof(tid), "0x%08X", preview_val_.title_id);
                ImGui::TextColored(ImVec4(0.35f, 0.92f, 0.45f, 1.0f),
                                   "VALID XISO: %s [%s] (Title ID %s, XDK %u)",
                                   preview_val_.title_name.c_str(), preview_val_.revision.c_str(), tid,
                                   unsigned(preview_val_.xdk_build));
            } else {
                ImGui::TextColored(ImVec4(0.95f, 0.38f, 0.35f, 1.0f), "INVALID (%s): %s",
                                   preview_val_.code.c_str(), preview_val_.error.c_str());
            }
        } else {
            ImGui::TextDisabled(
                "Tap/Click a .xiso.iso or .iso file to validate read-only. Gamepad: D-Pad=Move, A=Open/Select, B=Up, LB/RB=Sidebar.");
        }

        ImGui::Spacing();
        float btn_h = 32.0f * s;
        float btn_cancel_w = 110.0f * s;
        float btn_select_w = 190.0f * s;
        float right_w = btn_cancel_w + btn_select_w + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(std::max(ImGui::GetStyle().WindowPadding.x, ImGui::GetWindowWidth() - right_w - 16.0f));
        if (ImGui::Button("Cancel", ImVec2(btn_cancel_w, btn_h))) {
            open_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!preview_valid_);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.24f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.62f, 0.32f, 1.0f));
        if (ImGui::Button("Select Disc Image", ImVec2(btn_select_w, btn_h))) {
            out_picked = selected_file_;
            picked_ok = true;
            open_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();

        ImGui::EndPopup();
    } else if (!ImGui::IsPopupOpen(kPopupId)) {
        open_ = false;
    }

    if (!modal_open) {
        open_ = false;
    }
    return picked_ok;
}

}  // namespace suffering
