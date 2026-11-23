// SPDX-License-Identifier: GPL-2.0-or-later
// Gamepad + Touch + Mouse Dear ImGui XISO File Browser modal for Linux, Steam Deck (Gamescope), and Windows.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "app/app.h"

namespace suffering {

class XisoFileBrowser {
public:
    void Open(const std::filesystem::path& initial_dir = {});
    void Close();
    bool IsOpen() const { return open_; }

    // Renders the Dear ImGui modal popup. Returns true once the user confirms a valid disc image
    // (written to out_picked). Optional system_dialog_fn enables a "System Dialog..." fallback button
    // outside Gamescope.
    bool Draw(float ui_scale, std::filesystem::path& out_picked,
              const std::function<bool(std::filesystem::path&)>& system_dialog_fn = nullptr);

private:
    struct QuickLocation {
        std::string label;
        std::filesystem::path path;
    };

    struct DirEntry {
        std::string name;
        std::filesystem::path path;
        bool is_dir = false;
        bool is_archive_hint = false;
        uint64_t size_bytes = 0;
    };

    void RefreshQuickLocations();
    void NavigateTo(const std::filesystem::path& dir);
    void SelectFileForPreview(const std::filesystem::path& file);

    bool open_ = false;
    bool open_popup_requested_ = false;
    bool show_all_files_ = false;
    bool focus_sidebar_requested_ = false;
    bool focus_list_requested_ = false;
    bool sidebar_focused_ = false;

    std::filesystem::path current_dir_;
    std::vector<QuickLocation> quick_locations_;
    std::vector<DirEntry> entries_;
    std::string dir_error_;

    std::filesystem::path selected_file_;
    bool preview_checked_ = false;
    bool preview_valid_ = false;
    app::DiscValidation preview_val_{};
};

}  // namespace suffering
