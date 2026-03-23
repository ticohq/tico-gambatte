/// @file TicoConfig.h
/// @brief Minimal hardcoded configuration for tico overlay (gambatte)
#pragma once

#include <string>

namespace TicoConfig {
    constexpr const char* TEST_ROM = "sdmc:/tico/roms/gbc/test.gbc";

    constexpr const char* FONT_PATH = "romfs:/fonts/font.ttf";

    // System path is shared for both GB and GBC
    inline std::string SYSTEM_PATH = "sdmc:/tico/system/gb/";

    // Saves and states are per-slug (gb or gbc)
    inline std::string SAVES_PATH = "sdmc:/tico/saves/gbc/";
    inline std::string STATES_PATH = "sdmc:/tico/states/gbc/";

    // Current console slug (set from argv)
    inline std::string CURRENT_SLUG = "gbc";

    /// @brief Set paths based on console slug (gb, gbc)
    inline void SetSlug(const std::string& slug) {
        CURRENT_SLUG = slug;
        SAVES_PATH = "sdmc:/tico/saves/" + slug + "/";
        STATES_PATH = "sdmc:/tico/states/" + slug + "/";
    }

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
