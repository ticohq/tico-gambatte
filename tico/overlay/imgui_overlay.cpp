// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "overlay/imgui_overlay.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "glad.h"
#include "imgui.h"
#define STB_IMAGE_IMPLEMENTATION
#include "deps/stb/stb_image.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "TicoLogger.h"
#include "overlay/ra_alerts.h"
#include "overlay/tico_config.h"

namespace SwitchFrontend::ImGuiOverlay {
namespace {

constexpr std::array<const char*, 6> kAvatarPaths = {{
    "sdmc:/tico/assets/avatar.jpg",
    "sdmc:/tico/assets/avatar.jpeg",
    "sdmc:/tico/assets/avatar.png",
    "romfs:/assets/avatar.jpg",
    "romfs:/assets/avatar.jpeg",
    "romfs:/assets/avatar.png",
}};
// Selection border strips, as tico-nx ships them: index 0 and "original" (the
// last) use the cyan/violet strip, every other tint has its own. The slugs
// mirror tico-nx's TintPalette::GetBorderGradientFile.
constexpr const char* kBorderDir = "romfs:/assets/border/";
constexpr std::array<const char*, 13> kBorderSlugs = {{
    "default", "aqua", "violet", "sunset", "lime", "rose", "gold", "ice", "ember", "mint",
    "lagoon", "cobalt", "original",
}};

bool s_initialized = false;
bool s_visible = false;
bool s_psm_initialized = false;
OverlayUI::NavInput s_nav{};
OverlayUI::Action s_action = OverlayUI::Action::None;
GLuint s_avatar_texture = 0;
GLuint s_border_texture = 0;

// Uploads an RGBA image decoded by stb_image and frees it. Returns the GL
// texture, or 0 when there was no image.
GLuint Upload(unsigned char* rgba, int width, int height, const char* source) {
    if (!rgba) {
        return 0;
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(rgba);
    LOG_INFO("OVERLAY", "loaded %s (%dx%d)", source, width, height);
    return texture;
}

void LoadBorder() {
    const int tint = TicoConfig::BorderTint();
    const int original = static_cast<int>(kBorderSlugs.size()) - 1;
    std::string path = kBorderDir;
    if (tint <= 0 || tint >= original) {
        path += "border_gradient.png";
    } else {
        path += std::string("border_gradient_") + kBorderSlugs[static_cast<std::size_t>(tint)] +
                ".png";
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    s_border_texture =
        Upload(stbi_load(path.c_str(), &width, &height, &channels, 4), width, height, path.c_str());
    if (!s_border_texture) {
        LOG_WARN("OVERLAY", "no selection border strip at %s", path.c_str());
    }
    OverlayUI::SetBorderTextureId(s_border_texture);
}

#ifdef __SWITCH__
bool LoadAvatarFromAccount() {
    if (R_FAILED(accountInitialize(AccountServiceType_Application))) {
        return false;
    }

    AccountUid uid{};
    bool found = R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid);
    if (!found) {
        found = R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid);
    }
    if (!found) {
        AccountUid uids[ACC_USER_LIST_SIZE]{};
        s32 total = 0;
        if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &total)) && total > 0) {
            uid = uids[0];
            found = accountUidIsValid(&uid);
        }
    }

    AccountProfile profile{};
    if (found && R_SUCCEEDED(accountGetProfile(&profile, uid))) {
        AccountProfileBase profile_base{};
        if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &profile_base)) &&
            profile_base.nickname[0] != '\0') {
            OverlayUI::SetNickname(profile_base.nickname);
        }
        u32 image_size = 0;
        if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &image_size)) && image_size > 0) {
            std::vector<unsigned char> jpeg(image_size);
            u32 actual = 0;
            if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpeg.data(), image_size, &actual)) &&
                actual > 0) {
                int width = 0;
                int height = 0;
                int channels = 0;
                s_avatar_texture =
                    Upload(stbi_load_from_memory(jpeg.data(), static_cast<int>(actual), &width,
                                                 &height, &channels, 4),
                           width, height, "switch-account-avatar");
            }
        }
        accountProfileClose(&profile);
    }
    accountExit();
    return s_avatar_texture != 0;
}
#endif

void LoadAvatar() {
    for (const char* path : kAvatarPaths) {
        int width = 0;
        int height = 0;
        int channels = 0;
        s_avatar_texture = Upload(stbi_load(path, &width, &height, &channels, 4), width, height, path);
        if (s_avatar_texture) {
            break;
        }
    }
#ifdef __SWITCH__
    if (!s_avatar_texture && !LoadAvatarFromAccount()) {
        LOG_INFO("OVERLAY", "no avatar image found");
    }
#endif
    OverlayUI::SetAvatarTextureId(s_avatar_texture);
}

} // namespace

bool Init() {
    if (s_initialized) {
        return true;
    }
#ifdef __SWITCH__
    if (!s_psm_initialized && R_SUCCEEDED(psmInitialize())) {
        s_psm_initialized = true;
    }
#endif
    LoadAvatar();
    LoadBorder();
    s_visible = false;
    s_action = OverlayUI::Action::None;
    s_initialized = true;
    LOG_INFO("OVERLAY", "tico overlay initialized");
    return true;
}

void Shutdown() {
    if (!s_initialized) {
        return;
    }
    for (GLuint* texture : {&s_avatar_texture, &s_border_texture}) {
        if (*texture) {
            glDeleteTextures(1, texture);
            *texture = 0;
        }
    }
    OverlayUI::SetAvatarTextureId(0);
    OverlayUI::SetBorderTextureId(0);
    OverlayUI::SetVisible(false);
    OverlayUI::ShowToast(std::string{});
#ifdef __SWITCH__
    if (s_psm_initialized) {
        psmExit();
        s_psm_initialized = false;
    }
#endif
    s_initialized = false;
}

void SetVisible(bool visible) {
    s_visible = visible;
    s_nav = {};
    OverlayUI::SetVisible(visible);
}

bool IsVisible() {
    return s_visible;
}

void FeedNav(const OverlayUI::NavInput& nav) {
    s_nav.up |= nav.up;
    s_nav.down |= nav.down;
    s_nav.left |= nav.left;
    s_nav.right |= nav.right;
    s_nav.accept |= nav.accept;
    s_nav.cancel |= nav.cancel;
}

void Draw(TicoCore* core, float width, float height, float delta_time) {
    if (!s_initialized) {
        return;
    }
    OverlayUI::FeedNav(s_nav);
    s_nav = {};
    const OverlayUI::Action action =
        OverlayUI::Render(static_cast<int>(width), static_cast<int>(height));
    RAAlerts::Render(core, ImGui::GetForegroundDrawList(), ImVec2(width, height), delta_time);
    if (action != OverlayUI::Action::None) {
        s_action = action;
    }
}

OverlayUI::Action ConsumeAction() {
    const OverlayUI::Action action = s_action;
    s_action = OverlayUI::Action::None;
    return action;
}

} // namespace SwitchFrontend::ImGuiOverlay
