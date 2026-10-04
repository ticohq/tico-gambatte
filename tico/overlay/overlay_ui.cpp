// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "overlay/overlay_ui.h"
#include "overlay/tico_config.h"
#include "overlay/translation_manager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string_view>
#include <vector>

#include <switch.h>

#include <imgui.h>

namespace SwitchFrontend::OverlayUI {

namespace {
constexpr int kOverlaySlotCount = 4;
constexpr int kToastSlotCount = 4;
// rows shown at once before a list starts scrolling
constexpr int kMaxVisibleRows = 8;
constexpr float kMenuWidth = 480.0f;
// the settings panel: a category sidebar beside the options of the focused one
constexpr float kSettingsWidth = 1040.0f;
constexpr float kSettingsHeight = 500.0f;
constexpr float kSidebarWidth = 300.0f;
constexpr float kSettingsRowHeight = 56.0f;

constexpr float kAnimDuration = 0.4f;
constexpr float kToastDuration = 2.0f;
constexpr float kToastFadeDuration = 0.5f;

enum class MenuScreen {
    QuickMenu,
    SaveStates,
    LoadStates,
    Rewind,
    Discs,
    Cheats,
    SettingsCategories,
    SettingsOptions,
};

enum class QuickItem {
    SaveState,
    LoadState,
    Rewind,
    ChangeDisc,
    Cheats,
    Settings,
    Reset,
    Exit,
};

// One line of whatever list is on screen. Every menu is drawn from these, so
// they all share scrolling, selection and layout.
struct MenuRow {
    std::string label;
    // shown right-aligned; empty for rows that only have a label
    std::string value;
    bool has_checkbox = false;
    bool checked = false;
    bool dimmed = false;
};

std::string s_title;
std::string s_nickname;
NavInput s_nav{};
bool s_visible = false;
float s_anim_timer = 0.0f;
MenuScreen s_menu = MenuScreen::QuickMenu;
// the selected row on the current screen, and the ones to return to
int s_selected = 0;
int s_quick_selected = 0;
int s_category_selected = 0;
unsigned long long s_avatar_texture_id = 0;
unsigned long long s_border_texture_id = 0;
std::mutex s_toast_mutex;
std::array<std::string, kToastSlotCount> s_toast_messages{};
std::array<float, kToastSlotCount> s_toast_timers{};
SlotOccupiedFn s_slot_occupied_cb;
CheatListFn s_cheat_list_cb;
CheatToggleFn s_cheat_toggle_cb;
RewindListFn s_rewind_list_cb;
DiscListFn s_disc_list_cb;
std::vector<CheatMenuEntry> s_cheat_entries;
std::vector<DiscMenuEntry> s_disc_entries;
std::array<bool, kOverlaySlotCount> s_slot_occupied{};
std::vector<int> s_rewind_points;

// Results the emulation thread collects: the overlay renders on the
// presentation thread and never touches the emulator itself.
std::mutex s_pending_mutex;
bool s_settings_changed = false;
int s_rewind_index = -1;
int s_disc_index = -1;
const TicoConfig::OptionDef* s_text_edit_option = nullptr;

// HUD drawn over the game while the menu is closed
std::mutex s_hud_mutex;
HudStats s_hud_stats{};
int s_fps_position = 0;
int s_resolution_position = 0;

float EaseOutCubic(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - std::pow(1.0f - t, 3.0f);
}

std::string TrOr(const char* key, const char* fallback) {
    const std::string translated = OverlayTranslation::tr(key);
    return translated == key ? std::string(fallback) : translated;
}

void DrawTextWithShadow(ImDrawList* dl, ImFont* font, float font_size, ImVec2 pos, ImU32 color,
                        std::string_view text) {
    const ImU32 shadow_color = IM_COL32(0, 0, 0, 80);
    dl->AddText(font, font_size, ImVec2(pos.x + 1.5f, pos.y + 1.5f), shadow_color, text.data(),
                text.data() + text.size());
    dl->AddText(font, font_size, pos, color, text.data(), text.data() + text.size());
}

void DrawSwitchButton(ImDrawList* dl, ImFont* font, float font_size, ImVec2 center, float size,
                      std::string_view symbol, float alpha) {
    const ImU32 fill_color = IM_COL32(220, 220, 220, static_cast<int>(255.0f * alpha));
    const ImU32 text_color = IM_COL32(40, 40, 40, static_cast<int>(255.0f * alpha));
    dl->AddCircleFilled(center, size * 0.5f, fill_color, 16);

    const float symbol_size = font_size * 0.75f;
    const ImVec2 text_size = font->CalcTextSizeA(symbol_size, FLT_MAX, 0.0f, symbol.data(),
                                                 symbol.data() + symbol.size());
    const ImVec2 text_pos(center.x - (text_size.x * 0.5f), center.y - (text_size.y * 0.5f));
    dl->AddText(font, symbol_size, text_pos, text_color, symbol.data(),
                symbol.data() + symbol.size());
}

std::string EllipsizeText(ImFont* font, float font_size, const std::string& text, float max_width) {
    if (font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str()).x <= max_width) {
        return text;
    }

    constexpr std::string_view suffix = "...";
    std::string result = text;
    while (!result.empty()) {
        result.pop_back();
        const std::string candidate = result + "...";
        if (font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, candidate.c_str()).x <= max_width) {
            return candidate;
        }
    }
    return std::string(suffix);
}

void DrawCheckBox(ImDrawList* dl, ImVec2 center, float size, bool checked, float alpha) {
    const float half = size * 0.5f;
    const ImVec2 p0(center.x - half, center.y - half);
    const ImVec2 p1(center.x + half, center.y + half);
    const ImU32 border = IM_COL32(220, 220, 220, static_cast<int>(220.0f * alpha));
    const ImU32 fill = IM_COL32(235, 235, 235, static_cast<int>(checked ? 220.0f * alpha : 0.0f));
    dl->AddRect(p0, p1, border, 4.0f, 0, 2.0f);
    if (!checked) {
        return;
    }

    dl->AddRectFilled(ImVec2(p0.x + 3.0f, p0.y + 3.0f), ImVec2(p1.x - 3.0f, p1.y - 3.0f), fill,
                      3.0f);
    const ImU32 mark = IM_COL32(45, 45, 45, static_cast<int>(255.0f * alpha));
    dl->PathLineTo(ImVec2(p0.x + (size * 0.25f), center.y));
    dl->PathLineTo(ImVec2(p0.x + (size * 0.44f), p0.y + (size * 0.68f)));
    dl->PathLineTo(ImVec2(p0.x + (size * 0.76f), p0.y + (size * 0.32f)));
    dl->PathStroke(mark, 0, 2.5f);
}

// Port of tico-nx's UIStyle::DrawAnimatedGradientBorder (textured path): the
// tint's gradient strip is wrapped around the rounded outline and scrolled
// along it over time.
void DrawAnimatedGradientBorder(ImDrawList* dl, ImVec2 min, ImVec2 max, float corner_radius,
                                float frame_width, float alpha, ImTextureID texture) {
    constexpr float kPi = 3.14159265f;
    constexpr int kCornerSegments = 12;
    const float phase = static_cast<float>(ImGui::GetTime()) * 0.5f;
    const float phase_mod = phase - std::floor(phase);
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    corner_radius = std::min(corner_radius, std::min(w, h) * 0.5f);
    const float perimeter =
        (2.0f * (w + h - (4.0f * corner_radius))) + (2.0f * kPi * corner_radius);

    struct BorderPoint {
        ImVec2 pos;
        ImVec2 normal;
        // distance along the outline, 0..1
        float dist;
    };
    std::array<BorderPoint, 4 * (kCornerSegments + 1)> points{};
    float travelled = 0.0f;
    const std::array<ImVec2, 4> centers = {{
        ImVec2(min.x + corner_radius, min.y + corner_radius),
        ImVec2(max.x - corner_radius, min.y + corner_radius),
        ImVec2(max.x - corner_radius, max.y - corner_radius),
        ImVec2(min.x + corner_radius, max.y - corner_radius),
    }};
    std::size_t count = 0;
    for (int c = 0; c < 4; ++c) {
        const float start = -kPi + (static_cast<float>(c) * kPi * 0.5f);
        for (int i = 0; i <= kCornerSegments; ++i) {
            const float angle = start + (kPi * 0.5f * static_cast<float>(i) / kCornerSegments);
            const ImVec2 normal(std::cos(angle), std::sin(angle));
            const ImVec2 pos(centers[c].x + (normal.x * corner_radius),
                             centers[c].y + (normal.y * corner_radius));
            if (count > 0) {
                const float dx = pos.x - points[count - 1].pos.x;
                const float dy = pos.y - points[count - 1].pos.y;
                travelled += std::sqrt((dx * dx) + (dy * dy));
            }
            points[count++] = {pos, normal, perimeter > 0.0f ? travelled / perimeter : 0.0f};
        }
    }

    const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(255.0f * alpha));
    const float half = frame_width * 0.5f;
    const auto lerp = [](ImVec2 a, ImVec2 b, float t) {
        return ImVec2(a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t));
    };
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t next = (i + 1) % count;
        const BorderPoint& p1 = points[i];
        const BorderPoint& p2 = points[next];
        const ImVec2 v1o(p1.pos.x + (p1.normal.x * half), p1.pos.y + (p1.normal.y * half));
        const ImVec2 v1i(p1.pos.x - (p1.normal.x * half), p1.pos.y - (p1.normal.y * half));
        const ImVec2 v2o(p2.pos.x + (p2.normal.x * half), p2.pos.y + (p2.normal.y * half));
        const ImVec2 v2i(p2.pos.x - (p2.normal.x * half), p2.pos.y - (p2.normal.y * half));
        float u1 = p1.dist + phase_mod;
        float u2 = (next == 0 ? 1.0f : p2.dist) + phase_mod;
        const auto emit = [&](float t1, float t2, float u_start, float u_end) {
            dl->AddImageQuad(ImTextureRef(texture), lerp(v1o, v2o, t1), lerp(v1o, v2o, t2),
                             lerp(v1i, v2i, t2), lerp(v1i, v2i, t1), ImVec2(u_start, 0.0f),
                             ImVec2(u_end, 0.0f), ImVec2(u_end, 1.0f), ImVec2(u_start, 1.0f), tint);
        };
        // keep the UVs inside [0,1]: split a segment that crosses the wrap
        if (u1 < 1.0f && u2 > 1.0f) {
            const float split = (1.0f - u1) / (u2 - u1);
            emit(0.0f, split, u1, 1.0f);
            emit(split, 1.0f, 0.0f, u2 - 1.0f);
        } else {
            if (u1 >= 1.0f)
                u1 -= 1.0f;
            if (u2 >= 1.0f)
                u2 -= 1.0f;
            emit(0.0f, 1.0f, u1, u2);
        }
    }
}

// The focused row of any list: the animated tint border, or a plain
// highlight when the strip could not be loaded.
void DrawSelection(ImDrawList* dl, ImVec2 min, ImVec2 max, float corner_radius, float alpha) {
    const float scale = ImGui::GetIO().FontGlobalScale;
    if (s_border_texture_id == 0) {
        dl->AddRectFilled(min, max, IM_COL32(60, 60, 60, static_cast<int>(255.0f * alpha)),
                          corner_radius);
        return;
    }
    DrawAnimatedGradientBorder(dl, min, max, corner_radius, 4.0f * scale, alpha,
                               static_cast<ImTextureID>(s_border_texture_id));
}

// A solid swatch of the tint (one column of the strip), for accents.
void DrawTintSwatch(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float alpha) {
    const ImU32 color = IM_COL32(255, 255, 255, static_cast<int>(255.0f * alpha));
    if (s_border_texture_id == 0) {
        dl->AddRectFilled(min, max, IM_COL32(220, 220, 220, static_cast<int>(255.0f * alpha)),
                          rounding);
        return;
    }
    dl->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(s_border_texture_id)), min, max,
                        ImVec2(0.1f, 0.0f), ImVec2(0.1f, 1.0f), color, rounding);
}

std::string TrLabel(const char* key, const std::string& fallback) {
    if (!key) {
        return fallback;
    }
    const std::string translated = OverlayTranslation::tr(key);
    return translated == key ? fallback : translated;
}

std::vector<QuickItem> BuildQuickItems() {
    std::vector<QuickItem> items = {QuickItem::SaveState, QuickItem::LoadState};
    if (s_rewind_list_cb && TicoConfig::GetConfigValue("enable_rewind", "false") == "true") {
        items.push_back(QuickItem::Rewind);
    }
    if (s_disc_entries.size() > 1) {
        items.push_back(QuickItem::ChangeDisc);
    }
    if (s_cheat_list_cb) {
        items.push_back(QuickItem::Cheats);
    }
    items.push_back(QuickItem::Settings);
    items.push_back(QuickItem::Reset);
    items.push_back(QuickItem::Exit);
    return items;
}

std::string QuickItemLabel(QuickItem item) {
    switch (item) {
    case QuickItem::SaveState:
        return TrOr("emulator_save_state", "Save State");
    case QuickItem::LoadState:
        return TrOr("emulator_load_state", "Load State");
    case QuickItem::Rewind:
        return TrOr("emulator_rewind", "Rewind");
    case QuickItem::ChangeDisc:
        return TrOr("emulator_select_disc", "Change Disc");
    case QuickItem::Cheats:
        return TrOr("emulator_cheats", "Cheats");
    case QuickItem::Settings:
        return TrOr("emulator_settings", "Settings");
    case QuickItem::Reset:
        return TrOr("emulator_reset", "Reset");
    case QuickItem::Exit:
    default:
        return TrOr("emulator_exit_game", "Exit Game");
    }
}

const TicoConfig::OptionCategory& CurrentCategory() {
    const auto& categories = TicoConfig::GetCategories();
    const std::size_t index =
        std::min(static_cast<std::size_t>(std::max(s_category_selected, 0)), categories.size() - 1);
    return categories[index];
}

// The current category's options the menu lists right now; an option can
// depend on another's value (FSR sharpness only shows with the FSR filter).
std::vector<const TicoConfig::OptionDef*> VisibleOptions() {
    const TicoConfig::OptionCategory& category = CurrentCategory();
    std::vector<const TicoConfig::OptionDef*> options;
    for (std::size_t i = 0; i < category.option_count; ++i) {
        if (TicoConfig::IsOptionShown(category.options[i])) {
            options.push_back(&category.options[i]);
        }
    }
    return options;
}

const TicoConfig::OptionDef* SelectedOption() {
    const std::vector<const TicoConfig::OptionDef*> options = VisibleOptions();
    if (s_selected < 0 || s_selected >= static_cast<int>(options.size())) {
        return nullptr;
    }
    return options[static_cast<std::size_t>(s_selected)];
}

void RefreshCheats() {
    s_cheat_entries = s_cheat_list_cb ? s_cheat_list_cb() : std::vector<CheatMenuEntry>{};
}

void RefreshSlots() {
    for (int i = 0; i < kOverlaySlotCount; ++i) {
        s_slot_occupied[i] = s_slot_occupied_cb ? s_slot_occupied_cb(i + 1) : false;
    }
}

void RefreshRewindPoints() {
    s_rewind_points = s_rewind_list_cb ? s_rewind_list_cb() : std::vector<int>{};
}

void RefreshDiscs() {
    s_disc_entries = s_disc_list_cb ? s_disc_list_cb() : std::vector<DiscMenuEntry>{};
}

void ReloadHudPositions() {
    const auto& categories = TicoConfig::GetCategories();
    int fps = 0;
    int resolution = 0;
    for (const auto& category : categories) {
        for (std::size_t i = 0; i < category.option_count; ++i) {
            const TicoConfig::OptionDef& option = category.options[i];
            const std::string_view key = option.key;
            if (key != "fps_counter_position" && key != "rendered_ir_position") {
                continue;
            }
            const std::string value = TicoConfig::GetOptionValue(option);
            int position = 0;
            for (std::size_t c = 0; c < option.choice_count; ++c) {
                if (value == option.choices[c].value) {
                    position = static_cast<int>(c);
                }
            }
            (key == "fps_counter_position" ? fps : resolution) = position;
        }
    }
    std::lock_guard lock(s_hud_mutex);
    s_fps_position = fps;
    s_resolution_position = resolution;
}

// The focused category's options, as the settings panel lists them.
std::vector<MenuRow> BuildOptionRows() {
    std::vector<MenuRow> rows;
    for (const TicoConfig::OptionDef* shown : VisibleOptions()) {
        const TicoConfig::OptionDef& option = *shown;
        const TicoConfig::OptionValueLabel value = TicoConfig::GetOptionValueLabel(option);
        MenuRow row{TrLabel(option.label_key, option.fallback)};
        row.value = TrLabel(value.label_key, value.fallback);
        if (row.value.empty()) {
            row.value = "-";
        }
        rows.push_back(row);
    }
    return rows;
}

// The rows of the screen that is currently open.
std::vector<MenuRow> BuildRows() {
    std::vector<MenuRow> rows;
    switch (s_menu) {
    case MenuScreen::QuickMenu:
        for (const QuickItem item : BuildQuickItems()) {
            rows.push_back({QuickItemLabel(item)});
        }
        break;
    case MenuScreen::SaveStates:
    case MenuScreen::LoadStates: {
        const std::string slot_format = TrOr("emulator_slot", "Slot %d (%s)");
        const std::string in_use = TrOr("emulator_in_use", "In Use");
        const std::string empty = TrOr("emulator_empty", "Empty");
        for (int i = 0; i < kOverlaySlotCount; ++i) {
            char label[128];
            std::snprintf(label, sizeof(label), slot_format.c_str(), i + 1,
                          s_slot_occupied[i] ? in_use.c_str() : empty.c_str());
            MenuRow row{label};
            row.dimmed = s_menu == MenuScreen::LoadStates && !s_slot_occupied[i];
            rows.push_back(row);
        }
        break;
    }
    case MenuScreen::Rewind: {
        if (s_rewind_points.empty()) {
            MenuRow row{TrOr("emulator_no_rewind_points", "Nothing to rewind to yet")};
            row.dimmed = true;
            rows.push_back(row);
            break;
        }
        const std::string format = TrOr("emulator_seconds_ago", "%d seconds ago");
        for (const int seconds : s_rewind_points) {
            char label[96];
            std::snprintf(label, sizeof(label), format.c_str(), seconds);
            rows.push_back({label});
        }
        break;
    }
    case MenuScreen::Discs:
        for (const DiscMenuEntry& entry : s_disc_entries) {
            MenuRow row{entry.name};
            row.has_checkbox = true;
            row.checked = entry.current;
            rows.push_back(row);
        }
        break;
    case MenuScreen::Cheats:
        if (s_cheat_entries.empty()) {
            MenuRow row{TrOr("emulator_no_cheats", "No Cheats")};
            row.dimmed = true;
            rows.push_back(row);
            break;
        }
        for (const CheatMenuEntry& entry : s_cheat_entries) {
            MenuRow row{entry.name};
            if (entry.is_add_row) {
                rows.push_back(row);
                continue;
            }
            row.has_checkbox = true;
            row.checked = entry.enabled;
            row.dimmed = !entry.toggleable;
            rows.push_back(row);
        }
        break;
    case MenuScreen::SettingsCategories:
        for (const auto& category : TicoConfig::GetCategories()) {
            rows.push_back({TrLabel(category.label_key, category.fallback)});
        }
        break;
    case MenuScreen::SettingsOptions:
        rows = BuildOptionRows();
        break;
    }
    return rows;
}

std::string BuildTitle() {
    std::string title;
    switch (s_menu) {
    case MenuScreen::SaveStates:
        title = TrOr("emulator_save_state", "Save State");
        break;
    case MenuScreen::LoadStates:
        title = TrOr("emulator_load_state", "Load State");
        break;
    case MenuScreen::Rewind:
        title = TrOr("emulator_rewind", "Rewind");
        break;
    case MenuScreen::Discs:
        title = TrOr("emulator_select_disc", "Change Disc");
        break;
    case MenuScreen::Cheats:
        title = TrOr("emulator_cheats", "Cheats");
        break;
    case MenuScreen::SettingsCategories:
    case MenuScreen::SettingsOptions:
        // the panel names the category itself
        title = TrOr("emulator_settings", "Settings");
        break;
    case MenuScreen::QuickMenu:
    default:
        title = s_title.empty() ? "Gambatte" : s_title;
        break;
    }

    std::replace(title.begin(), title.end(), '\n', ' ');
    std::replace(title.begin(), title.end(), '\r', ' ');
    std::replace(title.begin(), title.end(), '\t', ' ');
    const std::size_t first = title.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = title.find_last_not_of(' ');
    return title.substr(first, last - first + 1);
}

std::vector<std::string> WrapTitle(const std::string& title, float max_width) {
    std::vector<std::string> lines;
    std::string current;
    std::size_t pos = 0;

    while (pos < title.size()) {
        while (pos < title.size() && title[pos] == ' ') {
            pos++;
        }
        std::size_t end = title.find(' ', pos);
        if (end == std::string::npos) {
            end = title.size();
        }

        std::string word = title.substr(pos, end - pos);
        pos = end;
        if (word.empty()) {
            continue;
        }

        const std::string candidate = current.empty() ? word : current + " " + word;
        if (current.empty() || ImGui::CalcTextSize(candidate.c_str()).x <= max_width) {
            current = candidate;
        } else {
            lines.push_back(current);
            current = word;
        }
    }

    if (!current.empty()) {
        lines.push_back(current);
    }
    if (lines.empty()) {
        lines.push_back(title);
    }
    if (lines.size() > 3) {
        lines.resize(3);
        lines[2] += "...";
    }
    return lines;
}

Action MakeSaveActionForSlot(int slot_index) {
    return static_cast<Action>(static_cast<int>(Action::SaveStateSlot1) + slot_index);
}

Action MakeLoadActionForSlot(int slot_index) {
    return static_cast<Action>(static_cast<int>(Action::LoadStateSlot1) + slot_index);
}

void RenderOverlayBackground(ImDrawList* dl, ImVec2 display_size, float ease) {
    const int base_alpha = static_cast<int>(200.0f * ease);
    const int max_alpha = static_cast<int>(250.0f * ease);
    if (base_alpha <= 0)
        return;

    const float top_h = display_size.y * 0.20f;
    const float bottom_h = display_size.y * 0.20f;
    const float center_h = display_size.y - top_h - bottom_h;
    const ImU32 col_max = IM_COL32(0, 0, 0, max_alpha);
    const ImU32 col_base = IM_COL32(0, 0, 0, base_alpha);

    dl->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), ImVec2(display_size.x, top_h), col_max, col_max,
                                col_base, col_base);
    dl->AddRectFilled(ImVec2(0.0f, top_h), ImVec2(display_size.x, top_h + center_h), col_base);
    dl->AddRectFilledMultiColor(ImVec2(0.0f, display_size.y - bottom_h), display_size, col_base,
                                col_base, col_max, col_max);
}

void RenderTitleCard(ImDrawList* dl, ImVec2 display_size, float ease) {
    const std::string title = BuildTitle();
    const float scale = ImGui::GetIO().FontGlobalScale;
    const float title_height = 72.0f * scale;
    const float available_top_space = 110.0f * scale;
    const float card_width = display_size.x * 0.7f;
    const float card_x = (display_size.x - card_width) * 0.5f;
    const float card_y = (available_top_space - title_height) * 0.5f;
    const float start_y = -150.0f * scale;
    const float current_y = start_y + ((card_y - start_y) * ease);
    const ImU32 text_color = IM_COL32(200, 200, 200, static_cast<int>(255.0f * ease));
    const std::vector<std::string> lines = WrapTitle(title.empty() ? std::string{"Gambatte"} : title,
                                                     card_width);
    const float line_height = ImGui::GetTextLineHeight();
    const float block_height = line_height * static_cast<float>(lines.size());
    float text_y = current_y + ((title_height - block_height) * 0.5f);

    for (const std::string& line : lines) {
        const ImVec2 text_size = ImGui::CalcTextSize(line.c_str());
        const float text_x = card_x + ((card_width - text_size.x) * 0.5f);
        DrawTextWithShadow(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(text_x, text_y),
                           text_color, line);
        text_y += line_height;
    }
}

// The first row to draw so that `selected` stays in view, centred while the
// list can scroll.
int FirstVisibleRow(int selected, int count, int visible) {
    if (count <= visible) {
        return 0;
    }
    return std::clamp(selected - (visible / 2), 0, count - visible);
}

// The label, and the value (with change arrows when selected) or checkbox, of
// one row inside [item_min, item_max].
void DrawRowContent(ImDrawList* dl, const MenuRow& row, ImVec2 item_min, ImVec2 item_max,
                    bool selected, float ease, float label_size) {
    const float scale = ImGui::GetIO().FontGlobalScale;
    ImFont* font = ImGui::GetFont();
    const int alpha = static_cast<int>(255.0f * ease);
    const float item_height = item_max.y - item_min.y;
    const int shade = row.dimmed ? 150 : (selected ? 255 : 200);
    const ImU32 text_color = IM_COL32(shade, shade, shade, alpha);
    const float text_x = item_min.x + (20.0f * scale);
    const float right_edge = item_max.x - (28.0f * scale);

    float label_max_width = right_edge - text_x;
    if (row.has_checkbox) {
        const float checkbox_size = 22.0f * scale;
        DrawCheckBox(dl, ImVec2(right_edge - (checkbox_size * 0.5f), item_min.y + (item_height * 0.5f)),
                     checkbox_size, row.checked, ease);
        label_max_width -= checkbox_size + (28.0f * scale);
    } else if (!row.value.empty()) {
        const float arrow_size = 12.0f * scale;
        const float arrow_gap = 12.0f * scale;
        // the value never takes more than half the row
        const std::string value =
            EllipsizeText(font, label_size, row.value,
                          ((item_max.x - item_min.x) * 0.5f) - (2.0f * arrow_size));
        const ImVec2 value_size = font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, value.c_str());
        const float value_x = right_edge - arrow_size - arrow_gap - value_size.x;
        const float value_y = item_min.y + ((item_height - value_size.y) * 0.5f);
        dl->AddText(font, label_size, ImVec2(value_x, value_y), text_color, value.c_str());

        if (selected) {
            const float arrow_y = item_min.y + ((item_height - arrow_size) * 0.5f);
            const float left_x = value_x - arrow_gap - arrow_size;
            dl->AddTriangleFilled(ImVec2(left_x, arrow_y + (arrow_size * 0.5f)),
                                  ImVec2(left_x + arrow_size, arrow_y),
                                  ImVec2(left_x + arrow_size, arrow_y + arrow_size), text_color);
            const float right_x = value_x + value_size.x + arrow_gap;
            dl->AddTriangleFilled(ImVec2(right_x + arrow_size, arrow_y + (arrow_size * 0.5f)),
                                  ImVec2(right_x, arrow_y), ImVec2(right_x, arrow_y + arrow_size),
                                  text_color);
        }
        label_max_width = value_x - (2.0f * arrow_gap) - arrow_size - text_x;
    }

    const std::string label = EllipsizeText(font, label_size, row.label, label_max_width);
    const ImVec2 text_size = font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, label.c_str());
    const float text_y = item_min.y + ((item_height - text_size.y) * 0.5f);
    dl->AddText(font, label_size, ImVec2(text_x, text_y), text_color, label.c_str());
}

// Scroll position for a list that does not fit, drawn as a thin bar at x.
void DrawScrollbar(ImDrawList* dl, float x, float top, float height, int first_visible,
                   int visible_count, int item_count, float ease) {
    if (item_count <= visible_count) {
        return;
    }
    const float scale = ImGui::GetIO().FontGlobalScale;
    const float thumb_height =
        height * static_cast<float>(visible_count) / static_cast<float>(item_count);
    const float thumb_y = top + (height - thumb_height) * static_cast<float>(first_visible) /
                                    static_cast<float>(item_count - visible_count);
    dl->AddRectFilled(ImVec2(x, thumb_y), ImVec2(x + (3.0f * scale), thumb_y + thumb_height),
                      IM_COL32(200, 200, 200, static_cast<int>(120.0f * ease)), 2.0f);
}

// Draws the current screen's rows. Lists longer than kMaxVisibleRows scroll to
// keep the selection in view; rows with a value get change arrows when selected.
void RenderMenu(ImDrawList* dl, ImVec2 display_size, float ease, const std::vector<MenuRow>& rows) {
    const bool wide = s_menu == MenuScreen::Cheats;
    const float scale = ImGui::GetIO().FontGlobalScale;
    const float menu_width = kMenuWidth * (wide ? 1.5f : 1.0f) * scale;
    const float item_height = (wide ? 58.0f : 64.0f) * scale;
    const int item_count = static_cast<int>(rows.size());
    const int visible_count = std::min(item_count, kMaxVisibleRows);
    const ImVec2 menu_size(menu_width, static_cast<float>(visible_count) * item_height);
    const float target_y = (display_size.y - menu_size.y) * 0.5f;
    const float start_y = display_size.y + (100.0f * scale);
    const float current_y = start_y + ((target_y - start_y) * ease);
    const ImVec2 menu_pos((display_size.x - menu_size.x) * 0.5f, current_y);
    const float corner_radius = 16.0f * scale;
    const int alpha = static_cast<int>(255.0f * ease);

    dl->AddRectFilled(menu_pos, ImVec2(menu_pos.x + menu_size.x, menu_pos.y + menu_size.y),
                      IM_COL32(45, 45, 45, alpha), corner_radius);

    const float label_size = ImGui::GetFontSize() * (wide ? 0.82f : 0.85f);
    const int first_visible = FirstVisibleRow(s_selected, item_count, visible_count);
    for (int row_index = 0; row_index < visible_count; ++row_index) {
        const int i = first_visible + row_index;
        const bool selected = s_selected == i;
        const float item_y = menu_pos.y + (static_cast<float>(row_index) * item_height);
        const ImVec2 item_min(menu_pos.x, item_y);
        const ImVec2 item_max(menu_pos.x + menu_size.x, item_y + item_height);
        if (selected) {
            DrawSelection(dl, item_min, item_max, corner_radius, ease);
        }
        DrawRowContent(dl, rows[static_cast<std::size_t>(i)], item_min, item_max, selected, ease,
                       label_size);
    }

    DrawScrollbar(dl, menu_pos.x + menu_size.x - (8.0f * scale), menu_pos.y + corner_radius,
                  menu_size.y - (2.0f * corner_radius), first_visible, visible_count, item_count,
                  ease);
}

// Settings as one wide panel: the categories down a sidebar on the left and the
// focused category's options on the right. Up and down move through the
// categories, previewing their options; A or right moves into the options,
// and B returns to the sidebar.
void RenderSettings(ImDrawList* dl, ImVec2 display_size, float ease) {
    const bool options_focused = s_menu == MenuScreen::SettingsOptions;
    const float scale = ImGui::GetIO().FontGlobalScale;
    ImFont* font = ImGui::GetFont();
    const int alpha = static_cast<int>(255.0f * ease);
    const float corner_radius = 16.0f * scale;
    const float row_radius = 12.0f * scale;
    const float pad = 14.0f * scale;
    const float row_height = kSettingsRowHeight * scale;
    const float label_size = ImGui::GetFontSize() * 0.8f;

    // between the title card and the helpers bar
    const float top_space = 110.0f * scale;
    const float bottom_space = 96.0f * scale;
    const ImVec2 panel_size(std::min(kSettingsWidth * scale, display_size.x - (64.0f * scale)),
                            std::min(kSettingsHeight * scale,
                                     display_size.y - top_space - bottom_space));
    const float target_y =
        top_space + ((display_size.y - top_space - bottom_space - panel_size.y) * 0.5f);
    const float start_y = display_size.y + (100.0f * scale);
    const ImVec2 panel_min((display_size.x - panel_size.x) * 0.5f,
                           start_y + ((target_y - start_y) * ease));
    const ImVec2 panel_max(panel_min.x + panel_size.x, panel_min.y + panel_size.y);
    const float sidebar_right = panel_min.x + (kSidebarWidth * scale);

    dl->AddRectFilled(panel_min, panel_max, IM_COL32(45, 45, 45, alpha), corner_radius);
    dl->AddRectFilled(panel_min, ImVec2(sidebar_right, panel_max.y), IM_COL32(34, 34, 34, alpha),
                      corner_radius, ImDrawFlags_RoundCornersLeft);

    // sidebar
    const auto& categories = TicoConfig::GetCategories();
    const int category_count = static_cast<int>(categories.size());
    const int sidebar_visible = std::min(
        category_count, std::max(1, static_cast<int>((panel_size.y - (2.0f * pad)) / row_height)));
    const int first_category = FirstVisibleRow(s_category_selected, category_count, sidebar_visible);
    for (int row_index = 0; row_index < sidebar_visible; ++row_index) {
        const int i = first_category + row_index;
        const bool active = i == s_category_selected;
        const ImVec2 item_min(panel_min.x + pad,
                              panel_min.y + pad + (static_cast<float>(row_index) * row_height));
        const ImVec2 item_max(sidebar_right - pad, item_min.y + row_height);
        if (active && options_focused) {
            // where the options came from, while focus is on them
            dl->AddRectFilled(item_min, item_max, IM_COL32(56, 56, 56, alpha), row_radius);
            const float bar_height = row_height * 0.45f;
            const float bar_y = item_min.y + ((row_height - bar_height) * 0.5f);
            DrawTintSwatch(dl, ImVec2(item_min.x + (6.0f * scale), bar_y),
                           ImVec2(item_min.x + (10.0f * scale), bar_y + bar_height), 2.0f * scale,
                           ease);
        } else if (active) {
            DrawSelection(dl, item_min, item_max, row_radius, ease);
        }
        const auto& category = categories[static_cast<std::size_t>(i)];
        MenuRow row{TrLabel(category.label_key, category.fallback)};
        DrawRowContent(dl, row, item_min, item_max, active, ease, label_size);
    }
    DrawScrollbar(dl, sidebar_right - (6.0f * scale), panel_min.y + corner_radius,
                  panel_size.y - (2.0f * corner_radius), first_category, sidebar_visible,
                  category_count, ease);

    // options pane: the category name over its options
    const float pane_left = sidebar_right + pad;
    const float pane_right = panel_max.x - pad;
    const std::string heading = TrLabel(CurrentCategory().label_key, CurrentCategory().fallback);
    const float heading_size = ImGui::GetFontSize() * 0.95f;
    const float heading_height = 56.0f * scale;
    const ImVec2 heading_text_size =
        font->CalcTextSizeA(heading_size, FLT_MAX, 0.0f, heading.c_str());
    const float heading_x = pane_left + (20.0f * scale);
    DrawTextWithShadow(dl, font, heading_size,
                       ImVec2(heading_x, panel_min.y + pad +
                                             ((heading_height - heading_text_size.y) * 0.5f)),
                       IM_COL32(235, 235, 235, alpha),
                       EllipsizeText(font, heading_size, heading, pane_right - heading_x));
    const float rule_y = panel_min.y + pad + heading_height;
    dl->AddLine(ImVec2(heading_x, rule_y), ImVec2(pane_right - (20.0f * scale), rule_y),
                IM_COL32(80, 80, 80, alpha), 1.0f * scale);

    // a line under the list says when the focused option applies on the next start
    const float footer_height = 40.0f * scale;
    const float list_top = rule_y + (8.0f * scale);
    const float list_bottom = panel_max.y - pad - footer_height;
    const std::vector<MenuRow> rows = BuildOptionRows();
    const int option_count = static_cast<int>(rows.size());
    const int option_visible = std::min(
        option_count, std::max(1, static_cast<int>((list_bottom - list_top) / row_height)));
    const int focused = options_focused ? s_selected : -1;
    const int first_option = FirstVisibleRow(std::max(focused, 0), option_count, option_visible);
    for (int row_index = 0; row_index < option_visible; ++row_index) {
        const int i = first_option + row_index;
        const bool selected = i == focused;
        const ImVec2 item_min(pane_left, list_top + (static_cast<float>(row_index) * row_height));
        const ImVec2 item_max(pane_right, item_min.y + row_height);
        if (selected) {
            DrawSelection(dl, item_min, item_max, row_radius, ease);
        }
        DrawRowContent(dl, rows[static_cast<std::size_t>(i)], item_min, item_max, selected, ease,
                       label_size);
    }
    DrawScrollbar(dl, panel_max.x - (8.0f * scale), list_top, list_bottom - list_top, first_option,
                  option_visible, option_count, ease);

    const TicoConfig::OptionDef* option = options_focused ? SelectedOption() : nullptr;
    if (option && option->needs_restart) {
        const std::string note =
            TrOr("emulator_applies_next_launch", "Applies the next time the game starts");
        const float note_size = ImGui::GetFontSize() * 0.62f;
        const ImVec2 note_text_size = font->CalcTextSizeA(note_size, FLT_MAX, 0.0f, note.c_str());
        dl->AddText(font, note_size,
                    ImVec2(heading_x, list_bottom + ((footer_height - note_text_size.y) * 0.5f)),
                    IM_COL32(160, 160, 160, alpha),
                    EllipsizeText(font, note_size, note, pane_right - heading_x).c_str());
    }
}

void RenderHelpersBar(ImDrawList* dl, ImVec2 display_size, float ease) {
    struct Helper {
        const char* button;
        std::string_view description;
    };

    const std::string back = TrOr("emulator_back", "Back");
    std::string accept = TrOr("emulator_select", "Select");
    if (s_menu == MenuScreen::SaveStates)
        accept = TrOr("emulator_save_state", "Save State");
    else if (s_menu == MenuScreen::LoadStates)
        accept = TrOr("emulator_load_state", "Load State");
    else if (s_menu == MenuScreen::Rewind)
        accept = TrOr("emulator_rewind", "Rewind");
    else if (s_menu == MenuScreen::Discs)
        accept = TrOr("emulator_change", "Change");
    else if (s_menu == MenuScreen::Cheats && !s_cheat_entries.empty())
        accept = TrOr("emulator_toggle", "Toggle");
    else if (s_menu == MenuScreen::SettingsOptions)
        accept = TrOr("emulator_change", "Change");

    const std::array<Helper, 2> helpers = {{
        {"B", back},
        {"A", accept},
    }};

    const float scale = ImGui::GetIO().FontGlobalScale;
    const float bar_height = 48.0f * scale;
    const float margin_bottom = 24.0f * scale;
    const float padding = 16.0f * scale;
    const float button_size = 22.0f * scale;
    const float item_spacing = 12.0f * scale;
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize() * 0.78f;

    float total_width = padding * 2.0f;
    for (size_t i = 0; i < helpers.size(); ++i) {
        const char* const text_begin = helpers[i].description.data();
        const char* const text_end = text_begin + helpers[i].description.size();
        total_width += button_size + (8.0f * scale) +
                       font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text_begin, text_end).x;
        if (i + 1 < helpers.size())
            total_width += item_spacing;
    }

    const float current_offset = (400.0f * scale) * (1.0f - ease);
    const float bar_x = display_size.x - total_width - 20.0f + current_offset;
    const float bar_y = display_size.y - margin_bottom - bar_height;
    float cursor_x = bar_x + padding;
    const float center_y = bar_y + (bar_height * 0.5f);
    const ImU32 text_color = IM_COL32(200, 200, 200, static_cast<int>(255.0f * ease));

    for (const Helper& helper : helpers) {
        DrawSwitchButton(dl, font, font_size, ImVec2(cursor_x + (button_size * 0.5f), center_y),
                         button_size, helper.button, ease);

        cursor_x += button_size + (8.0f * scale);
        const char* const text_begin = helper.description.data();
        const char* const text_end = text_begin + helper.description.size();
        const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text_begin, text_end);
        dl->AddText(font, font_size, ImVec2(cursor_x, center_y - (text_size.y * 0.5f)), text_color,
                    text_begin, text_end);
        cursor_x += text_size.x + item_spacing;
    }
}

void RenderSocialArea(ImDrawList* dl, float ease) {
    if (ease <= 0.0f)
        return;

    const float scale = ImGui::GetIO().FontGlobalScale;
    const float start_offset = 200.0f * scale;
    const float current_offset = start_offset * (1.0f - ease);
    const float avatar_size = 72.0f * scale;
    const float side_margin = 32.0f * scale;
    const float top_margin = 32.0f * scale;
    const float bar_height = 50.0f * scale;
    const ImVec2 avatar_center(side_margin + (avatar_size * 0.5f) - current_offset,
                               top_margin + (bar_height * 0.5f));
    const float radius = avatar_size * 0.5f;

    dl->AddCircleFilled(avatar_center, radius, IM_COL32(45, 45, 45, static_cast<int>(255.0f * ease)));

    const float image_radius = radius - (4.0f * scale);
    if (s_avatar_texture_id != 0) {
        const ImVec2 image_min(avatar_center.x - image_radius, avatar_center.y - image_radius);
        const ImVec2 image_max(avatar_center.x + image_radius, avatar_center.y + image_radius);
        dl->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(s_avatar_texture_id)), image_min,
                            image_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32_WHITE,
                            image_radius);
        dl->AddCircle(avatar_center, image_radius, IM_COL32(255, 255, 255, 60), 0, 1.0f);
    } else {
        dl->AddCircleFilled(avatar_center, image_radius, IM_COL32(200, 200, 210, 255));
    }
}

void RenderStatusBar(ImDrawList* dl, ImVec2 display_size, float ease) {
    const float scale = ImGui::GetIO().FontGlobalScale;
    const float bar_height = 50.0f * scale;
    const float top_margin = 32.0f * scale;
    const float side_margin = 18.0f * scale;
    const float item_spacing = 20.0f * scale;
    const float padding = 20.0f * scale;
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();

    std::time_t now = std::time(nullptr);
    std::tm local_tm{};
    char time_str[16] = "00:00";
    if (localtime_r(&now, &local_tm))
        std::strftime(time_str, sizeof(time_str), "%H:%M", &local_tm);

    u32 battery_level = 0;
    const bool has_battery = psmGetBatteryChargePercentage(&battery_level) == 0;
    PsmChargerType charger_type = PsmChargerType_Unconnected;
    const bool charging =
        psmGetChargerType(&charger_type) == 0 && charger_type != PsmChargerType_Unconnected;

    float total_width = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, time_str).x;
    if (has_battery) {
        total_width += item_spacing + (34.0f * scale);
        if (charging)
            total_width += 20.0f * scale;
    }
    total_width += padding * 2.0f;

    const float bar_x = display_size.x - total_width - side_margin;
    const float bar_y = top_margin + ((1.0f - ease) * -20.0f);
    const float center_y = bar_y + (bar_height * 0.5f);
    float cursor_x = bar_x + padding;
    const ImU32 text_color = IM_COL32(200, 200, 200, static_cast<int>(255.0f * ease));

    dl->AddText(font, font_size, ImVec2(cursor_x, center_y - (font_size * 0.5f)), text_color,
                time_str);
    cursor_x += font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, time_str).x;

    if (!has_battery)
        return;

    cursor_x += item_spacing;
    const float body_width = 32.0f * scale;
    const float body_height = 20.0f * scale;
    const float tip_width = 4.0f * scale;
    const float tip_height = 10.0f * scale;
    const ImVec2 body_min(cursor_x, center_y - (body_height * 0.5f));
    const ImVec2 body_max(body_min.x + body_width, body_min.y + body_height);

    dl->AddRect(body_min, body_max, text_color, 3.0f, 0, 2.0f);
    dl->AddRectFilled(ImVec2(body_max.x, body_min.y + ((body_height - tip_height) * 0.5f)),
                      ImVec2(body_max.x + tip_width, body_min.y + ((body_height + tip_height) * 0.5f)),
                      text_color, 2.0f, ImDrawFlags_RoundCornersRight);

    const float pct = std::clamp(static_cast<float>(battery_level) / 100.0f, 0.0f, 1.0f);
    if (pct > 0.0f) {
        const float pad = 4.0f * scale;
        const ImVec2 fill_min(body_min.x + pad, body_min.y + pad);
        const ImVec2 fill_max(fill_min.x + ((body_width - (pad * 2.0f)) * pct), body_max.y - pad);
        dl->AddRectFilled(fill_min, fill_max, text_color, 1.5f);
    }

    if (charging) {
        const float icon_height = 16.0f * scale;
        const float icon_width = icon_height * (448.0f / 512.0f);
        const ImVec2 icon_min(body_max.x + tip_width + (6.0f * scale),
                              center_y - (icon_height * 0.5f));
        const auto BoltPoint = [&](float x, float y) {
            return ImVec2(icon_min.x + ((x / 448.0f) * icon_width),
                          icon_min.y + ((y / 512.0f) * icon_height));
        };

        dl->PathLineTo(BoltPoint(338.8f, -9.9f));
        dl->PathBezierCubicCurveTo(BoltPoint(350.7f, -1.3f), BoltPoint(355.1f, 14.3f),
                                   BoltPoint(349.7f, 27.9f));
        dl->PathLineTo(BoltPoint(271.3f, 224.0f));
        dl->PathLineTo(BoltPoint(416.0f, 224.0f));
        dl->PathBezierCubicCurveTo(BoltPoint(429.5f, 224.0f), BoltPoint(441.5f, 232.4f),
                                   BoltPoint(446.1f, 245.1f));
        dl->PathBezierCubicCurveTo(BoltPoint(450.7f, 257.8f), BoltPoint(446.8f, 272.0f),
                                   BoltPoint(436.5f, 280.6f));
        dl->PathLineTo(BoltPoint(148.5f, 520.6f));
        dl->PathBezierCubicCurveTo(BoltPoint(137.2f, 530.0f), BoltPoint(121.1f, 530.5f),
                                   BoltPoint(109.2f, 521.9f));
        dl->PathBezierCubicCurveTo(BoltPoint(97.3f, 513.3f), BoltPoint(92.9f, 497.7f),
                                   BoltPoint(98.3f, 484.1f));
        dl->PathLineTo(BoltPoint(176.7f, 288.0f));
        dl->PathLineTo(BoltPoint(32.0f, 288.0f));
        dl->PathBezierCubicCurveTo(BoltPoint(18.5f, 288.0f), BoltPoint(6.5f, 279.6f),
                                   BoltPoint(1.9f, 266.9f));
        dl->PathBezierCubicCurveTo(BoltPoint(-2.7f, 254.2f), BoltPoint(1.2f, 240.0f),
                                   BoltPoint(11.5f, 231.4f));
        dl->PathLineTo(BoltPoint(299.5f, -8.6f));
        dl->PathBezierCubicCurveTo(BoltPoint(310.8f, -18.0f), BoltPoint(326.9f, -18.5f),
                                   BoltPoint(338.8f, -9.9f));
        dl->PathFillConcave(IM_COL32(235, 235, 235, static_cast<int>(255.0f * ease)));
    }
}

int GetToastSlot(ToastCorner corner) {
    switch (corner) {
    case ToastCorner::TopLeft:
        return 0;
    case ToastCorner::TopRight:
        return 1;
    case ToastCorner::BottomLeft:
        return 2;
    case ToastCorner::BottomRight:
        return 3;
    }
    return 0;
}

bool GetAndAdvanceToast(ToastCorner corner, float delta_time, std::string* message, float* timer) {
    std::lock_guard lock(s_toast_mutex);
    const int slot = GetToastSlot(corner);
    if (s_toast_messages[slot].empty() || s_toast_timers[slot] <= 0.0f)
        return false;

    *message = s_toast_messages[slot];
    *timer = s_toast_timers[slot];
    s_toast_timers[slot] = std::max(0.0f, s_toast_timers[slot] - delta_time);
    if (s_toast_timers[slot] <= 0.0f)
        s_toast_messages[slot].clear();

    return true;
}

void RenderToastSlot(ImDrawList* dl, ToastCorner corner, float delta_time) {
    std::string message;
    float timer = 0.0f;
    if (!GetAndAdvanceToast(corner, delta_time, &message, &timer))
        return;

    const ImVec2 display_size = ImGui::GetIO().DisplaySize;
    const float scale = ImGui::GetIO().FontGlobalScale;
    const float alpha =
        timer < kToastFadeDuration ? std::clamp(timer / kToastFadeDuration, 0.0f, 1.0f) : 1.0f;
    const float margin_x = 24.0f * scale;
    const float margin_y = 16.0f * scale;
    const float pad_x = 16.0f * scale;
    const float pad_y = 8.0f * scale;
    const float rounding = 14.0f * scale;
    const ImVec2 text_size = ImGui::CalcTextSize(message.c_str());
    const float toast_w = text_size.x + pad_x * 2.0f;
    const float toast_h = text_size.y + pad_y * 2.0f;
    float toast_x = margin_x;
    float toast_y = margin_y;

    if (corner == ToastCorner::TopRight || corner == ToastCorner::BottomRight)
        toast_x = std::max(margin_x, display_size.x - margin_x - toast_w);
    if (corner == ToastCorner::BottomLeft || corner == ToastCorner::BottomRight)
        toast_y = std::max(margin_y, display_size.y - margin_y - toast_h);

    const ImVec2 p0(toast_x, toast_y);
    const ImVec2 p1(p0.x + text_size.x + pad_x * 2.0f, p0.y + text_size.y + pad_y * 2.0f);

    dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, static_cast<int>(153.0f * alpha)), rounding);
    dl->AddText(ImVec2(p0.x + pad_x, p0.y + pad_y),
                IM_COL32(255, 255, 255, static_cast<int>(240.0f * alpha)), message.c_str());
}

void RenderToast(ImDrawList* dl, float delta_time) {
    RenderToastSlot(dl, ToastCorner::TopLeft, delta_time);
    RenderToastSlot(dl, ToastCorner::TopRight, delta_time);
    RenderToastSlot(dl, ToastCorner::BottomLeft, delta_time);
    RenderToastSlot(dl, ToastCorner::BottomRight, delta_time);
}

// The FPS counter, rendered resolution and fast-forward marker, each in the
// corner its setting asks for. Position 0 is hidden; 1..4 are the corners in
// the order top left, top right, bottom left, bottom right.
void RenderHud(ImDrawList* dl, ImVec2 display_size) {
    HudStats stats;
    int fps_position;
    int resolution_position;
    {
        std::lock_guard lock(s_hud_mutex);
        stats = s_hud_stats;
        fps_position = s_fps_position;
        resolution_position = s_resolution_position;
    }

    std::array<std::string, 5> lines{};
    if (fps_position > 0) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.0f FPS", stats.fps);
        lines[fps_position] = text;
    }
    if (resolution_position > 0 && stats.rendered_width > 0 && stats.rendered_height > 0) {
        char text[32];
        std::snprintf(text, sizeof(text), "%dx%d", stats.rendered_width, stats.rendered_height);
        std::string& line = lines[resolution_position];
        line = line.empty() ? text : line + "  " + text;
    }
    if (stats.fast_forward) {
        std::string& line = lines[2];
        line = line.empty() ? std::string(">>") : std::string(">>  ") + line;
    }

    const float scale = ImGui::GetIO().FontGlobalScale;
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize() * 0.6f;
    const float margin = 12.0f * scale;
    const float pad_x = 10.0f * scale;
    const float pad_y = 4.0f * scale;
    for (int position = 1; position <= 4; ++position) {
        const std::string& line = lines[position];
        if (line.empty()) {
            continue;
        }
        const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, line.c_str());
        const float box_w = text_size.x + (pad_x * 2.0f);
        const float box_h = text_size.y + (pad_y * 2.0f);
        const bool right = position == 2 || position == 4;
        const bool bottom = position == 3 || position == 4;
        const ImVec2 p0(right ? display_size.x - margin - box_w : margin,
                        bottom ? display_size.y - margin - box_h : margin);
        dl->AddRectFilled(p0, ImVec2(p0.x + box_w, p0.y + box_h), IM_COL32(0, 0, 0, 140),
                          8.0f * scale);
        dl->AddText(font, font_size, ImVec2(p0.x + pad_x, p0.y + pad_y),
                    IM_COL32(255, 255, 255, 230), line.c_str());
    }
}

bool HudActive() {
    std::lock_guard lock(s_hud_mutex);
    return s_fps_position > 0 || s_resolution_position > 0 || s_hud_stats.fast_forward;
}

void OpenScreen(MenuScreen screen) {
    s_menu = screen;
    s_selected = 0;
}

bool IsActionInRange(Action action, Action first, Action last) {
    const int value = static_cast<int>(action);
    return value >= static_cast<int>(first) && value <= static_cast<int>(last);
}

// A setting was changed from the menu: tell the emulation thread, and say so
// when the change only takes effect on the next launch.
void OnOptionChanged(const TicoConfig::OptionDef& option) {
    ReloadHudPositions();
    {
        std::lock_guard lock(s_pending_mutex);
        s_settings_changed = true;
    }
    if (option.needs_restart) {
        ShowToast(TrOr("emulator_applies_next_launch", "Applies the next time the game starts"),
                  ToastCorner::TopRight);
    }
}

// What A does on the selected row. Returns the action for the emulation thread, if any.
Action AcceptSelection(const std::vector<MenuRow>& rows) {
    if (rows.empty() || s_selected < 0 || s_selected >= static_cast<int>(rows.size())) {
        return Action::None;
    }

    switch (s_menu) {
    case MenuScreen::QuickMenu: {
        const std::vector<QuickItem> items = BuildQuickItems();
        s_quick_selected = s_selected;
        switch (items[static_cast<std::size_t>(s_selected)]) {
        case QuickItem::SaveState:
            RefreshSlots();
            OpenScreen(MenuScreen::SaveStates);
            break;
        case QuickItem::LoadState:
            RefreshSlots();
            OpenScreen(MenuScreen::LoadStates);
            break;
        case QuickItem::Rewind:
            RefreshRewindPoints();
            OpenScreen(MenuScreen::Rewind);
            break;
        case QuickItem::ChangeDisc:
            OpenScreen(MenuScreen::Discs);
            for (std::size_t i = 0; i < s_disc_entries.size(); ++i) {
                if (s_disc_entries[i].current) {
                    s_selected = static_cast<int>(i);
                }
            }
            break;
        case QuickItem::Cheats:
            RefreshCheats();
            OpenScreen(MenuScreen::Cheats);
            break;
        case QuickItem::Settings:
            OpenScreen(MenuScreen::SettingsCategories);
            break;
        case QuickItem::Reset:
            return Action::Reset;
        case QuickItem::Exit:
            return Action::Exit;
        }
        return Action::None;
    }
    case MenuScreen::SaveStates:
        return MakeSaveActionForSlot(s_selected);
    case MenuScreen::LoadStates:
        if (!s_slot_occupied[static_cast<std::size_t>(s_selected)]) {
            return Action::None;
        }
        return MakeLoadActionForSlot(s_selected);
    case MenuScreen::Rewind: {
        if (s_rewind_points.empty()) {
            return Action::None;
        }
        std::lock_guard lock(s_pending_mutex);
        s_rewind_index = s_selected;
        return Action::Rewind;
    }
    case MenuScreen::Discs: {
        if (s_disc_entries[static_cast<std::size_t>(s_selected)].current) {
            return Action::None;
        }
        std::lock_guard lock(s_pending_mutex);
        s_disc_index = s_selected;
        return Action::SwapDisc;
    }
    case MenuScreen::Cheats:
        if (!s_cheat_entries.empty() && s_cheat_toggle_cb) {
            const CheatMenuEntry& entry = s_cheat_entries[static_cast<std::size_t>(s_selected)];
            if (entry.is_add_row) {
                return Action::AddCheat;
            }
            if (entry.toggleable && s_cheat_toggle_cb(entry.source_index)) {
                RefreshCheats();
            }
        }
        return Action::None;
    case MenuScreen::SettingsCategories:
        s_category_selected = s_selected;
        OpenScreen(MenuScreen::SettingsOptions);
        return Action::None;
    case MenuScreen::SettingsOptions: {
        const TicoConfig::OptionDef* selected = SelectedOption();
        if (!selected) {
            return Action::None;
        }
        const TicoConfig::OptionDef& option = *selected;
        if (option.type == TicoConfig::OptionType::Text) {
            std::lock_guard lock(s_pending_mutex);
            s_text_edit_option = &option;
            return Action::EditText;
        }
        TicoConfig::StepOption(option, 1);
        OnOptionChanged(option);
        return Action::None;
    }
    }
    return Action::None;
}

// What B does: one level up, or close the menu from the top.
Action CancelScreen() {
    switch (s_menu) {
    case MenuScreen::QuickMenu:
        return Action::Resume;
    case MenuScreen::SettingsOptions:
        s_menu = MenuScreen::SettingsCategories;
        s_selected = s_category_selected;
        break;
    default:
        s_menu = MenuScreen::QuickMenu;
        s_selected = s_quick_selected;
        break;
    }
    return Action::None;
}

} // namespace

void SetVisible(bool visible) {
    if (visible && !s_visible) {
        OverlayTranslation::TranslationManager::Instance().Init();
        s_anim_timer = 0.0f;
        s_quick_selected = 0;
        s_category_selected = 0;
        RefreshDiscs();
        OpenScreen(MenuScreen::QuickMenu);
    } else if (!visible) {
        s_anim_timer = 0.0f;
        s_cheat_entries.clear();
        s_rewind_points.clear();
        s_disc_entries.clear();
        OpenScreen(MenuScreen::QuickMenu);
    }

    s_visible = visible;
}

void SetGameTitle(std::string title) {
    s_title = std::move(title);
}

void SetNickname(std::string nickname) {
    s_nickname = std::move(nickname);
}

void SetAvatarTextureId(unsigned long long texture_id) {
    s_avatar_texture_id = texture_id;
}

void SetBorderTextureId(unsigned long long texture_id) {
    s_border_texture_id = texture_id;
}

void SetSlotOccupiedCallback(SlotOccupiedFn callback) {
    s_slot_occupied_cb = std::move(callback);
}

void SetCheatCallbacks(CheatListFn list_callback, CheatToggleFn toggle_callback) {
    s_cheat_list_cb = std::move(list_callback);
    s_cheat_toggle_cb = std::move(toggle_callback);
    s_cheat_entries.clear();
}

void RefreshCheatList() {
    RefreshCheats();
}

void SetRewindCallback(RewindListFn callback) {
    s_rewind_list_cb = std::move(callback);
    s_rewind_points.clear();
}

void SetDiscCallback(DiscListFn callback) {
    s_disc_list_cb = std::move(callback);
    s_disc_entries.clear();
}

void SetHudStats(const HudStats& stats) {
    std::lock_guard lock(s_hud_mutex);
    s_hud_stats = stats;
}

void ReloadSettings() {
    ReloadHudPositions();
}

void ShowToast(std::string message, ToastCorner corner) {
    std::lock_guard lock(s_toast_mutex);
    if (message.empty()) {
        for (std::string& toast_message : s_toast_messages)
            toast_message.clear();
        s_toast_timers.fill(0.0f);
        return;
    }

    const int slot = GetToastSlot(corner);
    s_toast_messages[slot] = std::move(message);
    s_toast_timers[slot] = kToastDuration;
}

bool HasTransientContent() {
    if (HudActive()) {
        return true;
    }
    std::lock_guard lock(s_toast_mutex);
    for (int i = 0; i < kToastSlotCount; ++i) {
        if (!s_toast_messages[i].empty() && s_toast_timers[i] > 0.0f)
            return true;
    }
    return false;
}

bool ConsumeSettingsChanged() {
    std::lock_guard lock(s_pending_mutex);
    const bool changed = s_settings_changed;
    s_settings_changed = false;
    return changed;
}

int ConsumeRewindIndex() {
    std::lock_guard lock(s_pending_mutex);
    const int index = s_rewind_index;
    s_rewind_index = -1;
    return index;
}

int ConsumeDiscIndex() {
    std::lock_guard lock(s_pending_mutex);
    const int index = s_disc_index;
    s_disc_index = -1;
    return index;
}

const TicoConfig::OptionDef* ConsumeTextEditOption() {
    std::lock_guard lock(s_pending_mutex);
    const TicoConfig::OptionDef* option = s_text_edit_option;
    s_text_edit_option = nullptr;
    return option;
}

void NotifyOptionEdited(const TicoConfig::OptionDef& option) {
    OnOptionChanged(option);
}

void FeedNav(const NavInput& nav) {
    s_nav = nav;
}

Action Render(int display_w, int display_h) {
    const float delta_time = ImGui::GetIO().DeltaTime;
    const ImVec2 display_size(static_cast<float>(display_w), static_cast<float>(display_h));
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    if (!s_visible) {
        RenderHud(dl, display_size);
        RenderToast(dl, delta_time);
        return Action::None;
    }

    const NavInput nav = s_nav;
    s_nav = {};

    s_anim_timer = std::min(s_anim_timer + delta_time, kAnimDuration);
    const float ease = EaseOutCubic(s_anim_timer / kAnimDuration);

    std::vector<MenuRow> rows = BuildRows();
    const int item_count = static_cast<int>(rows.size());
    if (item_count > 0) {
        s_selected = std::clamp(s_selected, 0, item_count - 1);
        if (nav.up)
            s_selected = (s_selected - 1 + item_count) % item_count;
        if (nav.down)
            s_selected = (s_selected + 1) % item_count;
    }

    const TicoConfig::OptionDef* stepped =
        s_menu == MenuScreen::SettingsOptions && (nav.left || nav.right) ? SelectedOption() : nullptr;
    if (stepped) {
        const TicoConfig::OptionDef& option = *stepped;
        if (option.type != TicoConfig::OptionType::Text) {
            TicoConfig::StepOption(option, nav.right ? 1 : -1);
            OnOptionChanged(option);
            rows = BuildRows();
        }
    }

    // after the options were stepped, so the right that opens them changes nothing
    if (s_menu == MenuScreen::SettingsCategories) {
        s_category_selected = s_selected;
        if (nav.right && !nav.accept) {
            OpenScreen(MenuScreen::SettingsOptions);
            rows = BuildRows();
        }
    }

    Action result = Action::None;
    if (nav.cancel) {
        result = CancelScreen();
        rows = BuildRows();
    }

    RenderOverlayBackground(dl, display_size, ease);
    RenderTitleCard(dl, display_size, ease);
    if (s_menu == MenuScreen::SettingsCategories || s_menu == MenuScreen::SettingsOptions) {
        RenderSettings(dl, display_size, ease);
    } else {
        RenderMenu(dl, display_size, ease, rows);
    }
    RenderHelpersBar(dl, display_size, ease);
    RenderSocialArea(dl, ease);
    RenderStatusBar(dl, display_size, ease);
    RenderToast(dl, delta_time);

    if (nav.accept && result == Action::None) {
        result = AcceptSelection(rows);
    }

    return result;
}

bool IsSaveStateAction(Action action) {
    return IsActionInRange(action, Action::SaveStateSlot1, Action::SaveStateSlot4);
}

bool IsLoadStateAction(Action action) {
    return IsActionInRange(action, Action::LoadStateSlot1, Action::LoadStateSlot4);
}

int GetStateSlotForAction(Action action) {
    if (IsSaveStateAction(action))
        return static_cast<int>(action) - static_cast<int>(Action::SaveStateSlot1) + 1;
    if (IsLoadStateAction(action))
        return static_cast<int>(action) - static_cast<int>(Action::LoadStateSlot1) + 1;
    return 0;
}

} // namespace SwitchFrontend::OverlayUI
