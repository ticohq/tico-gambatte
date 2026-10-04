// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "overlay/overlay_ui.h"

class TicoCore;

namespace SwitchFrontend::ImGuiOverlay {

// Loads the avatar and the selection border as OpenGL textures. Call once the
// GL context and the ImGui context exist.
bool Init();
void Shutdown();

// Shows or hides the quick menu (the HUD and toasts are drawn either way).
void SetVisible(bool visible);
bool IsVisible();

// Edge-triggered menu navigation for the next drawn frame.
void FeedNav(const OverlayUI::NavInput& nav);

// Draws the menu, HUD, toasts and RetroAchievements alerts into the current
// ImGui frame, over a width x height display.
void Draw(TicoCore* core, float width, float height, float delta_time);

// The action the menu returned while drawing, once; None when there was none.
OverlayUI::Action ConsumeAction();

} // namespace SwitchFrontend::ImGuiOverlay
