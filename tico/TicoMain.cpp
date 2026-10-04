/// @file TicoMain.cpp
/// @brief Entry point for tico-integrated gambatte NRO
/// Sets up SDL/Vulkan/ImGui and runs the main loop

#include "TicoCore.h"
#include "TicoConfig.h"
#include "TicoAudio.h"
#include "TicoShaderChain.h"
#include "TicoVulkan.h"
#include "overlay/imgui_overlay.h"
#include "overlay/overlay_ui.h"
#include "overlay/tico_config.h"
#include "overlay/translation_manager.h"

#include <SDL.h>
#include <dirent.h>
#include <json.hpp>
#include <strings.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <cstring>
#include "TicoUtils.h"
#include "TicoLogger.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "imgui.h"
#include "imgui_impl_sdl2.h"

//==============================================================================
// NX System Configuration (extern "C")
//==============================================================================

extern "C" {
u32 __NvOptimusEnablement = 1;
u32 __NvDeveloperOption = 1;
u32 __nx_applet_type = AppletType_Application;
u32 __nx_applet_exit_mode = 0; // 0 = standard exit (return to Homebrew ABI loader if NRO). 1 = forceful applet exit
size_t __nx_heap_size = 0;
}

//==============================================================================
// Globals
//==============================================================================

static SDL_Window *g_window = nullptr;
static std::unique_ptr<TicoShaderChain> g_chain;
static std::string g_activePreset = "\x01"; // forces the first load

namespace OverlayUI = SwitchFrontend::OverlayUI;
namespace ImGuiOverlay = SwitchFrontend::ImGuiOverlay;
namespace OverlayConfig = SwitchFrontend::TicoConfig;

static std::unique_ptr<TicoCore> g_core;

// Quick menu
static bool g_menuOpen = false;
static bool g_overlayReady = false;
static bool g_toggleHeld = false;
static uint32_t g_navHeldPrev = 0;
static int g_navRepeatFrames = 0;
static constexpr int kNavInitialDelayFrames = 14;
static constexpr int kNavRepeatFrames = 6;

// HUD frame counter
static int g_hudFrames = 0;
static float g_hudSeconds = 0.0f;
static float g_hudFps = 0.0f;

static bool g_running = true;
static TicoAudio g_audio;
static SDL_AudioDeviceID g_audioDevice = 0;
static SDL_GameController *g_controllers[4] = {nullptr, nullptr, nullptr, nullptr};
static bool g_controllersDirty = true;



#ifdef __SWITCH__
static u8 g_lastOperationMode = 255;

static void ApplySwitchPerformanceProfile()
{
    Result rcNormal = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, 0x92220007);
    Result rcBoost = apmSetPerformanceConfiguration(ApmPerformanceMode_Boost, 0x92220008);
    if (R_FAILED(rcNormal) || R_FAILED(rcBoost))
    {
        LOG_WARN("HOME", "Switch performance profile failed (normal=0x%x boost=0x%x)", rcNormal, rcBoost);
    }
    else
    {
        LOG_INFO("HOME", "Applied Switch performance profile");
    }
}

static void PinCurrentThreadToCore(int core, const char *label)
{
    if (core < 0 || core > 2)
        return;

    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    if (R_FAILED(rc))
    {
        LOG_WARN("HOME", "Failed to pin %s thread to core %d (rc=0x%x)", label, core, rc);
    }
    else
    {
        LOG_INFO("HOME", "Pinned %s thread to core %d", label, core);
    }
}

static bool UpdateScreenMode()
{
    u8 operationMode = appletGetOperationMode();
    if (operationMode == g_lastOperationMode)
        return false;

    // Size the window to the mode and crop from the top-left, as tico-dolphin
    // does, so the swapchain always matches what is on screen.
    const bool handheld = operationMode == AppletOperationMode_Handheld;
    const u32 w = handheld ? 1280 : 1920, h = handheld ? 720 : 1080;
    nwindowSetDimensions(nwindowGetDefault(), w, h);
    nwindowSetCrop(nwindowGetDefault(), 0, 0, w, h);
    TicoVulkan::Resize(w, h);
    LOG_INFO("DISPLAY", "Mode → %s (%ux%u)", handheld ? "Handheld" : "Docked", w, h);
    if (ImGui::GetCurrentContext()) {
        ImGui::GetIO().FontGlobalScale = handheld ? 1.0f : 1.5f;
    }
    g_lastOperationMode = operationMode;
    return true;
}
#endif

//==============================================================================
// SDL/Vulkan Initialization
//==============================================================================

static void CloseControllers()
{
    for (SDL_GameController *&controller : g_controllers)
    {
        if (controller)
        {
            SDL_GameControllerClose(controller);
            controller = nullptr;
        }
    }
}

static void RefreshControllers()
{
    CloseControllers();

    int controllerIndex = 0;
    int joystickCount = SDL_NumJoysticks();

    for (int i = 0; i < joystickCount && controllerIndex < 4; ++i)
    {
        if (!SDL_IsGameController(i))
            continue;

        SDL_GameController *controller = SDL_GameControllerOpen(i);
        if (!controller)
        {
            LOG_WARN("INPUT", "Failed to open controller %d: %s", i, SDL_GetError());
            continue;
        }

        g_controllers[controllerIndex++] = controller;
    }

    g_controllersDirty = false;
}

static void GetDisplayResolution(int &w, int &h)
{
#ifdef __SWITCH__
    u8 opMode = appletGetOperationMode();
    if (opMode == AppletOperationMode_Handheld)
    {
        w = 1280;
        h = 720;
    }
    else
    {
        w = 1920;
        h = 1080;
    }
#else
    uint32_t sw = 0, sh = 0;
    TicoVulkan::GetSwapExtent(sw, sh);
    if (sw && sh)
    {
        w = (int)sw;
        h = (int)sh;
    }
    else if (g_window)
        SDL_GetWindowSize(g_window, &w, &h);
    else
    {
        w = 1280;
        h = 720;
    }
#endif
}

bool InitWindow()
{
    LOG_INFO("HOME", "Starting initialization...");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER |
                 SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
    {
        LOG_ERROR("HOME", "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    LOG_INFO("HOME", "SDL initialized");

#ifdef __SWITCH__
    g_window = nullptr;
    LOG_INFO("HOME", "Switch: skipping SDL window (using native window)");

    UpdateScreenMode();
    int w, h;
    GetDisplayResolution(w, h);
    LOG_INFO("HOME", "Switch Resolution: %dx%d (logical)", w, h);

#else
    g_window = SDL_CreateWindow("gambatte",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                TicoConfig::WINDOW_WIDTH, TicoConfig::WINDOW_HEIGHT,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);

    if (!g_window)
    {
        LOG_ERROR("HOME", "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
#endif

    if (TicoConfig::USE_SDLQUEUEAUDIO)
    {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = TicoAudio::SAMPLE_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = TicoAudio::CHANNELS;
        want.samples = 2048;
        want.callback = NULL;

        g_audioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (g_audioDevice == 0)
        {
            LOG_ERROR("AUDIO", "SDL_OpenAudioDevice failed: %s", SDL_GetError());
        }
        else
        {
            LOG_INFO("AUDIO", "SDL_QueueAudio initialized. DeviceID: %d, Freq: %d", g_audioDevice, have.freq);
        }
    }
    else
    {
        if (Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 1024) < 0)
        {
            LOG_ERROR("AUDIO", "Mix_OpenAudio failed: %s", Mix_GetError());
        }
        else
        {
            LOG_INFO("AUDIO", "SDL_mixer initialized");
        }
    }

    return true;
}

static void AudioSampleCallback(int16_t left, int16_t right)
{
    g_audio.PushSample(left, right);
}

static size_t AudioSampleBatchCallback(const int16_t *data, size_t frames)
{
    return g_audio.PushSamples(data, frames);
}

static void VideoCallback(const void *data, unsigned width, unsigned height, size_t pitch,
                          retro_pixel_format format)
{
    if (g_chain)
        g_chain->SetSourceFrame(data, width, height, pitch, format);
}

static void AudioFlushCallback()
{
    g_audio.Flush();
    LOG_INFO("AUDIO", "Audio flushed");
}

bool InitImGui()
{
    LOG_INFO("HOME", "InitImGui starting...");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    LOG_INFO("HOME", "ImGui context created");

    // Switch has no SDL window and the overlay reads the pads itself, so the
    // SDL platform backend is desktop-only.
#ifndef __SWITCH__
    ImGui_ImplSDL2_InitForVulkan(g_window);
#endif
    int w, h;
    GetDisplayResolution(w, h);
    if (!TicoVulkan::Init(g_window, (uint32_t)w, (uint32_t)h))
    {
        LOG_ERROR("HOME", "Vulkan initialization failed");
        return false;
    }
    LOG_INFO("HOME", "ImGui backends initialized");

#ifdef __SWITCH__
    ImFontConfig fontCfg;
    fontCfg.SizePixels = TicoConfig::FONT_SIZE;
    if (io.Fonts->AddFontFromFileTTF(TicoConfig::FONT_PATH, TicoConfig::FONT_SIZE))
    {
        LOG_INFO("HOME", "Loaded ImGui font from %s", TicoConfig::FONT_PATH);
    }
    else if (!io.Fonts->AddFontDefault(&fontCfg))
    {
        LOG_ERROR("HOME", "Failed to load font from romfs and built-in ImGui fallback");
        return false;
    }
    else
    {
        LOG_WARN("HOME", "Failed to load %s, using built-in ImGui font", TicoConfig::FONT_PATH);
    }
    // Load secondary font for RA alert descriptions
    io.Fonts->AddFontFromFileTTF("romfs:/fonts/description.ttf", TicoConfig::FONT_SIZE * 0.75f);
#else
    if (!io.Fonts->AddFontFromFileTTF("tico/fonts/font.ttf", TicoConfig::FONT_SIZE))
    {
        LOG_ERROR("HOME", "Failed to load ImGui font from tico/fonts/font.ttf");
        return false;
    }
    // Load secondary font for RA alert descriptions
    io.Fonts->AddFontFromFileTTF("tico/fonts/description.ttf", TicoConfig::FONT_SIZE * 0.75f);
#endif

    LOG_INFO("HOME", "ImGui initialized");
    return true;
}

void CleanupWindow()
{
    CloseControllers();

    g_chain.reset();
    TicoSlang::Shutdown();
    TicoVulkan::Shutdown();
#ifndef __SWITCH__
    ImGui_ImplSDL2_Shutdown();
#endif
    ImGui::DestroyContext();

    if (g_window)
    {
        SDL_DestroyWindow(g_window);
    }

    SDL_Quit();
}

//==============================================================================
// Main Loop
//==============================================================================

void ProcessEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
#ifndef __SWITCH__
        ImGui_ImplSDL2_ProcessEvent(&event);
#endif

        if (event.type == SDL_QUIT)
        {
            LOG_INFO("HOME", "Received SDL_QUIT event");
            g_running = false;
        }

        if (event.type == SDL_CONTROLLERDEVICEADDED ||
            event.type == SDL_CONTROLLERDEVICEREMOVED ||
            event.type == SDL_JOYDEVICEADDED ||
            event.type == SDL_JOYDEVICEREMOVED)
        {
            g_controllersDirty = true;
        }

#ifdef __SWITCH__
        if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
        {
            LOG_INFO("HOME", "Received Escape key event, requesting exit");
            g_running = false;
        }
#endif
    }
}

//==============================================================================
// Quick menu
//==============================================================================

static void ChainloadTico()
{
#ifdef __SWITCH__
    const char *primaryNro = "sdmc:/switch/tico.nro";
    const char *fallbackNro = "sdmc:/switch/tico/tico.nro";
    const char *targetNro = nullptr;

    struct stat buffer;
    if (stat(primaryNro, &buffer) == 0)
        targetNro = primaryNro;
    else if (stat(fallbackNro, &buffer) == 0)
        targetNro = fallbackNro;

    if (targetNro != nullptr)
    {
        // Build args as space-separated string (per libnx envSetNextLoad docs)
        char args[512];
        snprintf(args, sizeof(args), "%s --resume", targetNro);
        envSetNextLoad(targetNro, args);
        LOG_INFO("HOME", "Chainloading back to %s with args: %s", targetNro, args);
    }
    else
    {
        LOG_WARN("HOME", "Chainload target not found! Exiting normally.");
    }
    remove("imgui.ini");
#endif
}

static std::string StatePath(int slot)
{
    std::string romName = g_core ? g_core->GetGamePath() : std::string();
    size_t lastSlash = romName.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        romName = romName.substr(lastSlash + 1);
    size_t lastDot = romName.find_last_of('.');
    if (lastDot != std::string::npos)
        romName = romName.substr(0, lastDot);
    const std::string dir = TicoConfig::StatesPath();
    TicoConfig::MakeDirs(dir);
    return dir + romName + ".state" + std::to_string(slot);
}

//==============================================================================
// Shaders
//==============================================================================

#ifdef __SWITCH__
static const char *kBuiltinShaderDir = "romfs:/shaders/";
static const char *kUserShaderDir = "sdmc:/tico/shaders/";
#else
static const char *kBuiltinShaderDir = "tico/shaders/";
static const char *kUserShaderDir = "shaders/";
#endif

// The built-ins, with the names the menu shows for them.
static const std::pair<const char *, const char *> kBuiltinShaders[] = {
    {"lcd.slangp", "LCD"},
    {"lcd-grid-v2.slangp", "LCD Grid"},
    {"dot.slangp", "Dot Matrix"},
    {"xbrz.slangp", "xBRZ"},
    {"eagle.slangp", "Eagle"},
    {"crt-easymode.slangp", "CRT Easy Mode"},
};

static bool EndsWith(const std::string &s, const char *suffix)
{
    const size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

static std::string ShaderPreset()
{
    return OverlayConfig::GetConfigValue("shader_preset", "");
}

static void SetShaderPreset(const std::string &path)
{
    OverlayConfig::SetConfigValue("shader_preset", path);
    OverlayConfig::SaveConfig();
}

// Settings from before slang presets picked one of the old GL shaders.
static void MigrateShaderSetting()
{
    if (OverlayConfig::GetConfigValue("shader_preset", "\x01") != "\x01")
        return; // already chosen, "" included
    static const std::pair<const char *, const char *> kOld[] = {
        {"LCD", "lcd.slangp"}, {"xBRZ", "xbrz.slangp"}, {"Eagle", "eagle.slangp"},
        {"Dot", "dot.slangp"}, {"LcdGridV2", "lcd-grid-v2.slangp"},
    };
    const std::string old = OverlayConfig::GetConfigValue("shader_type", "None");
    for (const auto &entry : kOld)
        if (old == entry.first)
            SetShaderPreset(kBuiltinShaderDir + std::string(entry.second));
}

static std::string ShaderPresetLabel()
{
    const std::string preset = ShaderPreset();
    if (preset.empty())
        return std::string();
    for (const auto &builtin : kBuiltinShaders)
        if (preset == kBuiltinShaderDir + std::string(builtin.first))
            return builtin.second;
    std::string name = preset;
    const size_t slash = name.find_last_of('/');
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    return EndsWith(name, ".slangp") ? name.substr(0, name.size() - 7) : name;
}

// The browser: the user folder lists the built-ins first, every other folder
// its parent; then subfolders and presets, by name.
static std::vector<OverlayUI::ShaderBrowseEntry> BrowseShaders(std::string dir)
{
    using Entry = OverlayUI::ShaderBrowseEntry;
    if (dir.empty() || dir.back() != '/')
        dir += '/';
    std::vector<Entry> entries;
    if (dir == kUserShaderDir)
    {
        entries.push_back({"> " + SwitchFrontend::OverlayTranslation::tr("emulator_builtin_shaders"),
                           kBuiltinShaderDir, true});
        entries.push_back({SwitchFrontend::OverlayTranslation::tr("emulator_none"), "", false});
    }
    else
    {
        std::string parent = kUserShaderDir;
        if (dir != kBuiltinShaderDir)
        {
            const std::string d = dir.substr(0, dir.size() - 1);
            const size_t slash = d.find_last_of('/');
            if (slash != std::string::npos)
                parent = d.substr(0, slash + 1);
        }
        entries.push_back({"..", parent, true});
    }
    if (dir == kBuiltinShaderDir)
    {
        for (const auto &builtin : kBuiltinShaders)
            entries.push_back({builtin.second, dir + builtin.first, false});
        return entries;
    }

    std::vector<Entry> dirs, files;
    if (DIR *d = opendir(dir.c_str()))
    {
        while (struct dirent *e = readdir(d))
        {
            const std::string name = e->d_name;
            if (name.empty() || name[0] == '.')
                continue;
            const std::string path = dir + name;
            bool isDir = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN)
            {
                struct stat st;
                isDir = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            }
            if (isDir)
                dirs.push_back({name + "/", path + "/", true});
            else if (EndsWith(name, ".slangp"))
                files.push_back({name.substr(0, name.size() - 7), path, false});
        }
        closedir(d);
    }
    auto byName = [](const Entry &a, const Entry &b) {
        return strcasecmp(a.label.c_str(), b.label.c_str()) < 0;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    entries.insert(entries.end(), dirs.begin(), dirs.end());
    entries.insert(entries.end(), files.begin(), files.end());
    return entries;
}

// Parameter overrides per preset, in gambatte.jsonc's shader_parameters.
static nlohmann::json ShaderParameterOverrides()
{
    const std::string text = OverlayConfig::GetConfigJson("shader_parameters");
    nlohmann::json j = text.empty() ? nlohmann::json::object()
                                    : nlohmann::json::parse(text, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

static void SaveShaderParameterOverrides(const nlohmann::json &j)
{
    OverlayConfig::SetConfigJson("shader_parameters", j.dump());
    OverlayConfig::SaveConfig();
}

// A preset just loaded: start from its defaults, then the saved overrides.
static void OnShaderLoaded()
{
    if (!g_chain)
        return;
    g_chain->ResetParameters();
    const nlohmann::json overrides = ShaderParameterOverrides();
    const auto it = overrides.find(ShaderPreset());
    if (it == overrides.end() || !it->is_object())
        return;
    for (const auto &param : it->items())
        if (param.value().is_number())
            g_chain->SetParameter(param.key(), param.value().get<float>());
}

static void SetShaderParameter(const std::string &id, float value)
{
    if (!g_chain)
        return;
    g_chain->SetParameter(id, value);
    nlohmann::json overrides = ShaderParameterOverrides();
    nlohmann::json &preset = overrides[ShaderPreset()];
    if (!preset.is_object())
        preset = nlohmann::json::object();
    for (const TicoSlang::Parameter &p : g_chain->Parameters())
    {
        if (p.id != id)
            continue;
        const float step = p.step > 0.0f ? p.step : 0.01f;
        if (std::fabs(value - p.initial) < step * 0.5f)
            preset.erase(id);
        else
            preset[id] = value;
    }
    if (preset.empty())
        overrides.erase(ShaderPreset());
    SaveShaderParameterOverrides(overrides);
}

static void ResetShaderParameters()
{
    if (g_chain)
        g_chain->ResetParameters();
    nlohmann::json overrides = ShaderParameterOverrides();
    overrides.erase(ShaderPreset());
    SaveShaderParameterOverrides(overrides);
}

static void RegisterShaderMenu()
{
    OverlayUI::ShaderCallbacks callbacks;
    callbacks.preset_label = [] { return ShaderPresetLabel(); };
    callbacks.browse_start = [] {
        const std::string preset = ShaderPreset();
        const size_t slash = preset.find_last_of('/');
        return slash == std::string::npos ? std::string(kUserShaderDir) : preset.substr(0, slash + 1);
    };
    callbacks.browse = [](const std::string &dir) { return BrowseShaders(dir); };
    callbacks.select = [](const std::string &path) { SetShaderPreset(path); };
    callbacks.parameters = [] {
        std::vector<OverlayUI::ShaderParameter> out;
        if (g_chain)
            for (const TicoSlang::Parameter &p : g_chain->Parameters())
                out.push_back({p.id, p.description, p.value, p.minimum, p.maximum, p.step});
        return out;
    };
    callbacks.set_parameter = [](const std::string &id, float value) { SetShaderParameter(id, value); };
    callbacks.reset_parameters = [] { ResetShaderParameters(); };
    OverlayUI::SetShaderCallbacks(std::move(callbacks));
}

// Loads the preset the settings name once it differs from the active one.
// Compiling can take a while on the Switch, so the frame before it shows a
// toast instead of the screen just freezing.
static void ApplyShaderPreset()
{
    const std::string wanted = ShaderPreset();
    if (!g_chain || wanted == g_activePreset)
        return;
    static std::string announced;
    if (announced != wanted && !wanted.empty())
    {
        announced = wanted;
        OverlayUI::ShowToast(SwitchFrontend::OverlayTranslation::tr("emulator_loading_shader"),
                             OverlayUI::ToastCorner::TopRight);
        return;
    }
    announced.clear();
    std::string error;
    if (g_chain->LoadPreset(wanted, error))
    {
        g_activePreset = wanted;
        OnShaderLoaded();
        return;
    }
    LOG_ERROR("SHADER", "Cannot load %s: %s", wanted.c_str(), error.c_str());
    const std::string firstLine = error.substr(0, error.find('\n'));
    OverlayUI::ShowToast(SwitchFrontend::OverlayTranslation::tr("emulator_shader_failed") + ": " +
                             firstLine.substr(0, 80),
                         OverlayUI::ToastCorner::TopRight);
    // Keep showing (and saving) what actually runs.
    if (g_activePreset == "\x01")
        g_activePreset.clear();
    SetShaderPreset(g_activePreset);
}

// settings.json is the one settings definition: every core option it lists
// reaches the core, with its default when the config file does not set it.
static void ApplySettingsToCore()
{
    if (!g_core)
        return;
    OverlayConfig::ApplyToCore([](const std::string &key, const std::string &value) {
        g_core->SetOption(key, value);
    });
}

static std::string TrFormat(const char *key, int value)
{
    const std::string format = SwitchFrontend::OverlayTranslation::tr(key);
    char text[256];
    snprintf(text, sizeof(text), format.c_str(), value);
    return text;
}

static void OpenMenu()
{
    if (!g_overlayReady || g_menuOpen)
        return;
    g_menuOpen = true;
    g_navHeldPrev = 0;
    g_navRepeatFrames = 0;
    ImGuiOverlay::SetVisible(true);
}

static void CloseMenu()
{
    if (!g_menuOpen)
        return;
    g_menuOpen = false;
    ImGuiOverlay::SetVisible(false);
    if (g_core)
        g_core->ClearInputs();
}

// D-pad + left stick, edge plus hold-repeat; Switch A accepts, B goes back.
static void FeedMenu(SDL_GameController *pad)
{
    enum : uint32_t { Up = 1, Down = 2, Left = 4, Right = 8 };
    const Sint16 axisX = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    const Sint16 axisY = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    uint32_t held = 0;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) || axisY < -16000) held |= Up;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || axisY > 16000) held |= Down;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || axisX < -16000) held |= Left;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || axisX > 16000) held |= Right;

    uint32_t fire = held & ~g_navHeldPrev; // new presses fire instantly
    if (held != 0 && held == g_navHeldPrev)
    {
        if (--g_navRepeatFrames <= 0)
        {
            fire |= held;
            g_navRepeatFrames = kNavRepeatFrames;
        }
    }
    else if (fire != 0)
    {
        g_navRepeatFrames = kNavInitialDelayFrames;
    }
    g_navHeldPrev = held;

    // SDL names buttons by position: B is the Switch A (east), A the Switch B.
    static bool acceptHeld = false;
    static bool cancelHeld = false;
    const bool accept = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B);
    const bool cancel = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A);
    ImGuiOverlay::FeedNav({
        .up = (fire & Up) != 0,
        .down = (fire & Down) != 0,
        .left = (fire & Left) != 0,
        .right = (fire & Right) != 0,
        .accept = accept && !acceptHeld,
        .cancel = cancel && !cancelHeld,
    });
    acceptHeld = accept;
    cancelHeld = cancel;
}

// Carries out what the menu chose on the last drawn frame.
static void RunMenuAction()
{
    using OverlayUI::Action;
    const Action action = ImGuiOverlay::ConsumeAction();
    if (OverlayUI::ConsumeSettingsChanged())
        ApplySettingsToCore();

    switch (action)
    {
    case Action::None:
        return;
    case Action::Resume:
        CloseMenu();
        return;
    case Action::Exit:
        LOG_INFO("HOME", "Exit requested");
        CloseMenu();
        ChainloadTico();
        g_running = false;
        return;
    case Action::Reset:
        if (g_core)
            g_core->Reset();
        CloseMenu();
        return;
    default:
        break;
    }

    if (OverlayUI::IsSaveStateAction(action) && g_core)
    {
        const int slot = OverlayUI::GetStateSlotForAction(action);
        g_core->SaveState(StatePath(slot - 1));
        OverlayUI::ShowToast(TrFormat("emulator_state_saved", slot));
        CloseMenu();
    }
    else if (OverlayUI::IsLoadStateAction(action) && g_core)
    {
        const int slot = OverlayUI::GetStateSlotForAction(action);
        g_core->LoadState(StatePath(slot - 1));
        OverlayUI::ShowToast(TrFormat("emulator_state_loaded", slot));
        CloseMenu();
    }
}

static void UpdateHud(float deltaTime)
{
    g_hudFrames++;
    g_hudSeconds += deltaTime;
    if (g_hudSeconds >= 0.5f)
    {
        g_hudFps = static_cast<float>(g_hudFrames) / g_hudSeconds;
        g_hudFrames = 0;
        g_hudSeconds = 0.0f;
    }
    OverlayUI::HudStats stats;
    stats.fps = g_hudFps;
    stats.fast_forward = g_audio.IsFastForwarding();
    if (g_core)
    {
        stats.rendered_width = g_core->GetFrameWidth();
        stats.rendered_height = g_core->GetFrameHeight();
    }
    OverlayUI::SetHudStats(stats);
}

// The game's on-screen rectangle, from the Display tab: Integer scales the
// frame by 1x, 2x or the largest that fits ("Auto"); Display fits an aspect
// ratio (4:3, 16:9, the core's own "Original") or stretches.
static ImVec4 ComputeGameRect(ImVec2 displaySize)
{
    const int width = g_core ? g_core->GetFrameWidth() : 160;
    const int height = g_core ? g_core->GetFrameHeight() : 144;
    const float aspectRatio = g_core ? g_core->GetAspectRatio() : 160.0f / 144.0f;
    const std::string mode = OverlayConfig::GetConfigValue("display_mode", "Integer");
    const std::string size = OverlayConfig::GetConfigValue("display_size", "Auto");

    const float baseW = width > 0 ? static_cast<float>(width) : 160.0f;
    const float baseH = height > 0 ? static_cast<float>(height) : 144.0f;
    float dstWidth = displaySize.x;
    float dstHeight = displaySize.y;
    if (mode == "Integer")
    {
        int scale;
        if (size == "1x")
            scale = 1;
        else if (size == "2x")
            scale = 2;
        else
            scale = std::max(1, std::min(static_cast<int>(displaySize.x / baseW),
                                         static_cast<int>(displaySize.y / baseH)));
        dstWidth = std::min(displaySize.x, baseW * scale);
        dstHeight = std::min(displaySize.y, baseH * scale);
    }
    else if (size != "Stretch")
    {
        float ar = aspectRatio > 0.0f ? aspectRatio : baseW / baseH;
        if (size == "4:3")
            ar = 4.0f / 3.0f;
        else if (size == "16:9")
            ar = 16.0f / 9.0f;
        if (ar > displaySize.x / displaySize.y)
        {
            dstWidth = displaySize.x;
            dstHeight = displaySize.x / ar;
        }
        else
        {
            dstHeight = displaySize.y;
            dstWidth = displaySize.y * ar;
        }
    }
    dstWidth = std::floor(dstWidth);
    dstHeight = std::floor(dstHeight);
    return ImVec4(std::floor((displaySize.x - dstWidth) / 2.0f),
                  std::floor((displaySize.y - dstHeight) / 2.0f), dstWidth, dstHeight);
}

// Runs the shader chain at the game's on-screen size and draws its output.
static void DrawGame(VkCommandBuffer cmd, ImDrawList *dl, ImVec2 displaySize)
{
    dl->AddRectFilled(ImVec2(0, 0), displaySize, IM_COL32(0, 0, 0, 255));
    const ImVec4 rect = ComputeGameRect(displaySize);
    if (!g_chain || !cmd || rect.z < 1.0f || rect.w < 1.0f)
        return;
    const float ar = g_core ? g_core->GetAspectRatio() : 160.0f / 144.0f;
    const ImTextureID tex = g_chain->Process(cmd, (uint32_t)rect.z, (uint32_t)rect.w, ar,
                                             g_core ? g_core->GetFPS() : 60.0);
    if (tex != ImTextureID_Invalid)
        dl->AddImage(tex, ImVec2(rect.x, rect.y), ImVec2(rect.x + rect.z, rect.y + rect.w));
}

void HandleInput()
{
    SDL_GameController *controllers[4] = {nullptr, nullptr, nullptr, nullptr};
    int numControllers = 0;

    if (g_controllersDirty)
    {
        RefreshControllers();
    }

    for (int i = 0; i < 4; ++i)
    {
        if (g_controllers[i])
            controllers[numControllers++] = g_controllers[i];
    }

    RunMenuAction();
    if (!g_running)
        return;

    SDL_GameController *pad = numControllers > 0 ? controllers[0] : nullptr;
    if (pad && g_overlayReady)
    {
        // Guide, or Plus+Minus, opens the menu and closes it again.
        const bool start = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START);
        const bool select = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK);
        const bool guide = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_GUIDE);
        const bool toggle = guide || (start && select);
        if (toggle && !g_toggleHeld)
        {
            if (g_menuOpen)
                CloseMenu();
            else
                OpenMenu();
        }
        g_toggleHeld = toggle;
        if (toggle && g_core)
        {
            g_core->ClearInputs();
            return;
        }
    }
    if (g_menuOpen)
    {
        if (pad)
            FeedMenu(pad);
        return;
    }

    if (g_core)
    {
        g_core->ClearInputs();

        for (int p = 0; p < numControllers; p++)
        {
            SDL_GameController *controller = controllers[p];
            if (!controller) continue;

            // Standard RetroPad mapping for Switch (SDL assumes Xbox layout)
            // Switch A (Right, SDL B) -> RetroPad A (Right)
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_A,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B));
            // Switch B (Bottom, SDL A) -> RetroPad B (Bottom)
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_B,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A));
            // Switch X (Top, SDL Y) -> RetroPad X (Top)
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_X,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_Y));
            // Switch Y (Left, SDL X) -> RetroPad Y (Left)
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_Y,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_X));

            // Switch + -> RetroPad Start
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_START,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START));
            // Switch - -> RetroPad Select
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_SELECT,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK));

            // Switch DPad + Left Stick -> RetroPad DPad
            int16_t leftX = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
            int16_t leftY = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);

            bool dpadUp = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP) || (leftY < -16000);
            bool dpadDown = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || (leftY > 16000);
            bool dpadLeft = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || (leftX < -16000);
            bool dpadRight = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || (leftX > 16000);

            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_UP, dpadUp);
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_DOWN, dpadDown);
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_LEFT, dpadLeft);
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_RIGHT, dpadRight);

            // Switch L -> RetroPad L
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_L,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
            // Switch R -> RetroPad R
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_R,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
            
            bool zl = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
            bool zr = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;

            if (p == 0)
            {
                g_audio.SetFastForward(zr);
            }

            // Switch ZL -> RetroPad L2
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_L2, zl);
            // Switch ZR -> RetroPad R2
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_R2, zr);

            // Left stick -> RetroPad Analog Left
            g_core->SetAnalogState(p, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X,
                                   SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX));
            g_core->SetAnalogState(p, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y,
                                   SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY));
            // Right stick -> RetroPad Analog Right
            g_core->SetAnalogState(p, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X,
                                   SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX));
            g_core->SetAnalogState(p, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y,
                                   SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY));
        }
    }
}

void Render()
{
    static int frameCount = 0;
    frameCount++;

    if (frameCount <= 3)
    {
        LOG_DEBUG("RENDER", "Frame %d: Render starting", frameCount);
    }

#ifdef __SWITCH__
    UpdateScreenMode();
#endif

    // Waits for this frame slot's previous submission, so everything below
    // may reuse per-frame resources. A skipped frame (swapchain being
    // recreated) still runs the core so emulation keeps its pace.
    VkCommandBuffer cmd = TicoVulkan::BeginFrame();

#ifdef __SWITCH__
    ImGuiIO &io = ImGui::GetIO();
    int logW, logH;
    GetDisplayResolution(logW, logH);
    io.DisplaySize = ImVec2((float)logW, (float)logH);
    io.DeltaTime = 1.0f / 60.0f;
#else
    ImGui_ImplSDL2_NewFrame();
    int logW, logH;
    GetDisplayResolution(logW, logH);
    ImGui::GetIO().DisplaySize = ImVec2((float)logW, (float)logH);
    ImGui::GetIO().DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
#endif
    ImGui::NewFrame();

    int w, h;
    GetDisplayResolution(w, h);
    ImVec2 displaySize((float)w, (float)h);

    if (g_core && !g_menuOpen)
    {
        if (frameCount <= 3)
            LOG_DEBUG("RENDER", "Frame %d: Calling RunFrame", frameCount);
        g_core->RunFrame();
    }

    ApplyShaderPreset();
    DrawGame(cmd, ImGui::GetBackgroundDrawList(), displaySize);
    UpdateHud(ImGui::GetIO().DeltaTime);
    ImGuiOverlay::Draw(g_core.get(), displaySize.x, displaySize.y, ImGui::GetIO().DeltaTime);

    if (g_core && g_core->GetOSDFrames() > 0)
    {
        ImDrawList *fg = ImGui::GetForegroundDrawList();
        const float marginX = 24.0f;
        const float marginY = 16.0f;
        const float padX = 16.0f;
        const float padY = 8.0f;
        const float rounding = 14.0f;
        
        int frames = g_core->GetOSDFrames();
        float alpha = 1.0f;
        if (frames < 30) alpha = frames / 30.0f;
        
        std::string msg = g_core->GetOSDMessage();
        ImVec2 textSize = ImGui::CalcTextSize(msg.c_str());
        
        float pillW = textSize.x + padX * 2;
        float pillH = textSize.y + padY * 2;
        float pillX = marginX;
        float pillY = marginY;
        
        ImU32 bgCol = IM_COL32(0, 0, 0, (int)(alpha * 153));
        fg->AddRectFilled(ImVec2(pillX, pillY), ImVec2(pillX + pillW, pillY + pillH), bgCol, rounding);
        
        ImU32 textCol = IM_COL32(255, 255, 255, (int)(alpha * 240));
        fg->AddText(ImVec2(pillX + padX, pillY + padY), textCol, msg.c_str());
        
        g_core->DecrementOSD();
    }

    ImGui::Render();
    if (cmd)
        TicoVulkan::EndFrame(ImGui::GetDrawData());
}

//==============================================================================
// Main
//==============================================================================

int main(int argc, char *argv[])
{
    Logger::Instance().ResetLogFile();

    g_running = true;
    g_controllersDirty = true;

#ifdef __SWITCH__
    LOG_INFO("HOME", "Calling appletLockExit...");
    appletLockExit();
    LOG_INFO("HOME", "Calling romfsInit...");
    Result romfsRc = romfsInit();
    if (R_FAILED(romfsRc))
    {
        LOG_WARN("HOME", "romfsInit failed: 0x%x", romfsRc);
    }
    else
    {
        LOG_INFO("HOME", "romfsInit succeeded");
    }

    LOG_INFO("HOME", "Calling nwindowSetDimensions...");
    nwindowSetDimensions(nwindowGetDefault(), 1920, 1080);
    LOG_INFO("HOME", "Switch pre-init complete (romfs, nwindow)");

    if (R_SUCCEEDED(socketInitializeDefault()))
    {
        LOG_INFO("HOME", "socketInitializeDefault succeeded");
    }
    else
    {
        LOG_ERROR("HOME", "socketInitializeDefault failed");
    }
#endif

    LOG_INFO("HOME", "gambatte starting (slug: %s)...", TicoConfig::CURRENT_SLUG.c_str());

    LOG_INFO("HOME", "Calling InitWindow...");
    if (!InitWindow())
    {
        LOG_ERROR("HOME", "Failed to initialize window");
        Logger::Instance().CloseLogFile();
        return 1;
    }
    LOG_INFO("HOME", "InitWindow succeeded");

#ifdef __SWITCH__
    ApplySwitchPerformanceProfile();
    PinCurrentThreadToCore(2, "main/render");
#endif

    LOG_INFO("HOME", "Calling InitImGui...");
    if (!InitImGui())
    {
        LOG_ERROR("HOME", "Failed to initialize ImGui");
        CleanupWindow();
        Logger::Instance().CloseLogFile();
        return 1;
    }
    LOG_INFO("HOME", "InitImGui succeeded");

    g_chain = std::make_unique<TicoShaderChain>();
    if (!g_chain->Init())
    {
        LOG_ERROR("HOME", "Shader chain initialization failed");
        g_chain.reset();
    }
#ifdef __SWITCH__
    g_lastOperationMode = 255;
#endif

    // Parse arguments: argv[1] = console slug, argv[2] = ROM path, argv[3] = title
    std::string slug = "gbc";
    std::string romPath = TicoConfig::TEST_ROM;
    std::string titleArg;

    if (argc >= 3) {
        slug = argv[1];
        romPath = argv[2];
        if (argc >= 4 && argv[3])
            titleArg = argv[3];
    } else if (argc == 2) {
        // Fallback: single arg is ROM path
        romPath = argv[1];
    }

    // The console picks the save and state folders, so it is set before the
    // core, which reads them when it is created.
    TicoConfig::SetSlug(slug);
    LOG_INFO("HOME", "Console slug: %s", slug.c_str());
    LOG_INFO("HOME", "ROM path: %s", romPath.c_str());
    LOG_INFO("HOME", "Configured paths for slug '%s': saves=%s states=%s system=%s",
             slug.c_str(), TicoConfig::SavesPath().c_str(), TicoConfig::StatesPath().c_str(),
             TicoConfig::SystemPath().c_str());
    TicoConfig::MakeDirs(TicoConfig::SavesPath());
    TicoConfig::MakeDirs(TicoConfig::StatesPath());
    TicoConfig::MakeDirs(TicoConfig::SystemPath());

    LOG_INFO("HOME", "Creating core...");
    g_core = std::make_unique<TicoCore>();
    g_core->EnsureConfigLoaded();
    OverlayConfig::ReloadConfig();
    ApplySettingsToCore();

    g_core->SetAudioCallbacks(AudioSampleCallback, AudioSampleBatchCallback, AudioFlushCallback);
    g_core->SetVideoCallback(VideoCallback);

    if (!g_audio.Init(g_audioDevice))
    {
        LOG_WARN("HOME", "TicoAudio init failed");
    }

    g_overlayReady = ImGuiOverlay::Init();
    OverlayUI::SetSlotOccupiedCallback([](int slot) {
        struct stat st;
        return slot >= 1 && stat(StatePath(slot - 1).c_str(), &st) == 0;
    });
    MigrateShaderSetting();
    RegisterShaderMenu();
    OverlayUI::ReloadSettings();
    LOG_INFO("HOME", "Core and overlay created");

    {
        size_t lastSlash = romPath.find_last_of("/\\");
        std::string filename = (lastSlash != std::string::npos) ? romPath.substr(lastSlash + 1) : romPath;

        // Prefer the launcher-supplied title; fall back to the rom filename.
        std::string cleanTitle = titleArg.empty() ? TicoUtils::GetCleanTitle(filename) : titleArg;
        if (cleanTitle.empty())
            cleanTitle = filename;

        OverlayUI::SetGameTitle(cleanTitle);
    }

    LOG_INFO("HOME", "Loading ROM: %s", romPath.c_str());
    if (!g_core->LoadGame(romPath))
    {
        LOG_ERROR("HOME", "Failed to load ROM: %s", romPath.c_str());
    }
    else
    {
        g_audio.SetCoreSampleRate(g_core->GetSampleRate());
        LOG_INFO("AUDIO", "Configured audio pipeline for %.0f Hz core output", g_core->GetSampleRate());
    }

    // Frame pacing is handled entirely by vsync (FIFO presentation). Audio is
    // non-blocking, so presentation is the only governor. While fast-forwarding
    // the swapchain switches to an uncapped present mode.
    bool lastFastForward = false;

    while (g_running)
    {
#ifdef __SWITCH__
        if (!appletMainLoop())
        {
            LOG_INFO("HOME", "appletMainLoop returned false, exiting main loop");
            g_running = false;
            break;
        }
#endif

        bool fastForward = g_audio.IsFastForwarding();
        if (fastForward != lastFastForward)
        {
            TicoVulkan::SetVsync(!fastForward);
            lastFastForward = fastForward;
        }

        ProcessEvents();
        HandleInput();
        Render();
    }

    LOG_INFO("HOME", "Starting cleanup...");
    TicoVulkan::WaitIdle();
    OverlayUI::SetSlotOccupiedCallback(nullptr);
    OverlayUI::SetShaderCallbacks({});
    ImGuiOverlay::Shutdown();
    g_core.reset();



    g_audio.Shutdown();
    if (!TicoConfig::USE_SDLQUEUEAUDIO)
    {
        Mix_CloseAudio();
    }

    CleanupWindow();

#ifdef __SWITCH__
    socketExit();
    romfsExit();
    appletUnlockExit();
#endif

    LOG_INFO("HOME", "Clean exit");
    Logger::Instance().CloseLogFile();

    exit(0);
}
