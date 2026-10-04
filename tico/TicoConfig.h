/// @file TicoConfig.h
/// @brief Minimal hardcoded configuration for tico overlay (gambatte)
#pragma once

#include <string>

namespace TicoConfig {
    constexpr const char* TEST_ROM = "sdmc:/tico/roms/gbc/test.gbc";

    constexpr const char* FONT_PATH = "romfs:/fonts/font.ttf";

    // Current console slug (gb or gbc, from argv[1])
    inline std::string CURRENT_SLUG = "gbc";

    /// @brief Set the console being booted (gb, gbc)
    inline void SetSlug(const std::string& slug) {
        if (!slug.empty())
            CURRENT_SLUG = slug;
    }

    /// Content directories, with a trailing slash. Tico's per-module Paths tab
    /// stores custom roots as tico_{system,saves,states}_path in gambatte.jsonc;
    /// empty or missing keys fall back to sdmc:/tico/<kind>/. Saves and states
    /// append the console slug like tico's {saves}/{states}; BIOS files live
    /// in the module's shared system_dir, <system root>/gambatte/.
    std::string SystemPath();
    std::string SavesPath();
    std::string StatesPath();

    /// Create a directory and any missing parents.
    void MakeDirs(const std::string& path);

    /// @brief Map console slug to RetroAchievements console ID
    inline int GetRcConsoleId() {
        if (CURRENT_SLUG == "gb") return 4;   // RC_CONSOLE_GAMEBOY
        return 5; // RC_CONSOLE_GAMEBOY_COLOR (default)
    }

    constexpr int WINDOW_WIDTH = 1280;
    constexpr int WINDOW_HEIGHT = 720;
    constexpr float FONT_SIZE = 32.0f;

    /// @brief Use callback/ring-buffer path (supports resampling)
    constexpr bool USE_SDLQUEUEAUDIO = false;
}

/// @brief UI action identifiers for the helpers bar
enum UIActions {
    ACTION_CONFIRM,
    ACTION_BACK,
    ACTION_DETAILS,
    ACTION_MENU,
    ACTION_EDIT,
    ACTION_DELETE
};
