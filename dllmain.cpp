#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "offsets.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace {

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct Matrix4x4 {
    float m[4][4]{};
};

struct Bounds {
    float minX{};
    float minY{};
    float maxX{};
    float maxY{};
    bool valid{};
};

struct Vertex {
    float x{};
    float y{};
    float r{};
    float g{};
    float b{};
    float a{};
};

struct BulletTracer {
    Vec3 start{};
    Vec3 end{};
    ULONGLONG createdAt{};
    bool confirmed{};
};

struct BulletData {
    Vec3 position{};
    float timeStamp{};
    float expireTime{};
};
static_assert(sizeof(BulletData) == 20);

struct EventHash {
    std::uint32_t hash{};
    const char* name{};
};

struct GameEventListener {
    void** vtable{};
    int debugId{};
};

struct PurchaseEntry {
    std::uintptr_t controller{};
    std::string player;
    std::vector<std::string> items;
    int team{};
    ULONGLONG updatedAt{};
};

struct GameLogEntry {
    std::string title;
    std::string reason;
    ULONGLONG createdAt{};
    std::uint32_t color{};
};

struct GameLogListenerEntry {
    const char* name{};
    GameEventListener listener{};
    void* vtable[3]{};
    bool registered{};
};

struct PanoramaPanel {
    void* vtable{};
    void* panel{};
    char* panelName{};
};

struct PanoramaPanelData {
    std::uint8_t padding[8]{};
    std::uint32_t flags{};
    std::uint32_t visible{};
    PanoramaPanel* panel{};
    std::uint32_t panelId{};
    std::uint8_t tailPadding[4]{};
};

struct PanoramaUiEngine {
    void* vtable{};
    std::uint8_t padding[0x220]{};
    PanoramaPanelData* panels{};
    std::int32_t panelCount{};
};

struct ScoreboardWeapon {
    std::string name;
    int type{};
    bool operator==(const ScoreboardWeapon& other) const {
        return name == other.name && type == other.type;
    }
};

struct ScoreboardWeaponState {
    std::vector<ScoreboardWeapon> weapons;
    std::string activeName;
    int money{-1};
    bool operator==(const ScoreboardWeaponState& other) const {
        return weapons == other.weapons && activeName == other.activeName &&
               money == other.money;
    }
};

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using CreateInterfaceFn = void*(__cdecl*)(const char*, int*);
using EngineClientCmdFn = void(__fastcall*)(void*, int, const char*,
                                            std::uint64_t);
using GameEventGetFloatFn = float(__fastcall*)(void*, const char*, float);
using GameEventGetControllerFn = std::uintptr_t(__fastcall*)(void*,
                                                             const EventHash*);

constexpr wchar_t kClientName[] = L"client.dll";
constexpr wchar_t kMutexName[] = L"Local\\Cs2InternalPresent.Singleton";
constexpr std::size_t kRawChunkScanSize = 0x10000;
constexpr std::size_t kMaxVertices = 8192;

HANDLE g_log = INVALID_HANDLE_VALUE;
std::uintptr_t g_client = 0;
std::atomic_bool g_running{true};
std::atomic_bool g_unloadRequested{false};
std::mutex g_pawnMutex;
std::vector<std::uintptr_t> g_pawns;
int g_chunksRead = 0;
bool g_localFound = false;
std::atomic_bool g_menuOpen{false};
HWND g_gameWindow = nullptr;
WNDPROC g_originalWndProc = nullptr;
bool g_imguiInitialized = false;
std::wstring g_settingsPath;
void* g_inputSystem = nullptr;
std::uint8_t g_savedRelativeMouse = 1;
bool g_gameInputDisabled = false;

struct Settings {
    struct TriggerWeapon {
        bool enabled{true};
        int key{VK_XBUTTON1};
        int delay{5};
        bool headOnly{false};
    };

    bool enabled{true};
    bool box{true};
    bool healthBar{true};
    bool healthText{true};
    bool playerName{true};
    bool weaponName{true};
    bool teammates{false};
    bool boxOutline{true};
    float boxThickness{1.5f};
    float boxColor[4]{1.0f, 0.15f, 0.15f, 1.0f};
    float textColor[4]{1.0f, 1.0f, 1.0f, 1.0f};
    bool bulletTracers{false};
    bool tracerGlow{true};
    float tracerDuration{4.0f};
    float tracerThickness{2.0f};
    float tracerColor[4]{0.68f, 0.75f, 1.0f, 1.0f};
    bool purchasesOnTab{true};
    bool chatLogs{true};
    bool purchaseLogs{true};
    bool voteLogs{true};
    bool kickLogs{true};
    bool surrenderLogs{true};
    float logDuration{12.0f};
    bool triggerEnabled{false};
    std::array<TriggerWeapon, 6> trigger{};
};

Settings g_settings{};

PresentFn g_originalPresent = nullptr;
void* g_presentAddress = nullptr;
void* g_engineClient = nullptr;
EngineClientCmdFn g_engineClientCmd = nullptr;
bool g_attackHeld = false;
ULONGLONG g_attackReleaseAt = 0;
std::atomic_bool g_triggerKeyHeld{false};
std::atomic_bool g_triggerTargetFound{false};
std::atomic<int> g_crosshairEntity{-1};
std::atomic<std::uint64_t> g_triggerShots{0};
std::mutex g_tracerMutex;
std::vector<BulletTracer> g_bulletTracers;
std::atomic<std::uint64_t> g_bulletImpactCount{0};
std::atomic<ULONGLONG> g_lastLocalShotAt{0};
std::atomic_bool g_bulletServiceReady{false};
std::atomic<int> g_bulletServiceCount{0};
std::uintptr_t g_lastBulletService = 0;
std::uintptr_t g_lastBulletMemory = 0;
int g_lastBulletCount = 0;
std::uintptr_t g_gameEventManager = 0;
GameEventGetFloatFn g_gameEventGetFloat = nullptr;
GameEventGetControllerFn g_gameEventGetController = nullptr;
GameEventListener g_bulletImpactListener{};
void* g_bulletImpactVtable[3]{};
std::atomic_bool g_bulletImpactRegistered{false};
std::array<GameLogListenerEntry, 8> g_gameLogListeners{};
std::atomic_bool g_gameLogRegistered{false};
std::atomic<int> g_gameLogListenerCount{0};
std::mutex g_gameLogMutex;
std::vector<PurchaseEntry> g_roundPurchases;
std::vector<GameLogEntry> g_gameLogs;
std::string g_activeVoteTitle;
std::string g_activeVoteReason;
void* g_panoramaInterface = nullptr;
PanoramaUiEngine* g_panoramaUiEngine = nullptr;
PanoramaPanel* g_scoreboardPanel = nullptr;
bool g_scoreboardScriptInjected = false;
int g_scoreboardInitThrottle = 0;
int g_scoreboardUpdateThrottle = 0;
int g_scoreboardLastPanelCount = -1;
bool g_scoreboardTabWasDown = false;
bool g_scoreboardTriedForCurrentTab = false;
ULONGLONG g_scoreboardTabOpenedAt = 0;
std::uintptr_t g_scoreboardEntityList = 0;
std::unordered_map<std::uint64_t, ScoreboardWeaponState> g_scoreboardCache;
std::string g_scoreboardStatus{"not initialized"};
std::uintptr_t g_findHudElement = 0;
std::uintptr_t g_setVoiceData = 0;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11VertexShader* g_vertexShader = nullptr;
ID3D11PixelShader* g_pixelShader = nullptr;
ID3D11InputLayout* g_inputLayout = nullptr;
ID3D11Buffer* g_vertexBuffer = nullptr;
ID3D11BlendState* g_blendState = nullptr;
ID3D11DepthStencilState* g_depthState = nullptr;
ID3D11RasterizerState* g_rasterizerState = nullptr;

bool InitializeRenderer(IDXGISwapChain* swapChain);
void* FindModuleSignature(HMODULE module, const char* signature);
bool InitializeVelocityHudChat();
void UpdateVelocityScoreboard(std::uintptr_t entityList);
void ShutdownVelocityScoreboard();
void RenderVelocityScoreboardFallback(std::uintptr_t entityList, float width,
                                      float height);

bool InitializeInputSystem() {
    HMODULE module = GetModuleHandleW(L"inputsystem.dll");
    if (!module) return false;

    auto createInterface = reinterpret_cast<CreateInterfaceFn>(
        GetProcAddress(module, "CreateInterface"));
    if (!createInterface) return false;

    g_inputSystem = createInterface("InputSystemVersion001", nullptr);
    return g_inputSystem != nullptr;
}

void ReleaseMovementKeys() {
    if (!g_gameWindow || !g_originalWndProc) return;

    constexpr WPARAM keys[] = {
        'W', 'A', 'S', 'D', VK_SHIFT, VK_CONTROL, VK_SPACE,
        VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT
    };
    for (const WPARAM key : keys) {
        CallWindowProcW(g_originalWndProc, g_gameWindow, WM_KEYUP, key,
                        static_cast<LPARAM>(0xC0000001));
    }
    CallWindowProcW(g_originalWndProc, g_gameWindow, WM_LBUTTONUP, 0, 0);
    CallWindowProcW(g_originalWndProc, g_gameWindow, WM_RBUTTONUP, 0, 0);
}

void SetGameInputEnabled(bool enabled) {
    if (g_inputSystem) {
        void** table = *reinterpret_cast<void***>(g_inputSystem);
        if (table && table[76]) {
            using EnableInputFn = void(__fastcall*)(void*, bool);
            auto enableInput = reinterpret_cast<EnableInputFn>(table[76]);

            if (!enabled && !g_gameInputDisabled) {
                g_savedRelativeMouse = *reinterpret_cast<std::uint8_t*>(
                    reinterpret_cast<std::uintptr_t>(g_inputSystem) + 84);
            }
            enableInput(g_inputSystem,
                        enabled ? (g_savedRelativeMouse != 0) : false);
            g_gameInputDisabled = !enabled;
        }
    }

    if (!enabled) {
        ReleaseMovementKeys();
        ReleaseCapture();
        ClipCursor(nullptr);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
}

void SetMenuOpen(bool open) {
    const bool previous = g_menuOpen.exchange(open, std::memory_order_relaxed);
    if (previous == open) return;

    if (open) {
        ReleaseMovementKeys();
        SetGameInputEnabled(false);
    } else {
        SetGameInputEnabled(true);
    }
}

bool Readable(const MEMORY_BASIC_INFORMATION& info) {
    if (info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    constexpr DWORD mask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY;
    return (info.Protect & mask) != 0;
}

template <typename T>
T Read(std::uintptr_t address) {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    if (address < 0x10000 ||
        address > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) {
        return value;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0 ||
        !Readable(info)) {
        return value;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto end = start + info.RegionSize;
    if (address < start || address + sizeof(T) > end) return value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

void OpenLog() {
    wchar_t temp[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0 || length >= MAX_PATH) return;
    std::wstring path(temp, length);
    path += L"cs2_internal_esp.log";
    g_log = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Log(const std::wstring& text) {
    OutputDebugStringW((text + L"\n").c_str());
    if (g_log == INVALID_HANDLE_VALUE) return;
    const std::wstring line = text + L"\r\n";
    DWORD written = 0;
    WriteFile(g_log, line.data(),
              static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written, nullptr);
}

std::string ReadString(std::uintptr_t address, std::size_t maximum) {
    std::string result;
    if (address < 0x10000 || maximum == 0) return result;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info,
                     sizeof(info)) == 0 ||
        !Readable(info)) {
        return result;
    }
    const std::uintptr_t regionEnd =
        reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    const std::size_t readable = static_cast<std::size_t>(
        std::min<std::uintptr_t>(maximum, regionEnd - address));
    result.reserve(readable);
    const char* text = reinterpret_cast<const char*>(address);
    for (std::size_t i = 0; i < readable; ++i) {
        const char character = text[i];
        if (character == '\0') break;
        if (static_cast<unsigned char>(character) < 0x20) continue;
        result.push_back(character);
    }
    return result;
}

void WriteSetting(const wchar_t* key, bool value) {
    WritePrivateProfileStringW(L"esp", key, value ? L"1" : L"0",
                               g_settingsPath.c_str());
}

void WriteSetting(const wchar_t* key, float value) {
    const std::wstring text = std::to_wstring(value);
    WritePrivateProfileStringW(L"esp", key, text.c_str(),
                               g_settingsPath.c_str());
}

void WriteSetting(const wchar_t* key, int value) {
    const std::wstring text = std::to_wstring(value);
    WritePrivateProfileStringW(L"esp", key, text.c_str(),
                               g_settingsPath.c_str());
}

float ReadFloatSetting(const wchar_t* key, float fallback) {
    wchar_t value[64]{};
    const std::wstring fallbackText = std::to_wstring(fallback);
    GetPrivateProfileStringW(L"esp", key, fallbackText.c_str(), value,
                             static_cast<DWORD>(_countof(value)),
                             g_settingsPath.c_str());
    wchar_t* end = nullptr;
    const float parsed = std::wcstof(value, &end);
    return end == value ? fallback : parsed;
}

int ReadIntSetting(const wchar_t* key, int fallback) {
    return GetPrivateProfileIntW(L"esp", key, fallback,
                                 g_settingsPath.c_str());
}

void SaveSettings() {
    if (g_settingsPath.empty()) return;
    WriteSetting(L"enabled", g_settings.enabled);
    WriteSetting(L"box", g_settings.box);
    WriteSetting(L"health_bar", g_settings.healthBar);
    WriteSetting(L"health_text", g_settings.healthText);
    WriteSetting(L"player_name", g_settings.playerName);
    WriteSetting(L"weapon_name", g_settings.weaponName);
    WriteSetting(L"teammates", g_settings.teammates);
    WriteSetting(L"box_outline", g_settings.boxOutline);
    WriteSetting(L"box_thickness", g_settings.boxThickness);
    for (int i = 0; i < 4; ++i) {
        WriteSetting((L"box_color_" + std::to_wstring(i)).c_str(),
                     g_settings.boxColor[i]);
        WriteSetting((L"text_color_" + std::to_wstring(i)).c_str(),
                     g_settings.textColor[i]);
        WriteSetting((L"tracer_color_" + std::to_wstring(i)).c_str(),
                     g_settings.tracerColor[i]);
    }
    WriteSetting(L"bullet_tracers", g_settings.bulletTracers);
    WriteSetting(L"tracer_glow", g_settings.tracerGlow);
    WriteSetting(L"tracer_duration", g_settings.tracerDuration);
    WriteSetting(L"tracer_thickness", g_settings.tracerThickness);
    WriteSetting(L"purchases_on_tab", g_settings.purchasesOnTab);
    WriteSetting(L"chat_logs", g_settings.chatLogs);
    WriteSetting(L"purchase_logs", g_settings.purchaseLogs);
    WriteSetting(L"vote_logs", g_settings.voteLogs);
    WriteSetting(L"kick_logs", g_settings.kickLogs);
    WriteSetting(L"surrender_logs", g_settings.surrenderLogs);
    WriteSetting(L"log_duration", g_settings.logDuration);
    WriteSetting(L"trigger_enabled", g_settings.triggerEnabled);
    for (std::size_t i = 0; i < g_settings.trigger.size(); ++i) {
        const std::wstring prefix = L"trigger_" + std::to_wstring(i) + L"_";
        const auto key = [&](const wchar_t* name) { return prefix + name; };
        const Settings::TriggerWeapon& cfg = g_settings.trigger[i];
        WriteSetting(key(L"enabled").c_str(), cfg.enabled);
        WriteSetting(key(L"key").c_str(), cfg.key);
        WriteSetting(key(L"delay").c_str(), cfg.delay);
        WriteSetting(key(L"head_only").c_str(), cfg.headOnly);
    }
}

void LoadSettings() {
    wchar_t temp[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0 || length >= MAX_PATH) return;
    g_settingsPath.assign(temp, length);
    g_settingsPath += L"cs2_velocity_esp.ini";
    const auto readBool = [](const wchar_t* key, bool fallback) {
        return GetPrivateProfileIntW(L"esp", key, fallback ? 1 : 0,
                                     g_settingsPath.c_str()) != 0;
    };
    g_settings.enabled = readBool(L"enabled", g_settings.enabled);
    g_settings.box = readBool(L"box", g_settings.box);
    g_settings.healthBar = readBool(L"health_bar", g_settings.healthBar);
    g_settings.healthText = readBool(L"health_text", g_settings.healthText);
    g_settings.playerName = readBool(L"player_name", g_settings.playerName);
    g_settings.weaponName = readBool(L"weapon_name", g_settings.weaponName);
    g_settings.teammates = readBool(L"teammates", g_settings.teammates);
    g_settings.boxOutline = readBool(L"box_outline", g_settings.boxOutline);
    g_settings.boxThickness =
        ReadFloatSetting(L"box_thickness", g_settings.boxThickness);
    for (int i = 0; i < 4; ++i) {
        g_settings.boxColor[i] = ReadFloatSetting(
            (L"box_color_" + std::to_wstring(i)).c_str(),
            g_settings.boxColor[i]);
        g_settings.textColor[i] = ReadFloatSetting(
            (L"text_color_" + std::to_wstring(i)).c_str(),
            g_settings.textColor[i]);
        g_settings.tracerColor[i] = ReadFloatSetting(
            (L"tracer_color_" + std::to_wstring(i)).c_str(),
            g_settings.tracerColor[i]);
    }
    g_settings.bulletTracers =
        readBool(L"bullet_tracers", g_settings.bulletTracers);
    g_settings.tracerGlow =
        readBool(L"tracer_glow", g_settings.tracerGlow);
    g_settings.tracerDuration = std::clamp(
        ReadFloatSetting(L"tracer_duration", g_settings.tracerDuration),
        0.1f, 10.0f);
    const bool tracerWorldV2Migrated =
        GetPrivateProfileIntW(L"esp", L"tracer_world_v2", 0,
                              g_settingsPath.c_str()) != 0;
    if (!tracerWorldV2Migrated) {
        // Older builds saved the short 0.5 second default, which made a world
        // tracer disappear before the player could walk away from the shot.
        if (g_settings.tracerDuration <= 0.55f) {
            g_settings.tracerDuration = 4.0f;
            WriteSetting(L"tracer_duration", g_settings.tracerDuration);
        }
        WritePrivateProfileStringW(L"esp", L"tracer_world_v2", L"1",
                                   g_settingsPath.c_str());
    }
    g_settings.tracerThickness = std::clamp(
        ReadFloatSetting(L"tracer_thickness", g_settings.tracerThickness),
        1.0f, 6.0f);
    g_settings.purchasesOnTab =
        readBool(L"purchases_on_tab", g_settings.purchasesOnTab);
    g_settings.chatLogs = readBool(L"chat_logs", g_settings.chatLogs);
    g_settings.purchaseLogs =
        readBool(L"purchase_logs", g_settings.purchaseLogs);
    g_settings.voteLogs = readBool(L"vote_logs", g_settings.voteLogs);
    g_settings.kickLogs = readBool(L"kick_logs", g_settings.kickLogs);
    g_settings.surrenderLogs =
        readBool(L"surrender_logs", g_settings.surrenderLogs);
    g_settings.logDuration = std::clamp(
        ReadFloatSetting(L"log_duration", g_settings.logDuration),
        3.0f, 30.0f);
    g_settings.triggerEnabled =
        readBool(L"trigger_enabled", g_settings.triggerEnabled);
    for (std::size_t i = 0; i < g_settings.trigger.size(); ++i) {
        const std::wstring prefix = L"trigger_" + std::to_wstring(i) + L"_";
        const auto key = [&](const wchar_t* name) { return prefix + name; };
        Settings::TriggerWeapon& cfg = g_settings.trigger[i];
        cfg.enabled = readBool(key(L"enabled").c_str(), cfg.enabled);
        cfg.key = ReadIntSetting(key(L"key").c_str(), cfg.key);
        cfg.delay = std::clamp(
            ReadIntSetting(key(L"delay").c_str(), cfg.delay), 0, 250);
        cfg.headOnly = readBool(key(L"head_only").c_str(), cfg.headOnly);
    }
}

bool ValidPosition(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) &&
           (std::abs(value.x) > 0.01f || std::abs(value.y) > 0.01f ||
            std::abs(value.z) > 0.01f);
}

bool WorldToScreen(const Vec3& world, Vec3& screen, const Matrix4x4& matrix,
                   float width, float height) {
    const float w = world.x * matrix.m[3][0] + world.y * matrix.m[3][1] +
                    world.z * matrix.m[3][2] + matrix.m[3][3];
    if (!std::isfinite(w) || w < 0.01f) return false;
    const float x = world.x * matrix.m[0][0] + world.y * matrix.m[0][1] +
                    world.z * matrix.m[0][2] + matrix.m[0][3];
    const float y = world.x * matrix.m[1][0] + world.y * matrix.m[1][1] +
                    world.z * matrix.m[1][2] + matrix.m[1][3];
    screen.x = width * 0.5f * (1.0f + x / w);
    screen.y = height * 0.5f * (1.0f - y / w);
    return std::isfinite(screen.x) && std::isfinite(screen.y);
}

float ProjectionW(const Vec3& world, const Matrix4x4& matrix) {
    return world.x * matrix.m[3][0] + world.y * matrix.m[3][1] +
           world.z * matrix.m[3][2] + matrix.m[3][3];
}

bool WorldSegmentToScreen(Vec3 start, Vec3 end, Vec3& startScreen,
                          Vec3& endScreen, const Matrix4x4& matrix,
                          float width, float height) {
    // Keep the clipped endpoint a little in front of WorldToScreen's 0.01
    // rejection threshold so floating-point rounding cannot hide the segment.
    constexpr float nearW = 0.02f;
    float startW = ProjectionW(start, matrix);
    float endW = ProjectionW(end, matrix);
    if (!std::isfinite(startW) || !std::isfinite(endW) ||
        (startW < nearW && endW < nearW)) {
        return false;
    }

    // Clip the world-space segment against the camera plane.  This keeps an
    // old shot visible after the player walks past its original head position
    // instead of replacing the hidden endpoint with the screen centre.
    if (startW < nearW) {
        const float denominator = endW - startW;
        if (std::abs(denominator) < 0.000001f) return false;
        const float t = std::clamp((nearW - startW) / denominator, 0.0f, 1.0f);
        start = {start.x + (end.x - start.x) * t,
                 start.y + (end.y - start.y) * t,
                 start.z + (end.z - start.z) * t};
    } else if (endW < nearW) {
        const float denominator = startW - endW;
        if (std::abs(denominator) < 0.000001f) return false;
        const float t = std::clamp((nearW - endW) / denominator, 0.0f, 1.0f);
        end = {end.x + (start.x - end.x) * t,
               end.y + (start.y - end.y) * t,
               end.z + (start.z - end.z) * t};
    }

    return WorldToScreen(start, startScreen, matrix, width, height) &&
           WorldToScreen(end, endScreen, matrix, width, height);
}

std::uintptr_t Chunk(std::uintptr_t entityList, std::uint32_t chunkIndex) {
    return Read<std::uintptr_t>(entityList + offsets::entityChunkTable +
                                sizeof(std::uintptr_t) * chunkIndex);
}

std::uintptr_t EntityFromHandle(std::uintptr_t entityList, std::uint32_t handle) {
    if (handle == 0 || handle == std::numeric_limits<std::uint32_t>::max()) return 0;
    const std::uint32_t index = handle & offsets::entityIndexMask;
    if (index == 0) return 0;
    const std::uintptr_t chunk = Chunk(entityList, index >> offsets::entityChunkShift);
    return Read<std::uintptr_t>(
        chunk + offsets::entityIdentityStride * (index & offsets::entityChunkMask));
}

Vec3 LocalEyePosition(std::uintptr_t pawn) {
    if (!pawn) return {};
    const std::uintptr_t sceneNode =
        Read<std::uintptr_t>(pawn + offsets::m_pGameSceneNode);
    const Vec3 origin = Read<Vec3>(sceneNode + offsets::m_vecOrigin);
    const Vec3 viewOffset = Read<Vec3>(pawn + offsets::m_vecViewOffset);
    return {origin.x + viewOffset.x, origin.y + viewOffset.y,
            origin.z + viewOffset.z};
}

void StoreBulletTracer(const Vec3& start, const Vec3& end, bool confirmed) {
    if (!ValidPosition(start) || !ValidPosition(end)) return;
    const ULONGLONG now = GetTickCount64();
    const std::lock_guard<std::mutex> lock(g_tracerMutex);

    if (confirmed) {
        // Replace the prediction created when m_iShotsFired changed.  This
        // keeps the first frame responsive without drawing a duplicate once
        // BulletServices publishes the real impact point.
        for (auto it = g_bulletTracers.rbegin(); it != g_bulletTracers.rend();
             ++it) {
            if (!it->confirmed && now >= it->createdAt &&
                now - it->createdAt <= 350) {
                it->start = start;
                it->end = end;
                it->confirmed = true;
                return;
            }
        }
    }

    if (g_bulletTracers.size() >= 64) {
        g_bulletTracers.erase(g_bulletTracers.begin());
    }
    g_bulletTracers.push_back(BulletTracer{start, end, now, confirmed});
}

void PollBulletServiceImpacts(std::uintptr_t pawn) {
    if (!pawn) {
        g_bulletServiceReady.store(false, std::memory_order_relaxed);
        g_lastBulletService = 0;
        g_lastBulletMemory = 0;
        g_lastBulletCount = 0;
        return;
    }

    const std::uintptr_t service =
        Read<std::uintptr_t>(pawn + offsets::m_pBulletServices);
    const int count = Read<int>(service + offsets::m_bulletData);
    const std::uintptr_t memory =
        Read<std::uintptr_t>(service + offsets::m_bulletData + 0x8);
    const int capacity =
        Read<int>(service + offsets::m_bulletData + 0x10);
    const bool valid = service && count >= 0 && count <= 512 &&
                       capacity >= count && capacity <= 4096 &&
                       (count == 0 || memory != 0);
    if (!valid) {
        g_bulletServiceReady.store(false, std::memory_order_relaxed);
        g_bulletServiceCount.store(count, std::memory_order_relaxed);
        return;
    }

    g_bulletServiceReady.store(true, std::memory_order_relaxed);
    g_bulletServiceCount.store(count, std::memory_order_relaxed);
    if (service != g_lastBulletService) {
        g_lastBulletService = service;
        g_lastBulletMemory = memory;
        g_lastBulletCount = count;
        return;
    }
    // CUtlVector may allocate its backing storage on the first shot or move
    // it while growing.  Existing elements are copied, so preserve the old
    // count and consume only the newly appended impacts.
    if (memory != g_lastBulletMemory) {
        g_lastBulletMemory = memory;
    }
    if (count < g_lastBulletCount) {
        g_lastBulletCount = 0;
    }
    if (!g_settings.bulletTracers) {
        g_lastBulletCount = count;
        return;
    }
    if (count <= g_lastBulletCount || !memory) return;

    const Vec3 eye = LocalEyePosition(pawn);
    Vec3 finalImpact{};
    float farthestSquared = -1.0f;
    int accepted = 0;
    const int first = std::clamp(g_lastBulletCount, 0, count);
    for (int index = first; index < count; ++index) {
        const BulletData bullet = Read<BulletData>(
            memory + static_cast<std::uintptr_t>(index) * sizeof(BulletData));
        const Vec3& impact = bullet.position;
        if (!ValidPosition(impact) || std::abs(impact.x) > 50000.0f ||
            std::abs(impact.y) > 50000.0f || std::abs(impact.z) > 50000.0f) {
            continue;
        }
        const float dx = impact.x - eye.x;
        const float dy = impact.y - eye.y;
        const float dz = impact.z - eye.z;
        const float distanceSquared = dx * dx + dy * dy + dz * dz;
        if (!std::isfinite(distanceSquared) || distanceSquared < 16.0f) continue;
        ++accepted;
        if (distanceSquared > farthestSquared) {
            farthestSquared = distanceSquared;
            finalImpact = impact;
        }
    }
    g_lastBulletCount = count;
    if (accepted > 0 && ValidPosition(eye)) {
        StoreBulletTracer(eye, finalImpact, true);
        g_bulletImpactCount.fetch_add(static_cast<std::uint64_t>(accepted),
                                     std::memory_order_relaxed);
    }
}

float ReadGameEventFloat(void* event, const char* key, float fallback) {
    if (g_gameEventGetFloat) return g_gameEventGetFloat(event, key, fallback);
    const void** table = Read<const void**>(reinterpret_cast<std::uintptr_t>(event));
    const std::uintptr_t function = Read<std::uintptr_t>(
        reinterpret_cast<std::uintptr_t>(table) + sizeof(void*) * 8);
    if (!function) return fallback;
    using GetFloatFn = float(__fastcall*)(void*, const char*, float);
    return reinterpret_cast<GetFloatFn>(function)(event, key, fallback);
}

const char* ReadGameEventName(void* event) {
    const void** table = Read<const void**>(reinterpret_cast<std::uintptr_t>(event));
    if (!table) return "";
    const std::uintptr_t function = Read<std::uintptr_t>(
        reinterpret_cast<std::uintptr_t>(table) + sizeof(void*) * 1);
    if (!function) return "";
    using GetNameFn = const char*(__fastcall*)(void*);
    const char* value = reinterpret_cast<GetNameFn>(function)(event);
    return value ? value : "";
}

int ReadGameEventInt(void* event, const char* key, int fallback = 0) {
    const void** table = Read<const void**>(reinterpret_cast<std::uintptr_t>(event));
    if (!table) return fallback;
    const std::uintptr_t function = Read<std::uintptr_t>(
        reinterpret_cast<std::uintptr_t>(table) + sizeof(void*) * 6);
    if (!function) return fallback;
    using GetIntFn = int(__fastcall*)(void*, const char*, int);
    return reinterpret_cast<GetIntFn>(function)(event, key, fallback);
}

std::string ReadGameEventString(void* event, const char* key,
                                const char* fallback = "") {
    const void** table = Read<const void**>(reinterpret_cast<std::uintptr_t>(event));
    if (!table) return fallback;
    const std::uintptr_t function = Read<std::uintptr_t>(
        reinterpret_cast<std::uintptr_t>(table) + sizeof(void*) * 9);
    if (!function) return fallback;
    using GetStringFn = const char*(__fastcall*)(void*, const char*, const char*);
    const char* value =
        reinterpret_cast<GetStringFn>(function)(event, key, fallback);
    if (!value) return fallback;
    const std::string result = ReadString(reinterpret_cast<std::uintptr_t>(value), 256);
    return result.empty() ? std::string(fallback) : result;
}

std::string LowerText(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

std::string FriendlyToken(std::string value) {
    while (!value.empty() && (value.front() == '#' || value.front() == '$')) {
        value.erase(value.begin());
    }
    constexpr const char* prefixes[] = {
        "SFUI_Vote_", "SFUI_vote_", "SFUI_", "GameUI_"
    };
    for (const char* prefix : prefixes) {
        if (value.rfind(prefix, 0) == 0) {
            value.erase(0, std::strlen(prefix));
            break;
        }
    }
    std::replace(value.begin(), value.end(), '_', ' ');
    return value.empty() ? "unknown" : value;
}

std::string FriendlyWeapon(std::string weapon) {
    if (weapon.rfind("weapon_", 0) == 0) weapon.erase(0, 7);
    if (weapon.rfind("item_", 0) == 0) weapon.erase(0, 5);
    if (weapon == "ak47") return "AK-47";
    if (weapon == "m4a1_silencer") return "M4A1-S";
    if (weapon == "m4a1") return "M4A4";
    if (weapon == "hkp2000") return "P2000";
    if (weapon == "usp_silencer") return "USP-S";
    if (weapon == "ssg08") return "SSG 08";
    if (weapon == "flashbang") return "Flashbang";
    if (weapon == "hegrenade") return "HE grenade";
    if (weapon == "smokegrenade") return "Smoke";
    if (weapon == "molotov") return "Molotov";
    if (weapon == "incgrenade") return "Incendiary";
    if (weapon == "defuser") return "Defuse kit";
    if (weapon == "vest") return "Kevlar";
    if (weapon == "vesthelm") return "Kevlar + helmet";
    std::replace(weapon.begin(), weapon.end(), '_', ' ');
    if (!weapon.empty()) weapon.front() = static_cast<char>(
        std::toupper(static_cast<unsigned char>(weapon.front())));
    return weapon.empty() ? "unknown item" : weapon;
}

std::uintptr_t EventController(void* event, const char* key) {
    if (g_gameEventGetController) {
        const EventHash eventKey{0, key};
        const std::uintptr_t controller =
            g_gameEventGetController(event, &eventKey);
        if (controller) return controller;
    }
    const int raw = ReadGameEventInt(event, key, 0);
    const std::uintptr_t entityList =
        Read<std::uintptr_t>(g_client + offsets::dwEntityList);
    return raw > 0 && entityList
               ? EntityFromHandle(entityList, static_cast<std::uint32_t>(raw))
               : 0;
}

std::string ControllerName(std::uintptr_t controller, int fallbackIndex = 0) {
    if (controller) {
        const std::uintptr_t namePointer = Read<std::uintptr_t>(
            controller + offsets::m_sSanitizedPlayerName);
        const std::string name = ReadString(namePointer, 128);
        if (!name.empty()) return name;
    }
    return fallbackIndex > 0 ? "player #" + std::to_string(fallbackIndex)
                             : "unknown player";
}

std::string EscapeHudHtml(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            default: escaped.push_back(character); break;
        }
    }
    return escaped;
}

std::string VelocityGradientLabel() {
    constexpr char label[] = "[velocity]";
    constexpr int startR = 130;
    constexpr int startG = 160;
    constexpr int startB = 240;
    constexpr int endR = 200;
    constexpr int endG = 220;
    constexpr int endB = 255;
    std::string result;
    char tag[64]{};
    for (std::size_t index = 0; index < sizeof(label) - 1; ++index) {
        const float amount = static_cast<float>(index) /
                             static_cast<float>(sizeof(label) - 2);
        const int red = static_cast<int>(startR + (endR - startR) * amount);
        const int green = static_cast<int>(startG + (endG - startG) * amount);
        const int blue = static_cast<int>(startB + (endB - startB) * amount);
        std::snprintf(tag, sizeof(tag),
                      "<font color='#%02X%02X%02X'>%c</font>",
                      red, green, blue, label[index]);
        result += tag;
    }
    return result;
}

void VelocityChatPrint(const std::string& title, const std::string& reason) {
    if (!g_findHudElement || !g_setVoiceData) return;
    using FindHudElementFn = std::uintptr_t(__fastcall*)(const char*);
    using SetVoiceDataFn = void(__fastcall*)(std::uintptr_t, const char*,
                                             std::uint32_t, std::uint8_t*);
    const std::uintptr_t hud = reinterpret_cast<FindHudElementFn>(
        g_findHudElement)("CCSGO_HudVoiceStatus");
    if (hud < 32) return;

    const std::string message =
        VelocityGradientLabel() +
        " <font color='#CCCCCC'>- " + EscapeHudHtml(title) +
        " | </font><font color='#FFFFFF'>" + EscapeHudHtml(reason) +
        "</font>";
    std::uint8_t flags[2]{1, 0};
    reinterpret_cast<SetVoiceDataFn>(g_setVoiceData)(
        hud - 32, message.c_str(), 0xFFFFFFFFu, flags);
}

void AddGameLog(std::string title, std::string reason, std::uint32_t color) {
    VelocityChatPrint(title, reason);
    const std::lock_guard<std::mutex> lock(g_gameLogMutex);
    g_gameLogs.push_back(
        {std::move(title), std::move(reason), GetTickCount64(), color});
    if (g_gameLogs.size() > 64) {
        g_gameLogs.erase(g_gameLogs.begin(),
                         g_gameLogs.begin() + (g_gameLogs.size() - 64));
    }
}

void RegisterPurchase(std::uintptr_t controller, const std::string& player,
                      const std::string& item, int team) {
    const std::lock_guard<std::mutex> lock(g_gameLogMutex);
    auto entry = std::find_if(
        g_roundPurchases.begin(), g_roundPurchases.end(),
        [&](const PurchaseEntry& candidate) {
            return controller ? candidate.controller == controller
                              : candidate.player == player;
        });
    if (entry == g_roundPurchases.end()) {
        g_roundPurchases.push_back({controller, player, {}, team, GetTickCount64()});
        entry = std::prev(g_roundPurchases.end());
    }
    entry->player = player;
    entry->team = team;
    entry->updatedAt = GetTickCount64();
    entry->items.push_back(item);
    if (entry->items.size() > 16) entry->items.erase(entry->items.begin());
}

bool IsSurrenderText(const std::string& text) {
    const std::string lower = LowerText(text);
    return lower.find("surrender") != std::string::npos ||
           lower.find("concede") != std::string::npos;
}

bool IsKickText(const std::string& text) {
    return LowerText(text).find("kick") != std::string::npos;
}

bool IsKickDisconnectReason(int reason) {
    return reason == 39 || reason == 41 || (reason >= 150 && reason <= 164);
}

std::string KickDisconnectReason(int reason) {
    switch (reason) {
        case 39: return "kicked by server";
        case 41: return "kicked and banned";
        case 150: return "team killing";
        case 152: return "untrusted account";
        case 153: return "convicted account";
        case 154: return "competitive cooldown";
        case 155: return "team damage";
        case 156: return "hostage killing";
        case 157: return "kicked by vote";
        case 158: return "kicked for inactivity";
        case 159: return "kicked for suicide";
        case 162: return "input automation";
        case 163: return "abnormal behavior";
        case 164: return "insecure client";
        default: return "disconnect reason " + std::to_string(reason);
    }
}

void* __fastcall OnGameLogEvent(void* self, void* event) {
    if (!event) return nullptr;
    const auto* listener = reinterpret_cast<GameEventListener*>(self);
    constexpr const char* eventNames[] = {
        "item_purchase", "round_start", "vote_started", "vote_cast",
        "vote_passed", "vote_failed", "player_disconnect", "round_end"
    };
    if (!listener || listener->debugId < 1 ||
        listener->debugId > static_cast<int>(std::size(eventNames))) {
        return nullptr;
    }
    const std::string eventName = eventNames[listener->debugId - 1];

    if (eventName == "round_start") {
        const std::lock_guard<std::mutex> lock(g_gameLogMutex);
        g_roundPurchases.clear();
        g_activeVoteTitle.clear();
        g_activeVoteReason.clear();
        return nullptr;
    }

    if (eventName == "item_purchase") {
        const int rawUser = ReadGameEventInt(event, "userid", 0);
        const std::uintptr_t controller = EventController(event, "userid");
        const std::string player = ControllerName(controller, rawUser);
        const std::string item = FriendlyWeapon(
            ReadGameEventString(event, "weapon", "unknown item"));
        RegisterPurchase(controller, player, item,
                         ReadGameEventInt(event, "team", 0));
        if (g_settings.chatLogs && g_settings.purchaseLogs) {
            AddGameLog("BUY: " + player, item, IM_COL32(112, 188, 255, 255));
        }
        return nullptr;
    }

    if (eventName == "vote_started") {
        const std::string issue = ReadGameEventString(event, "issue", "vote");
        const std::string parameter = ReadGameEventString(event, "param1", "");
        const bool surrender = IsSurrenderText(issue) || IsSurrenderText(parameter);
        const bool kick = IsKickText(issue);
        const std::string title = surrender ? "SURRENDER VOTE"
                                  : kick ? "KICK VOTE" : "VOTE";
        const std::string reason = parameter.empty() ? FriendlyToken(issue)
                                                     : FriendlyToken(parameter);
        {
            const std::lock_guard<std::mutex> lock(g_gameLogMutex);
            g_activeVoteTitle = title;
            g_activeVoteReason = reason;
        }
        if (g_settings.chatLogs && g_settings.voteLogs &&
            (!surrender || g_settings.surrenderLogs)) {
            AddGameLog(title, reason, IM_COL32(255, 195, 94, 255));
        }
        return nullptr;
    }

    if (eventName == "vote_cast") {
        const int rawUser = ReadGameEventInt(event, "userid", 0);
        const std::uintptr_t controller = EventController(event, "userid");
        const std::string player = ControllerName(controller, rawUser);
        const int option = ReadGameEventInt(event, "vote_option", -1);
        const std::string choice = option == 0 ? "YES"
                                   : option == 1 ? "NO"
                                                 : "option " + std::to_string(option + 1);
        if (g_settings.chatLogs && g_settings.voteLogs) {
            AddGameLog("VOTE: " + player, choice,
                       option == 0 ? IM_COL32(104, 224, 145, 255)
                                   : IM_COL32(255, 122, 122, 255));
        }
        return nullptr;
    }

    if (eventName == "vote_passed" || eventName == "vote_failed") {
        std::string title;
        std::string reason;
        {
            const std::lock_guard<std::mutex> lock(g_gameLogMutex);
            title = g_activeVoteTitle.empty() ? "VOTE" : g_activeVoteTitle;
            reason = g_activeVoteReason;
            g_activeVoteTitle.clear();
            g_activeVoteReason.clear();
        }
        const std::string details = ReadGameEventString(event, "details", "");
        const std::string parameter = ReadGameEventString(event, "param1", "");
        if (!parameter.empty()) reason = FriendlyToken(parameter);
        else if (!details.empty()) reason = FriendlyToken(details);
        if (reason.empty()) reason = "no details";
        const bool surrender = title.find("SURRENDER") != std::string::npos ||
                               IsSurrenderText(details);
        title += eventName == "vote_passed" ? " PASSED" : " FAILED";
        if (g_settings.chatLogs && g_settings.voteLogs &&
            (!surrender || g_settings.surrenderLogs)) {
            AddGameLog(title, reason,
                       eventName == "vote_passed"
                           ? IM_COL32(104, 224, 145, 255)
                           : IM_COL32(255, 104, 112, 255));
        }
        return nullptr;
    }

    if (eventName == "player_disconnect") {
        const int reasonCode = ReadGameEventInt(event, "reason", 0);
        if (g_settings.chatLogs && g_settings.kickLogs &&
            IsKickDisconnectReason(reasonCode)) {
            const std::string player =
                ReadGameEventString(event, "name", "unknown player");
            AddGameLog("KICK: " + player, KickDisconnectReason(reasonCode),
                       IM_COL32(255, 112, 112, 255));
        }
        return nullptr;
    }

    if (eventName == "round_end" && g_settings.chatLogs &&
        g_settings.surrenderLogs) {
        const std::string message = ReadGameEventString(event, "message", "");
        if (IsSurrenderText(message)) {
            AddGameLog("SURRENDER", FriendlyToken(message),
                       IM_COL32(255, 148, 96, 255));
        }
    }
    return nullptr;
}

void* __fastcall OnBulletImpact(void*, void* event) {
    if (!event || !g_settings.bulletTracers || !g_client) {
        return nullptr;
    }

    const std::uintptr_t entityList =
        Read<std::uintptr_t>(g_client + offsets::dwEntityList);
    const std::uintptr_t localPawn =
        Read<std::uintptr_t>(g_client + offsets::dwLocalPlayerPawn);
    if (!entityList || !localPawn) return nullptr;

    const std::uint32_t controllerHandle =
        Read<std::uint32_t>(localPawn + offsets::m_hController);
    const std::uintptr_t localController =
        EntityFromHandle(entityList, controllerHandle);
    if (g_gameEventGetController) {
        const EventHash userId{0, "userid"};
        if (!localController ||
            g_gameEventGetController(event, &userId) != localController) {
            return nullptr;
        }
    } else {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG lastShot =
            g_lastLocalShotAt.load(std::memory_order_relaxed);
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0 &&
            (lastShot == 0 || now - lastShot > 250)) {
            return nullptr;
        }
    }

    const Vec3 impact{
        ReadGameEventFloat(event, "x", 0.0f),
        ReadGameEventFloat(event, "y", 0.0f),
        ReadGameEventFloat(event, "z", 0.0f),
    };
    const Vec3 eye = LocalEyePosition(localPawn);
    if (!ValidPosition(impact) || !ValidPosition(eye)) return nullptr;

    StoreBulletTracer(eye, impact, true);
    g_bulletImpactCount.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

int __fastcall BulletImpactDebugId(void* self) {
    return reinterpret_cast<GameEventListener*>(self)->debugId;
}

void RenderBulletTracers(const Matrix4x4& matrix, float width, float height) {
    const std::lock_guard<std::mutex> lock(g_tracerMutex);
    if (!g_settings.bulletTracers) {
        g_bulletTracers.clear();
        return;
    }

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ULONGLONG now = GetTickCount64();
    const float durationMs =
        std::max(g_settings.tracerDuration, 0.1f) * 1000.0f;
    for (auto it = g_bulletTracers.begin(); it != g_bulletTracers.end();) {
        const float age = static_cast<float>(now - it->createdAt);
        if (age >= durationMs) {
            it = g_bulletTracers.erase(it);
            continue;
        }

        Vec3 startScreen{};
        Vec3 endScreen{};
        if (!WorldSegmentToScreen(it->start, it->end, startScreen, endScreen,
                                  matrix, width, height)) {
            ++it;
            continue;
        }
        const ImVec2 start(startScreen.x, startScreen.y);
        const ImVec2 end(endScreen.x, endScreen.y);
        const float screenDx = end.x - start.x;
        const float screenDy = end.y - start.y;
        // A 3D beam viewed exactly end-on has no visible length.  Suppress the
        // tiny screen-space remnant instead of showing a dot at the crosshair.
        if (screenDx * screenDx + screenDy * screenDy < 36.0f) {
            ++it;
            continue;
        }
        const float fade = std::clamp(1.0f - age / durationMs, 0.0f, 1.0f);
        const float alpha = g_settings.tracerColor[3] * fade;
        const ImU32 color = ImGui::ColorConvertFloat4ToU32(
            ImVec4(g_settings.tracerColor[0], g_settings.tracerColor[1],
                   g_settings.tracerColor[2], alpha));
        if (g_settings.tracerGlow) {
            const ImU32 glow = ImGui::ColorConvertFloat4ToU32(
                ImVec4(g_settings.tracerColor[0], g_settings.tracerColor[1],
                       g_settings.tracerColor[2], alpha * 0.22f));
            draw->AddLine(start, end, glow,
                          g_settings.tracerThickness + 5.0f);
        }
        draw->AddLine(start, end, color, g_settings.tracerThickness);
        ++it;
    }
}

bool IsPawn(std::uintptr_t pawn) {
    const int health = Read<int>(pawn + offsets::m_iHealth);
    const std::uint8_t team = Read<std::uint8_t>(pawn + offsets::m_iTeamNum);
    return health > 0 && health <= 100 && team >= 2 && team <= 3;
}

Bounds PlayerBounds(std::uintptr_t pawn, const Matrix4x4& matrix,
                    float screenWidth, float screenHeight) {
    const std::uintptr_t collision =
        Read<std::uintptr_t>(pawn + offsets::m_pCollision);
    const std::uintptr_t sceneNode =
        Read<std::uintptr_t>(pawn + offsets::m_pGameSceneNode);
    if (!collision || !sceneNode) return {};

    const Vec3 origin = Read<Vec3>(sceneNode + offsets::m_vecOrigin);
    const Vec3 localMins = Read<Vec3>(collision + offsets::m_vecMins);
    const Vec3 localMaxs = Read<Vec3>(collision + offsets::m_vecMaxs);
    if (!ValidPosition(origin)) return {};

    const Vec3 mins{origin.x + localMins.x, origin.y + localMins.y,
                    origin.z + localMins.z};
    const Vec3 maxs{origin.x + localMaxs.x, origin.y + localMaxs.y,
                    origin.z + localMaxs.z};
    Bounds result{std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::max(),
                  -std::numeric_limits<float>::max(),
                  -std::numeric_limits<float>::max(), false};
    bool projected = false;
    for (unsigned i = 0; i < 8; ++i) {
        const Vec3 corner{
            (i & 1) ? maxs.x : mins.x,
            (i & 2) ? maxs.y : mins.y,
            (i & 4) ? maxs.z : mins.z,
        };
        Vec3 screen{};
        if (!WorldToScreen(corner, screen, matrix, screenWidth, screenHeight)) {
            continue;
        }
        result.minX = std::min(result.minX, screen.x);
        result.minY = std::min(result.minY, screen.y);
        result.maxX = std::max(result.maxX, screen.x);
        result.maxY = std::max(result.maxY, screen.y);
        projected = true;
    }
    result.valid = projected && result.minX < result.maxX &&
                   result.minY < result.maxY;
    return result;
}

void AddUniquePawn(std::vector<std::uintptr_t>& pawns, std::uintptr_t pawn) {
    if (!IsPawn(pawn)) return;
    if (std::find(pawns.begin(), pawns.end(), pawn) == pawns.end()) {
        pawns.push_back(pawn);
    }
}

void ScanPawns(std::uintptr_t entityList, int highest,
               std::vector<std::uintptr_t>& pawns, int& chunksRead,
               std::uintptr_t localPawn, bool& localFound) {
    pawns.clear();
    chunksRead = 0;
    localFound = false;

    const std::uintptr_t controllerChunk = Chunk(entityList, 0);
    for (std::uint32_t index = 1; index <= 64 && controllerChunk != 0; ++index) {
        const std::uintptr_t controller = Read<std::uintptr_t>(
            controllerChunk + offsets::entityIdentityStride * index);
        const std::uint32_t handle =
            Read<std::uint32_t>(controller + offsets::m_hPlayerPawn);
        AddUniquePawn(pawns, EntityFromHandle(entityList, handle));
    }
    if (!pawns.empty()) return;

    const int chunkCount = std::clamp((highest >> offsets::entityChunkShift) + 1, 1, 8);
    for (int chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
        const std::uintptr_t chunk = Chunk(entityList, static_cast<std::uint32_t>(chunkIndex));
        if (!chunk) continue;
        ++chunksRead;

        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(chunk), &info, sizeof(info)) == 0 ||
            !Readable(info)) {
            continue;
        }
        const auto regionEnd =
            reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        const auto scanEnd = std::min(chunk + kRawChunkScanSize, regionEnd);
        for (std::uintptr_t address = chunk;
             address + sizeof(std::uintptr_t) <= scanEnd;
             address += sizeof(std::uintptr_t)) {
            const std::uintptr_t candidate = Read<std::uintptr_t>(address);
            if (candidate == localPawn) localFound = true;
            AddUniquePawn(pawns, candidate);
        }
    }
}

void AddLine(std::vector<Vertex>& vertices, float x1, float y1, float x2, float y2,
             float width, float height, float r, float g, float b, float a = 1.0f) {
    if (vertices.size() + 2 > kMaxVertices || width <= 0.0f || height <= 0.0f) return;
    const auto makeVertex = [&](float x, float y) {
        return Vertex{2.0f * x / width - 1.0f,
                      1.0f - 2.0f * y / height, r, g, b, a};
    };
    vertices.push_back(makeVertex(x1, y1));
    vertices.push_back(makeVertex(x2, y2));
}

void AddBox(std::vector<Vertex>& vertices, float x, float y, float width,
            float height, int health, float screenWidth, float screenHeight) {
    AddLine(vertices, x, y, x + width, y, screenWidth, screenHeight, 1.0f, 0.1f, 0.1f);
    AddLine(vertices, x + width, y, x + width, y + height,
            screenWidth, screenHeight, 1.0f, 0.1f, 0.1f);
    AddLine(vertices, x + width, y + height, x, y + height,
            screenWidth, screenHeight, 1.0f, 0.1f, 0.1f);
    AddLine(vertices, x, y + height, x, y,
            screenWidth, screenHeight, 1.0f, 0.1f, 0.1f);

    const float ratio = static_cast<float>(std::clamp(health, 0, 100)) / 100.0f;
    AddLine(vertices, x - 5.0f, y + height, x - 5.0f,
            y + height * (1.0f - ratio), screenWidth, screenHeight,
            1.0f - ratio, ratio, 0.0f);
}

LRESULT CALLBACK GameWindowProc(HWND window, UINT message, WPARAM wParam,
                                LPARAM lParam) {
    if (g_menuOpen.load(std::memory_order_relaxed) && g_imguiInitialized) {
        ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);

        // A key may have been pressed before the menu opened. Forwarding its
        // release prevents Source input from retaining a stale movement state.
        if (message == WM_KEYUP || message == WM_SYSKEYUP) {
            return CallWindowProcW(g_originalWndProc, window, message, wParam,
                                   lParam);
        }

        switch (message) {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            case WM_CHAR:
            case WM_SYSCHAR:
            case WM_INPUT:
            case WM_MOUSEMOVE:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
            case WM_XBUTTONDBLCLK:
                return 0;
            case WM_SETCURSOR:
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            default:
                break;
        }
    }
    return CallWindowProcW(g_originalWndProc, window, message, wParam, lParam);
}

void ReleaseRenderer() {
    if (g_imguiInitialized) {
        if (g_gameWindow && g_originalWndProc) {
            SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(g_originalWndProc));
        }
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiInitialized = false;
        g_originalWndProc = nullptr;
        g_gameWindow = nullptr;
    }
    if (g_rasterizerState) g_rasterizerState->Release();
    if (g_depthState) g_depthState->Release();
    if (g_blendState) g_blendState->Release();
    if (g_vertexBuffer) g_vertexBuffer->Release();
    if (g_inputLayout) g_inputLayout->Release();
    if (g_pixelShader) g_pixelShader->Release();
    if (g_vertexShader) g_vertexShader->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    g_rasterizerState = nullptr;
    g_depthState = nullptr;
    g_blendState = nullptr;
    g_vertexBuffer = nullptr;
    g_inputLayout = nullptr;
    g_pixelShader = nullptr;
    g_vertexShader = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

bool InitializeImGui(IDXGISwapChain* swapChain) {
    if (g_imguiInitialized) return true;
    if (!InitializeRenderer(swapChain)) return false;

    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swapChain->GetDesc(&description)) || !description.OutputWindow) {
        return false;
    }
    g_gameWindow = description.OutputWindow;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 14.0f,
                                 nullptr, io.Fonts->GetGlyphRangesCyrillic());

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(9.0f, 7.0f);
    style.ScrollbarSize = 11.0f;
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.86f, 0.89f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.49f, 0.58f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.045f, 0.052f, 0.070f, 0.98f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.065f, 0.075f, 0.100f, 0.96f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.050f, 0.058f, 0.078f, 0.99f);
    colors[ImGuiCol_Border] = ImVec4(0.22f, 0.25f, 0.31f, 1.00f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.35f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.090f, 0.105f, 0.140f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.13f, 0.17f, 0.24f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.24f, 0.38f, 1.00f);
    colors[ImGuiCol_TitleBg] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_TitleBgActive] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_CheckMark] = ImVec4(0.27f, 0.56f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.27f, 0.56f, 1.00f, 0.85f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.67f, 1.00f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.105f, 0.125f, 0.170f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.17f, 0.27f, 0.44f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.22f, 0.42f, 0.72f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.10f, 0.12f, 0.17f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.14f, 0.23f, 0.38f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.39f, 0.67f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.23f, 0.29f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.27f, 0.56f, 1.00f, 0.35f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.27f, 0.56f, 1.00f, 0.70f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.40f, 0.67f, 1.00f, 1.00f);

    if (!ImGui_ImplWin32_Init(g_gameWindow) ||
        !ImGui_ImplDX11_Init(g_device, g_context)) {
        ImGui::DestroyContext();
        g_gameWindow = nullptr;
        return false;
    }
    SetLastError(0);
    g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GameWindowProc)));
    if (!g_originalWndProc && GetLastError() != 0) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_gameWindow = nullptr;
        return false;
    }
    g_imguiInitialized = true;
    Log(L"ImGui initialized");
    return true;
}

bool InitializeRenderer(IDXGISwapChain* swapChain) {
    if (g_device) return true;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device),
                                    reinterpret_cast<void**>(&g_device)))) {
        return false;
    }
    g_device->GetImmediateContext(&g_context);

    constexpr char vertexSource[] =
        "struct VIn{float2 p:POSITION;float4 c:COLOR;};"
        "struct VOut{float4 p:SV_POSITION;float4 c:COLOR;};"
        "VOut main(VIn i){VOut o;o.p=float4(i.p,0,1);o.c=i.c;return o;}";
    constexpr char pixelSource[] =
        "float4 main(float4 p:SV_POSITION,float4 c:COLOR):SV_TARGET{return c;}";

    ID3DBlob* vertexBlob = nullptr;
    ID3DBlob* pixelBlob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT result = D3DCompile(vertexSource, sizeof(vertexSource), nullptr, nullptr,
                                nullptr, "main", "vs_4_0", 0, 0,
                                &vertexBlob, &errors);
    if (errors) {
        errors->Release();
        errors = nullptr;
    }
    if (FAILED(result)) {
        ReleaseRenderer();
        return false;
    }
    result = D3DCompile(pixelSource, sizeof(pixelSource), nullptr, nullptr,
                        nullptr, "main", "ps_4_0", 0, 0,
                        &pixelBlob, &errors);
    if (errors) {
        errors->Release();
        errors = nullptr;
    }
    if (FAILED(result)) {
        vertexBlob->Release();
        ReleaseRenderer();
        return false;
    }

    result = g_device->CreateVertexShader(vertexBlob->GetBufferPointer(),
                                           vertexBlob->GetBufferSize(), nullptr,
                                           &g_vertexShader);
    if (SUCCEEDED(result)) {
        result = g_device->CreatePixelShader(pixelBlob->GetBufferPointer(),
                                              pixelBlob->GetBufferSize(), nullptr,
                                              &g_pixelShader);
    }
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (SUCCEEDED(result)) {
        result = g_device->CreateInputLayout(elements, 2,
                                              vertexBlob->GetBufferPointer(),
                                              vertexBlob->GetBufferSize(),
                                              &g_inputLayout);
    }
    vertexBlob->Release();
    pixelBlob->Release();
    if (FAILED(result)) {
        ReleaseRenderer();
        return false;
    }

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = static_cast<UINT>(sizeof(Vertex) * kMaxVertices);
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_device->CreateBuffer(&bufferDesc, nullptr, &g_vertexBuffer))) {
        ReleaseRenderer();
        return false;
    }

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_device->CreateBlendState(&blendDesc, &g_blendState);

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = FALSE;
    depthDesc.StencilEnable = FALSE;
    g_device->CreateDepthStencilState(&depthDesc, &g_depthState);

    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    g_device->CreateRasterizerState(&rasterDesc, &g_rasterizerState);
    Log(L"DirectX renderer initialized");
    return g_blendState && g_depthState && g_rasterizerState;
}

void RenderVertices(IDXGISwapChain* swapChain, const std::vector<Vertex>& vertices) {
    if (vertices.empty() || !InitializeRenderer(swapChain)) return;

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&backBuffer)))) {
        return;
    }
    ID3D11RenderTargetView* target = nullptr;
    const HRESULT targetResult =
        g_device->CreateRenderTargetView(backBuffer, nullptr, &target);
    backBuffer->Release();
    if (FAILED(targetResult)) return;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(g_vertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        target->Release();
        return;
    }
    std::memcpy(mapped.pData, vertices.data(), vertices.size() * sizeof(Vertex));
    g_context->Unmap(g_vertexBuffer, 0);

    ID3D11RenderTargetView* oldTarget = nullptr;
    ID3D11DepthStencilView* oldDepthView = nullptr;
    ID3D11InputLayout* oldLayout = nullptr;
    ID3D11Buffer* oldBuffer = nullptr;
    ID3D11VertexShader* oldVertexShader = nullptr;
    ID3D11PixelShader* oldPixelShader = nullptr;
    ID3D11BlendState* oldBlend = nullptr;
    ID3D11DepthStencilState* oldDepthState = nullptr;
    ID3D11RasterizerState* oldRasterizer = nullptr;
    UINT oldStride = 0;
    UINT oldOffset = 0;
    UINT oldSampleMask = 0;
    UINT oldStencilReference = 0;
    float oldBlendFactor[4]{};
    D3D11_PRIMITIVE_TOPOLOGY oldTopology{};

    g_context->OMGetRenderTargets(1, &oldTarget, &oldDepthView);
    g_context->IAGetInputLayout(&oldLayout);
    g_context->IAGetVertexBuffers(0, 1, &oldBuffer, &oldStride, &oldOffset);
    g_context->IAGetPrimitiveTopology(&oldTopology);
    g_context->VSGetShader(&oldVertexShader, nullptr, nullptr);
    g_context->PSGetShader(&oldPixelShader, nullptr, nullptr);
    g_context->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
    g_context->OMGetDepthStencilState(&oldDepthState, &oldStencilReference);
    g_context->RSGetState(&oldRasterizer);

    const UINT stride = sizeof(Vertex);
    const UINT offset = 0;
    const float blendFactor[4]{};
    g_context->OMSetRenderTargets(1, &target, nullptr);
    g_context->OMSetBlendState(g_blendState, blendFactor, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(g_depthState, 0);
    g_context->RSSetState(g_rasterizerState);
    g_context->IASetInputLayout(g_inputLayout);
    g_context->IASetVertexBuffers(0, 1, &g_vertexBuffer, &stride, &offset);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    g_context->VSSetShader(g_vertexShader, nullptr, 0);
    g_context->PSSetShader(g_pixelShader, nullptr, 0);
    g_context->Draw(static_cast<UINT>(vertices.size()), 0);

    g_context->OMSetRenderTargets(1, &oldTarget, oldDepthView);
    g_context->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
    g_context->OMSetDepthStencilState(oldDepthState, oldStencilReference);
    g_context->RSSetState(oldRasterizer);
    g_context->IASetInputLayout(oldLayout);
    g_context->IASetVertexBuffers(0, 1, &oldBuffer, &oldStride, &oldOffset);
    g_context->IASetPrimitiveTopology(oldTopology);
    g_context->VSSetShader(oldVertexShader, nullptr, 0);
    g_context->PSSetShader(oldPixelShader, nullptr, 0);

    if (oldRasterizer) oldRasterizer->Release();
    if (oldDepthState) oldDepthState->Release();
    if (oldBlend) oldBlend->Release();
    if (oldPixelShader) oldPixelShader->Release();
    if (oldVertexShader) oldVertexShader->Release();
    if (oldBuffer) oldBuffer->Release();
    if (oldLayout) oldLayout->Release();
    if (oldDepthView) oldDepthView->Release();
    if (oldTarget) oldTarget->Release();
    target->Release();
}

std::string PlayerName(std::uintptr_t entityList, std::uintptr_t pawn) {
    const std::uint32_t controllerHandle =
        Read<std::uint32_t>(pawn + offsets::m_hController);
    const std::uintptr_t controller =
        EntityFromHandle(entityList, controllerHandle);
    const std::uintptr_t namePointer =
        Read<std::uintptr_t>(controller + offsets::m_sSanitizedPlayerName);
    return ReadString(namePointer, 128);
}

std::string WeaponName(std::uintptr_t entityList, std::uintptr_t pawn) {
    const std::uintptr_t services =
        Read<std::uintptr_t>(pawn + offsets::m_pWeaponServices);
    const std::uint32_t weaponHandle =
        Read<std::uint32_t>(services + offsets::m_hActiveWeapon);
    const std::uintptr_t weapon = EntityFromHandle(entityList, weaponHandle);
    const std::uintptr_t weaponData =
        Read<std::uintptr_t>(weapon + offsets::m_nSubclassID + 0x8);
    const std::uintptr_t namePointer =
        Read<std::uintptr_t>(weaponData + offsets::m_szWeaponName);
    std::string name = ReadString(namePointer, 64);
    constexpr char prefix[] = "weapon_";
    if (name.rfind(prefix, 0) == 0) name.erase(0, sizeof(prefix) - 1);
    return name;
}

int WeaponGroup(std::uintptr_t entityList, std::uintptr_t pawn) {
    if (!entityList || !pawn) return -1;
    const std::uintptr_t services =
        Read<std::uintptr_t>(pawn + offsets::m_pWeaponServices);
    const std::uint32_t handle =
        Read<std::uint32_t>(services + offsets::m_hActiveWeapon);
    const std::uintptr_t weapon = EntityFromHandle(entityList, handle);
    if (!weapon) return -1;

    // m_iItemDefinitionIndex belongs to C_EconItemView.  C_EconEntity embeds
    // C_AttributeContainer, which in turn embeds the item view.
    const int id = Read<std::uint16_t>(
        weapon + offsets::m_AttributeManager + offsets::m_Item +
        offsets::m_iItemDefinitionIndex);
    switch (id) {
        case 1: case 2: case 3: case 4: case 30: case 32: case 36:
        case 61: case 63: case 64:
            return 0;
        case 17: case 19: case 23: case 24: case 26: case 33: case 34:
            return 1;
        case 7: case 8: case 10: case 13: case 16: case 39: case 60:
            return 2;
        case 25: case 27: case 29: case 35:
            return 3;
        case 9: case 11: case 38: case 40:
            return 4;
        case 14: case 28:
            return 5;
        default:
            break;
    }

    // Keep a schema-independent fallback.  WeaponName uses weapon VData and
    // has proved stable even when the economy-item layout changes.
    const std::string name = WeaponName(entityList, pawn);
    if (name == "deagle" || name == "elite" || name == "fiveseven" ||
        name == "glock" || name == "tec9" || name == "hkp2000" ||
        name == "p250" || name == "usp_silencer" || name == "cz75a" ||
        name == "revolver") {
        return 0;
    }
    if (name == "mac10" || name == "p90" || name == "mp5sd" ||
        name == "ump45" || name == "bizon" || name == "mp7" ||
        name == "mp9") {
        return 1;
    }
    if (name == "ak47" || name == "aug" || name == "famas" ||
        name == "galilar" || name == "m4a1" || name == "sg556" ||
        name == "m4a1_silencer") {
        return 2;
    }
    if (name == "xm1014" || name == "mag7" || name == "sawedoff" ||
        name == "nova") {
        return 3;
    }
    if (name == "awp" || name == "g3sg1" || name == "scar20" ||
        name == "ssg08") {
        return 4;
    }
    if (name == "m249" || name == "negev") return 5;
    return -1;
}

void ClickPrimaryAttack() {
    g_lastLocalShotAt.store(GetTickCount64(), std::memory_order_relaxed);
    if (g_engineClient && g_engineClientCmd) {
        if (!g_attackHeld) {
            g_engineClientCmd(g_engineClient, 0, "+attack", 0x7ffef001ull);
            g_attackHeld = true;
        }
        g_attackReleaseAt = GetTickCount64() + 32;
        return;
    }
    INPUT inputs[2]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, inputs, sizeof(INPUT));
}

void ReleasePrimaryAttack(bool force = false) {
    if (!g_attackHeld || !g_engineClient || !g_engineClientCmd) return;
    if (!force && GetTickCount64() < g_attackReleaseAt) return;
    g_engineClientCmd(g_engineClient, 0, "-attack", 0x7ffef001ull);
    g_attackHeld = false;
    g_attackReleaseAt = 0;
}

void RunTrigger(std::uintptr_t entityList, std::uintptr_t localPawn,
                const Matrix4x4& matrix, float screenWidth,
                float screenHeight) {
    ReleasePrimaryAttack();
    g_triggerKeyHeld.store(false, std::memory_order_relaxed);
    g_triggerTargetFound.store(false, std::memory_order_relaxed);
    g_crosshairEntity.store(-1, std::memory_order_relaxed);
    if (!g_settings.triggerEnabled || !localPawn || !entityList) return;

    const int group = WeaponGroup(entityList, localPawn);
    if (group < 0 || group >= static_cast<int>(g_settings.trigger.size())) return;
    Settings::TriggerWeapon& cfg = g_settings.trigger[group];
    if (!cfg.enabled) return;
    if (g_menuOpen.load(std::memory_order_relaxed) ||
        GetForegroundWindow() != g_gameWindow) return;

    static std::uintptr_t pendingTarget = 0;
    static ULONGLONG targetSince = 0;
    static ULONGLONG lastShot = 0;
    const bool keyHeld =
        cfg.key == 0 || (GetAsyncKeyState(cfg.key) & 0x8000) != 0;
    g_triggerKeyHeld.store(keyHeld, std::memory_order_relaxed);
    if (!keyHeld) {
        pendingTarget = 0;
        targetSince = 0;
        return;
    }

    const int crosshairIndex =
        Read<int>(localPawn + offsets::m_iIDEntIndex);
    g_crosshairEntity.store(crosshairIndex, std::memory_order_relaxed);
    const std::uintptr_t target = crosshairIndex > 0
                                      ? EntityFromHandle(entityList,
                                                         crosshairIndex)
                                      : 0;
    const std::uint8_t localTeam =
        Read<std::uint8_t>(localPawn + offsets::m_iTeamNum);
    if (!IsPawn(target) || target == localPawn ||
        Read<std::uint8_t>(target + offsets::m_iTeamNum) == localTeam) {
        pendingTarget = 0;
        targetSince = 0;
        return;
    }
    if (cfg.headOnly) {
        const Bounds bounds =
            PlayerBounds(target, matrix, screenWidth, screenHeight);
        const float crosshairX = screenWidth * 0.5f;
        const float crosshairY = screenHeight * 0.5f;
        const float headBottom =
            bounds.minY + (bounds.maxY - bounds.minY) * 0.30f;
        if (!bounds.valid || crosshairX < bounds.minX ||
            crosshairX > bounds.maxX || crosshairY < bounds.minY ||
            crosshairY > headBottom) {
            pendingTarget = 0;
            targetSince = 0;
            return;
        }
    }
    g_triggerTargetFound.store(true, std::memory_order_relaxed);

    const ULONGLONG now = GetTickCount64();
    if (pendingTarget != target) {
        pendingTarget = target;
        targetSince = now;
    }
    if (now - targetSince < static_cast<ULONGLONG>(cfg.delay) ||
        now - lastShot < 70) {
        return;
    }
    ClickPrimaryAttack();
    g_triggerShots.fetch_add(1, std::memory_order_relaxed);
    lastShot = now;
    targetSince = now;
}

void DrawOutlinedText(ImDrawList* drawList, ImVec2 position, ImU32 color,
                      const std::string& text) {
    if (text.empty()) return;
    constexpr ImU32 outline = IM_COL32(0, 0, 0, 230);
    drawList->AddText(ImVec2(position.x - 1, position.y), outline, text.c_str());
    drawList->AddText(ImVec2(position.x + 1, position.y), outline, text.c_str());
    drawList->AddText(ImVec2(position.x, position.y - 1), outline, text.c_str());
    drawList->AddText(ImVec2(position.x, position.y + 1), outline, text.c_str());
    drawList->AddText(position, color, text.c_str());
}

std::string PurchaseItemsText(const PurchaseEntry& entry) {
    std::vector<std::pair<std::string, int>> counts;
    for (const std::string& item : entry.items) {
        auto found = std::find_if(counts.begin(), counts.end(),
                                  [&](const auto& pair) {
                                      return pair.first == item;
                                  });
        if (found == counts.end()) counts.emplace_back(item, 1);
        else ++found->second;
    }
    std::string text;
    for (const auto& [item, count] : counts) {
        if (!text.empty()) text += ", ";
        text += item;
        if (count > 1) text += " x" + std::to_string(count);
    }
    if (text.size() > 82) text = text.substr(0, 79) + "...";
    return text.empty() ? "no purchases" : text;
}

void RenderPurchasesOnTab(float width, float height) {
    if (!g_settings.purchasesOnTab ||
        (GetAsyncKeyState(VK_TAB) & 0x8000) == 0 ||
        g_menuOpen.load(std::memory_order_relaxed)) {
        return;
    }

    std::vector<PurchaseEntry> purchases;
    {
        const std::lock_guard<std::mutex> lock(g_gameLogMutex);
        purchases = g_roundPurchases;
    }
    std::sort(purchases.begin(), purchases.end(),
              [](const PurchaseEntry& left, const PurchaseEntry& right) {
                  if (left.team != right.team) return left.team < right.team;
                  return left.player < right.player;
              });

    constexpr float panelWidth = 500.0f;
    constexpr float headerHeight = 38.0f;
    constexpr float rowHeight = 25.0f;
    const std::size_t shown = std::min<std::size_t>(purchases.size(), 12);
    const float panelHeight = headerHeight +
        rowHeight * static_cast<float>(std::max<std::size_t>(shown, 1)) + 10.0f;
    const float x = std::clamp(width * 0.5f + 185.0f, 12.0f,
                               std::max(12.0f, width - panelWidth - 12.0f));
    const float y = std::clamp(height * 0.18f, 12.0f,
                               std::max(12.0f, height - panelHeight - 12.0f));
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    draw->AddRectFilled(ImVec2(x, y), ImVec2(x + panelWidth, y + panelHeight),
                        IM_COL32(12, 15, 22, 232), 7.0f);
    draw->AddRect(ImVec2(x, y), ImVec2(x + panelWidth, y + panelHeight),
                  IM_COL32(74, 135, 240, 230), 7.0f, 0, 1.5f);
    draw->AddRectFilled(ImVec2(x, y), ImVec2(x + panelWidth, y + 4.0f),
                        IM_COL32(69, 143, 255, 255), 7.0f,
                        ImDrawFlags_RoundCornersTop);
    draw->AddText(ImVec2(x + 12.0f, y + 13.0f), IM_COL32(225, 233, 247, 255),
                  "PLAYER PURCHASES - CURRENT ROUND");

    if (purchases.empty()) {
        draw->AddText(ImVec2(x + 12.0f, y + headerHeight + 5.0f),
                      IM_COL32(145, 153, 169, 255), "No purchases recorded");
        return;
    }

    for (std::size_t index = 0; index < shown; ++index) {
        const PurchaseEntry& entry = purchases[index];
        const float rowY = y + headerHeight + rowHeight * static_cast<float>(index);
        if ((index & 1U) != 0) {
            draw->AddRectFilled(ImVec2(x + 6.0f, rowY),
                                ImVec2(x + panelWidth - 6.0f, rowY + rowHeight),
                                IM_COL32(255, 255, 255, 8), 3.0f);
        }
        const char* team = entry.team == 2 ? "T" : entry.team == 3 ? "CT" : "?";
        const ImU32 teamColor = entry.team == 2
                                    ? IM_COL32(232, 186, 92, 255)
                                    : entry.team == 3
                                          ? IM_COL32(104, 170, 255, 255)
                                          : IM_COL32(170, 170, 170, 255);
        draw->AddText(ImVec2(x + 12.0f, rowY + 5.0f), teamColor, team);
        std::string player = entry.player;
        if (player.size() > 20) player = player.substr(0, 17) + "...";
        draw->AddText(ImVec2(x + 40.0f, rowY + 5.0f),
                      IM_COL32(236, 239, 246, 255), player.c_str());
        const std::string items = PurchaseItemsText(entry);
        draw->AddText(ImVec2(x + 190.0f, rowY + 5.0f),
                      IM_COL32(174, 184, 202, 255), items.c_str());
    }
}

void RenderGameChatLogs(float width, float height) {
    if (!g_settings.chatLogs) return;
    const ULONGLONG now = GetTickCount64();
    const float durationMs =
        std::max(g_settings.logDuration, 3.0f) * 1000.0f;
    std::vector<GameLogEntry> visible;
    {
        const std::lock_guard<std::mutex> lock(g_gameLogMutex);
        g_gameLogs.erase(
            std::remove_if(g_gameLogs.begin(), g_gameLogs.end(),
                           [&](const GameLogEntry& entry) {
                               return static_cast<float>(now - entry.createdAt) >=
                                      durationMs;
                           }),
            g_gameLogs.end());
        const std::size_t first = g_gameLogs.size() > 7
                                      ? g_gameLogs.size() - 7
                                      : 0;
        visible.assign(g_gameLogs.begin() + first, g_gameLogs.end());
    }
    if (visible.empty()) return;

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float x = 22.0f;
    const float lineHeight = ImGui::GetFontSize() + 9.0f;
    const float bottom = std::clamp(height - 275.0f, 70.0f, height - 20.0f);
    for (std::size_t index = 0; index < visible.size(); ++index) {
        const GameLogEntry& entry = visible[index];
        const float age = static_cast<float>(now - entry.createdAt);
        const float fadeStart = durationMs * 0.78f;
        const float alpha = age <= fadeStart
                                ? 1.0f
                                : std::clamp(1.0f - (age - fadeStart) /
                                                        (durationMs - fadeStart),
                                             0.0f, 1.0f);
        const float y = bottom - lineHeight *
            static_cast<float>(visible.size() - index);
        const std::string line = entry.title + " | " + entry.reason;
        const ImVec2 textSize = ImGui::CalcTextSize(line.c_str());
        draw->AddRectFilled(ImVec2(x - 5.0f, y - 3.0f),
                            ImVec2(std::min(x + textSize.x + 8.0f, width - 8.0f),
                                   y + textSize.y + 3.0f),
                            IM_COL32(8, 10, 15,
                                     static_cast<int>(170.0f * alpha)),
                            3.0f);
        ImVec4 color = ImGui::ColorConvertU32ToFloat4(entry.color);
        color.w *= alpha;
        DrawOutlinedText(draw, ImVec2(x, y),
                         ImGui::ColorConvertFloat4ToU32(color), line);
    }
}

void DrawMenu() {
    if (!g_menuOpen.load(std::memory_order_relaxed)) return;

    static int tab = 0;
    static int triggerGroup = 2;
    constexpr ImVec4 accent{0.27f, 0.56f, 1.00f, 1.00f};
    constexpr ImVec4 good{0.31f, 0.86f, 0.55f, 1.00f};
    constexpr ImVec4 bad{1.00f, 0.37f, 0.42f, 1.00f};

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(690.0f, 470.0f), ImGuiCond_Always);
    ImGui::Begin("nova##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);

    const ImVec2 windowPos = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(windowPos,
                        ImVec2(windowPos.x + windowSize.x, windowPos.y + 4.0f),
                        ImGui::ColorConvertFloat4ToU32(accent), 8.0f,
                        ImDrawFlags_RoundCornersTop);
    draw->AddRect(windowPos,
                  ImVec2(windowPos.x + windowSize.x,
                         windowPos.y + windowSize.y),
                  IM_COL32(62, 69, 84, 255), 8.0f);

    ImGui::SetCursorPos(ImVec2(12.0f, 16.0f));
    ImGui::BeginChild("sidebar", ImVec2(142.0f, 442.0f), true,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::TextUnformatted("N O V A");
    ImGui::PopStyleColor();
    ImGui::TextDisabled("CS2 INTERNAL");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const char* tabs[] = {"Trigger", "Visuals", "Colors", "Logs", "Settings"};
    for (int i = 0; i < static_cast<int>(std::size(tabs)); ++i) {
        if (ImGui::Selectable(tabs[i], tab == i, 0, ImVec2(0.0f, 42.0f))) {
            tab = i;
        }
    }

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 82.0f);
    ImGui::TextDisabled("INSERT  menu");
    ImGui::TextDisabled("END     unload");
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.15f, 0.19f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.82f, 0.20f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImVec4(0.48f, 0.10f, 0.14f, 1.0f));
    if (ImGui::Button("UNLOAD", ImVec2(-1.0f, 28.0f))) {
        g_unloadRequested.store(true, std::memory_order_relaxed);
    }
    ImGui::PopStyleColor(3);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("content", ImVec2(0.0f, 442.0f), false);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.94f, 0.98f, 1.0f));
    ImGui::TextUnformatted(tabs[tab]);
    ImGui::PopStyleColor();
    ImGui::TextDisabled("simple, stable and weapon-specific");
    ImGui::Separator();
    ImGui::Spacing();

    if (tab == 0) {
        constexpr const char* groups[] = {
            "Pistol", "SMG", "Rifle", "Shotgun", "Sniper", "LMG"
        };
        constexpr const char* bindNames[] = {
            "Always", "Mouse 4", "Mouse 5", "Left Alt", "Left Shift",
            "Left Ctrl"
        };
        constexpr int bindValues[] = {
            0, VK_XBUTTON1, VK_XBUTTON2, VK_LMENU, VK_LSHIFT, VK_LCONTROL
        };
        const auto drawBind = [&](const char* label, int& key) {
            int selected = 0;
            for (int i = 0; i < static_cast<int>(std::size(bindValues)); ++i) {
                if (bindValues[i] == key) selected = i;
            }
            ImGui::SetNextItemWidth(210.0f);
            if (ImGui::Combo(label, &selected, bindNames,
                             static_cast<int>(std::size(bindNames)))) {
                key = bindValues[selected];
            }
        };

        ImGui::BeginChild("trigger_card", ImVec2(0.0f, 228.0f), true);
        ImGui::Checkbox("Enable trigger system", &g_settings.triggerEnabled);
        ImGui::Spacing();
        ImGui::SetNextItemWidth(210.0f);
        ImGui::Combo("Weapon profile", &triggerGroup, groups,
                     static_cast<int>(std::size(groups)));

        Settings::TriggerWeapon& cfg = g_settings.trigger[triggerGroup];
        ImGui::PushID(triggerGroup);
        ImGui::Checkbox("Enable for this weapon group", &cfg.enabled);
        drawBind("Activation", cfg.key);
        ImGui::SetNextItemWidth(210.0f);
        ImGui::SliderInt("Shot delay", &cfg.delay, 0, 250, "%d ms");
        ImGui::Checkbox("Head only", &cfg.headOnly);
        ImGui::Spacing();
        if (ImGui::Button("Copy profile to every weapon",
                          ImVec2(238.0f, 30.0f))) {
            for (Settings::TriggerWeapon& destination : g_settings.trigger) {
                destination = cfg;
            }
        }
        ImGui::PopID();
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::BeginChild("trigger_status", ImVec2(0.0f, 120.0f), true);
        const std::uintptr_t entityList =
            Read<std::uintptr_t>(g_client + offsets::dwEntityList);
        const std::uintptr_t localPawn =
            Read<std::uintptr_t>(g_client + offsets::dwLocalPlayerPawn);
        const int activeGroup = WeaponGroup(entityList, localPawn);
        ImGui::Text("Active weapon: %s",
                    activeGroup >= 0 ? groups[activeGroup] : "knife / utility");
        ImGui::TextColored(g_engineClientCmd ? good : bad, "%s",
                           g_engineClientCmd ? "Fire backend ready"
                                             : "Fire backend unavailable");
        ImGui::Text("Key: %s    Target: %s    Entity: %d",
                    g_triggerKeyHeld.load(std::memory_order_relaxed)
                        ? "active" : "idle",
                    g_triggerTargetFound.load(std::memory_order_relaxed)
                        ? "enemy" : "none",
                    g_crosshairEntity.load(std::memory_order_relaxed));
        ImGui::Text("Triggered shots: %llu",
                    static_cast<unsigned long long>(
                        g_triggerShots.load(std::memory_order_relaxed)));
        ImGui::EndChild();
    } else if (tab == 1) {
        ImGui::BeginChild("esp_main", ImVec2(248.0f, 340.0f), true);
        ImGui::TextColored(accent, "PLAYER ESP");
        ImGui::Separator();
        ImGui::Checkbox("Enabled", &g_settings.enabled);
        ImGui::Checkbox("Box", &g_settings.box);
        ImGui::Checkbox("Box outline", &g_settings.boxOutline);
        ImGui::Checkbox("Health bar", &g_settings.healthBar);
        ImGui::Checkbox("Health text", &g_settings.healthText);
        ImGui::Checkbox("Player name", &g_settings.playerName);
        ImGui::Checkbox("Weapon name", &g_settings.weaponName);
        ImGui::Checkbox("Show teammates", &g_settings.teammates);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("esp_shape", ImVec2(0.0f, 340.0f), true);
        ImGui::TextColored(accent, "STYLE");
        ImGui::Separator();
        ImGui::SliderFloat("Box thickness", &g_settings.boxThickness,
                           1.0f, 4.0f, "%.1f px");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Names are drawn above players, weapons below them, and health "
            "is shown as a side bar with a numeric value.");
        ImGui::Spacing();
        ImGui::TextColored(accent, "BULLET TRACERS");
        ImGui::Separator();
        const bool bulletDataReady =
            g_bulletServiceReady.load(std::memory_order_relaxed);
        ImGui::TextColored(bulletDataReady ? good : bad, "%s",
                           bulletDataReady ? "BulletServices ready"
                                           : "Waiting for first shot");
        ImGui::Text("Impact buffer: %d",
                    g_bulletServiceCount.load(std::memory_order_relaxed));
        ImGui::Text("Captured impacts: %llu",
                    static_cast<unsigned long long>(
                        g_bulletImpactCount.load(std::memory_order_relaxed)));
        ImGui::Checkbox("Enabled##tracers", &g_settings.bulletTracers);
        ImGui::Checkbox("Glow##tracers", &g_settings.tracerGlow);
        ImGui::SliderFloat("Duration##tracers", &g_settings.tracerDuration,
                           0.1f, 10.0f, "%.1f s");
        ImGui::SliderFloat("Thickness##tracers", &g_settings.tracerThickness,
                           1.0f, 6.0f, "%.1f px");
        ImGui::EndChild();
    } else if (tab == 2) {
        ImGui::BeginChild("colors_card", ImVec2(0.0f, 210.0f), true);
        ImGui::TextColored(accent, "ESP PALETTE");
        ImGui::Separator();
        ImGui::ColorEdit4("Box color", g_settings.boxColor,
                          ImGuiColorEditFlags_AlphaBar);
        ImGui::ColorEdit4("Text color", g_settings.textColor,
                          ImGuiColorEditFlags_AlphaBar);
        ImGui::ColorEdit4("Tracer color", g_settings.tracerColor,
                          ImGuiColorEditFlags_AlphaBar);
        ImGui::EndChild();
    } else if (tab == 3) {
        ImGui::BeginChild("logs_card", ImVec2(0.0f, 300.0f), true);
        ImGui::TextColored(accent, "PURCHASES AND GAME LOGS");
        ImGui::Separator();
        ImGui::TextColored(
            g_gameLogRegistered.load(std::memory_order_relaxed) ? good : bad,
            "%s (%d/8 events)",
            g_gameLogRegistered.load(std::memory_order_relaxed)
                ? "Event listener ready"
                : "Event listener unavailable",
            g_gameLogListenerCount.load(std::memory_order_relaxed));
        ImGui::TextColored(g_findHudElement && g_setVoiceData ? good : bad,
                           "%s", g_findHudElement && g_setVoiceData
                                     ? "Velocity HUD chat ready"
                                     : "Velocity HUD chat unavailable");
        ImGui::TextColored(g_scoreboardScriptInjected ? good : accent, "%s",
                           g_scoreboardScriptInjected
                               ? "Velocity Panorama scoreboard ready"
                               : "Panorama scoreboard waits for TAB panel");
        ImGui::TextDisabled("TAB status: %s", g_scoreboardStatus.c_str());
        ImGui::Spacing();
        ImGui::Checkbox("Velocity weapon icons in game scoreboard",
                        &g_settings.purchasesOnTab);
        ImGui::Checkbox("Velocity logs in real game chat", &g_settings.chatLogs);
        ImGui::Indent(18.0f);
        ImGui::Checkbox("Purchase logs", &g_settings.purchaseLogs);
        ImGui::Checkbox("Vote logs", &g_settings.voteLogs);
        ImGui::Checkbox("Kick logs", &g_settings.kickLogs);
        ImGui::Checkbox("Surrender logs", &g_settings.surrenderLogs);
        ImGui::Unindent(18.0f);
        ImGui::Spacing();
        if (ImGui::Button("Refresh scoreboard", ImVec2(190.0f, 30.0f))) {
            ShutdownVelocityScoreboard();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear logs", ImVec2(150.0f, 30.0f))) {
            const std::lock_guard<std::mutex> lock(g_gameLogMutex);
            g_gameLogs.clear();
        }
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("settings_card", ImVec2(0.0f, 245.0f), true);
        ImGui::TextColored(accent, "CONFIGURATION");
        ImGui::Separator();
        ImGui::TextUnformatted("INSERT opens or closes the menu.");
        ImGui::TextUnformatted("END unloads the DLL from the game.");
        ImGui::Spacing();
        if (ImGui::Button("Save settings", ImVec2(180.0f, 32.0f))) {
            SaveSettings();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset defaults", ImVec2(180.0f, 32.0f))) {
            g_settings = Settings{};
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Only ESP and trigger functionality are included.");
        ImGui::EndChild();
    }

    ImGui::EndChild();
    ImGui::End();
}

HRESULT __stdcall HookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    if (!g_running.load(std::memory_order_relaxed) || !InitializeImGui(swapChain)) {
        return g_originalPresent(swapChain, syncInterval, flags);
    }

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&backBuffer)))) {
        return g_originalPresent(swapChain, syncInterval, flags);
    }
    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);
    ID3D11RenderTargetView* target = nullptr;
    const HRESULT targetResult =
        g_device->CreateRenderTargetView(backBuffer, nullptr, &target);
    backBuffer->Release();
    if (FAILED(targetResult)) {
        return g_originalPresent(swapChain, syncInterval, flags);
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    const bool menuOpen = g_menuOpen.load(std::memory_order_relaxed);
    ImGui::GetIO().MouseDrawCursor = menuOpen;
    if (menuOpen) {
        // CS2 may attempt to recapture/clip the pointer on a focus or mode
        // transition. Keep it released for every menu frame.
        ReleaseCapture();
        ClipCursor(nullptr);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }

    const float width = static_cast<float>(desc.Width);
    const float height = static_cast<float>(desc.Height);
    const std::uintptr_t entityList =
        Read<std::uintptr_t>(g_client + offsets::dwEntityList);
    const std::uintptr_t localPawn =
        Read<std::uintptr_t>(g_client + offsets::dwLocalPlayerPawn);
    const std::uint8_t localTeam =
        Read<std::uint8_t>(localPawn + offsets::m_iTeamNum);
    const Matrix4x4 matrix =
        Read<Matrix4x4>(g_client + offsets::dwViewMatrix);

    static std::uintptr_t previousShotPawn = 0;
    static int previousShotsFired = 0;
    const int shotsFired = Read<int>(localPawn + offsets::m_iShotsFired);
    if (localPawn != previousShotPawn) {
        previousShotPawn = localPawn;
        previousShotsFired = shotsFired;
    } else if (shotsFired > previousShotsFired) {
        g_lastLocalShotAt.store(GetTickCount64(), std::memory_order_relaxed);
    }
    previousShotsFired = shotsFired;
    PollBulletServiceImpacts(localPawn);

    std::vector<std::uintptr_t> pawns;
    {
        const std::lock_guard<std::mutex> lock(g_pawnMutex);
        pawns = g_pawns;
    }

    int enemies = 0;
    int projected = 0;
    if (g_settings.enabled) {
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        const ImU32 boxColor = ImGui::ColorConvertFloat4ToU32(
            ImVec4(g_settings.boxColor[0], g_settings.boxColor[1],
                   g_settings.boxColor[2], g_settings.boxColor[3]));
        const ImU32 textColor = ImGui::ColorConvertFloat4ToU32(
            ImVec4(g_settings.textColor[0], g_settings.textColor[1],
                   g_settings.textColor[2], g_settings.textColor[3]));

        for (const std::uintptr_t pawn : pawns) {
            if (!IsPawn(pawn) || pawn == localPawn) continue;
            const std::uint8_t team =
                Read<std::uint8_t>(pawn + offsets::m_iTeamNum);
            if (!g_settings.teammates && team == localTeam) continue;
            ++enemies;

            const Bounds bounds = PlayerBounds(pawn, matrix, width, height);
            if (!bounds.valid) continue;
            ++projected;
            const float boxWidth = bounds.maxX - bounds.minX;
            const float boxHeight = bounds.maxY - bounds.minY;
            if (boxWidth < 2.0f || boxHeight < 5.0f || boxHeight > height) continue;

            const ImVec2 topLeft(bounds.minX, bounds.minY);
            const ImVec2 bottomRight(bounds.maxX, bounds.maxY);
            if (g_settings.box) {
                if (g_settings.boxOutline) {
                    drawList->AddRect(ImVec2(topLeft.x - 1, topLeft.y - 1),
                                      ImVec2(bottomRight.x + 1, bottomRight.y + 1),
                                      IM_COL32(0, 0, 0, 220), 0.0f, 0,
                                      g_settings.boxThickness + 2.0f);
                }
                drawList->AddRect(topLeft, bottomRight, boxColor, 0.0f, 0,
                                  g_settings.boxThickness);
            }

            const int health = Read<int>(pawn + offsets::m_iHealth);
            const float healthRatio =
                static_cast<float>(std::clamp(health, 0, 100)) / 100.0f;
            const float healthTop = bounds.maxY - boxHeight * healthRatio;
            if (g_settings.healthBar) {
                drawList->AddRectFilled(ImVec2(bounds.minX - 8, bounds.minY - 1),
                                        ImVec2(bounds.minX - 3, bounds.maxY + 1),
                                        IM_COL32(0, 0, 0, 220));
                const ImU32 healthColor = ImGui::ColorConvertFloat4ToU32(
                    ImVec4(1.0f - healthRatio, healthRatio, 0.0f, 1.0f));
                drawList->AddRectFilled(ImVec2(bounds.minX - 7, healthTop),
                                        ImVec2(bounds.minX - 4, bounds.maxY),
                                        healthColor);
            }
            if (g_settings.healthText) {
                const std::string text = std::to_string(health) + " HP";
                const ImVec2 size = ImGui::CalcTextSize(text.c_str());
                DrawOutlinedText(drawList,
                                 ImVec2(bounds.minX - 11 - size.x,
                                        healthTop - size.y * 0.5f),
                                 textColor, text);
            }
            if (g_settings.playerName) {
                const std::string name = PlayerName(entityList, pawn);
                const ImVec2 size = ImGui::CalcTextSize(name.c_str());
                DrawOutlinedText(drawList,
                                 ImVec2(bounds.minX + boxWidth * 0.5f - size.x * 0.5f,
                                        bounds.minY - size.y - 3.0f),
                                 textColor, name);
            }
            if (g_settings.weaponName) {
                const std::string weapon = WeaponName(entityList, pawn);
                const ImVec2 size = ImGui::CalcTextSize(weapon.c_str());
                DrawOutlinedText(drawList,
                                 ImVec2(bounds.minX + boxWidth * 0.5f - size.x * 0.5f,
                                        bounds.maxY + 3.0f),
                                 textColor, weapon);
            }
        }
    }

    RenderBulletTracers(matrix, width, height);
    RunTrigger(entityList, localPawn, matrix, width, height);
    UpdateVelocityScoreboard(entityList);
    RenderVelocityScoreboardFallback(entityList, width, height);
    DrawMenu();
    ImGui::Render();

    ID3D11RenderTargetView* oldTarget = nullptr;
    ID3D11DepthStencilView* oldDepth = nullptr;
    g_context->OMGetRenderTargets(1, &oldTarget, &oldDepth);
    g_context->OMSetRenderTargets(1, &target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_context->OMSetRenderTargets(1, &oldTarget, oldDepth);
    if (oldDepth) oldDepth->Release();
    if (oldTarget) oldTarget->Release();
    target->Release();

    static ULONGLONG lastLog = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastLog >= 1000) {
        lastLog = now;
        Log(L"present=1 pawns=" + std::to_wstring(pawns.size()) +
            L" targets=" + std::to_wstring(enemies) +
            L" projected=" + std::to_wstring(projected) +
            L" menu=" +
                std::to_wstring(g_menuOpen.load(std::memory_order_relaxed) ? 1 : 0) +
            L" trigger=" +
                std::to_wstring(g_settings.triggerEnabled ? 1 : 0) +
            L" group=" + std::to_wstring(WeaponGroup(entityList, localPawn)) +
            L" triggerKey=" +
                std::to_wstring(
                    g_triggerKeyHeld.load(std::memory_order_relaxed) ? 1 : 0) +
            L" crosshair=" +
                std::to_wstring(g_crosshairEntity.load(std::memory_order_relaxed)) +
            L" triggerTarget=" +
                std::to_wstring(
                    g_triggerTargetFound.load(std::memory_order_relaxed) ? 1 : 0) +
            L" triggerShots=" +
                std::to_wstring(g_triggerShots.load(std::memory_order_relaxed)) +
            L" impactEvents=" +
                std::to_wstring(
                    g_bulletImpactCount.load(std::memory_order_relaxed)) +
            L" bulletService=" +
                std::to_wstring(g_bulletServiceReady.load(
                                    std::memory_order_relaxed)
                                    ? 1
                                    : 0) +
            L" bulletCount=" +
                std::to_wstring(
                    g_bulletServiceCount.load(std::memory_order_relaxed)) +
            L" tracers=" +
                std::to_wstring(g_settings.bulletTracers ? 1 : 0) +
            L" impactListener=" +
                std::to_wstring(g_bulletImpactRegistered.load(
                                    std::memory_order_relaxed)
                                    ? 1
                                    : 0));
    }
    return g_originalPresent(swapChain, syncInterval, flags);
}

BOOL CALLBACK CloseOldOverlay(HWND window, LPARAM) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId()) return TRUE;
    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(_countof(className)));
    if (lstrcmpW(className, L"Cs2OffsetEspWindow") == 0 ||
        lstrcmpW(className, L"WarCs2OverlayWindow") == 0) {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

bool FindPresentAddress() {
    HWND dummyWindow = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
                                        0, 0, 100, 100, nullptr, nullptr,
                                        GetModuleHandleW(nullptr), nullptr);
    if (!dummyWindow) return false;

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferCount = 1;
    swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.OutputWindow = dummyWindow;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.Windowed = TRUE;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &swapDesc, &swapChain, &device, &featureLevel,
        &context);
    if (FAILED(result)) {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &swapDesc, &swapChain, &device, &featureLevel,
            &context);
    }
    if (SUCCEEDED(result)) {
        void** table = *reinterpret_cast<void***>(swapChain);
        g_presentAddress = table[8];
    }
    if (context) context->Release();
    if (device) device->Release();
    if (swapChain) swapChain->Release();
    DestroyWindow(dummyWindow);
    return g_presentAddress != nullptr;
}


int HexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

std::vector<int> ParseSignature(const char* signature) {
    std::vector<int> bytes;
    for (const char* cursor = signature; *cursor;) {
        if (*cursor == ' ') {
            ++cursor;
            continue;
        }
        if (*cursor == '?') {
            bytes.push_back(-1);
            ++cursor;
            if (*cursor == '?') ++cursor;
            continue;
        }
        const int high = HexDigit(cursor[0]);
        const int low = HexDigit(cursor[1]);
        if (high < 0 || low < 0) break;
        bytes.push_back((high << 4) | low);
        cursor += 2;
    }
    return bytes;
}

void* FindModuleSignature(HMODULE module, const char* signature) {
    if (!module) return nullptr;
    const std::vector<int> pattern = ParseSignature(signature);
    if (pattern.empty()) return nullptr;
    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections;
         ++index, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
        const std::uint8_t* start = base + section->VirtualAddress;
        const std::size_t size = section->Misc.VirtualSize;
        if (size < pattern.size()) continue;
        for (std::size_t offset = 0; offset <= size - pattern.size(); ++offset) {
            bool matches = true;
            for (std::size_t byte = 0; byte < pattern.size(); ++byte) {
                if (pattern[byte] >= 0 &&
                    start[offset + byte] !=
                        static_cast<std::uint8_t>(pattern[byte])) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                return const_cast<std::uint8_t*>(start + offset);
            }
        }
    }
    return nullptr;
}

std::uintptr_t ResolveRelativeCall(void* instruction) {
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(instruction);
    if (!address) return 0;
    const std::int32_t displacement = Read<std::int32_t>(address + 1);
    return address + 5 + displacement;
}

bool InitializeVelocityHudChat() {
    HMODULE client = GetModuleHandleW(kClientName);
    if (!client) return false;
    void* findCall = FindModuleSignature(
        client, "E8 ?? ?? ?? ?? 48 85 C0 48 8D 75 1C");
    void* voiceCall = FindModuleSignature(
        client, "E8 ?? ?? ?? ?? 4C 39 B5 C8 14 00 00");
    g_findHudElement = ResolveRelativeCall(findCall);
    g_setVoiceData = ResolveRelativeCall(voiceCall);
    Log(L"Velocity HUD chat findHud=" +
        std::to_wstring(g_findHudElement ? 1 : 0) + L" setVoice=" +
        std::to_wstring(g_setVoiceData ? 1 : 0));
    return g_findHudElement && g_setVoiceData;
}

constexpr const char* kVelocityScoreboardScript = R"PANORAMA(
(function(){
 if(typeof SClient!=="undefined") SClient=undefined;
 SClient=(function(){var h={};return{register_handler:function(t,c){h[t]=c;},receive:function(m){if(m&&h[m.type])h[m.type](m);}};})();
 SWeaponManager=(function(){
  function scoreboard(){var r=$.GetContextPanel();return r.FindChildTraverse("Scoreboard")||(r.id==="Scoreboard"?r:null);}
  function row(s,x){return s.FindChildTraverse("player-"+x)||s.FindChildTraverse("id-"+x);}
  function size(w){var t=w.type,p=w.path;if(t===1)return p.indexOf("usp_silencer")!==-1?"42px":p.indexOf("deagle")!==-1?"36px":"30px";if(t>=2&&t<=6)return p.indexOf("mac10")!==-1?"30px":"52px";if(t===8)return"22px";return"16px";}
  function icon(parent,w,active){
   var slot=$.CreatePanel("Panel",parent,"wep_"+w.path.replace(/[^a-zA-Z0-9]/g,"_"));
   slot.style.height="fit-children";slot.style.width="fit-children";slot.style.verticalAlign="center";slot.style.margin="0px 1px";
   var img=$.CreatePanel("Image",slot,"img");img.style.verticalAlign="center";img.scaling="stretch-aspect-preserve";
   var path=w.path;if(path.indexOf("file://")!==0){if(path.indexOf("icons/equipment")===-1)path="icons/equipment/"+path;if(path.indexOf(".svg")===-1&&path.indexOf(".vsvg")===-1)path+=".svg";path="file://{images}/"+path;}img.SetImage(path);
   var sz=size(w);img.style.height="16px";img.style.width=sz;slot.style.width=sz;slot.style.opacity=(w.path===active)?"1.0":"0.35";
  }
  return{update:function(x,weapons,active,money){
   var s=scoreboard();if(!s)return;var r=row(s,x);if(!r)return;var n=r.FindChildTraverse("id-sb-name__nameicons");if(!n)return;
   var id="custom-weapons-container-"+x,c=n.FindChildTraverse(id);if((!weapons||weapons.length===0)&&money<0){if(c)c.style.visibility="collapse";return;}
   if(!c){c=$.CreatePanel("Panel",n,id);c.AddClass("custom-weapons-container");c.style.flowChildren="none";c.style.height="20px";c.style.width="fit-children";c.style.verticalAlign="center";c.style.marginLeft="3px";}
   c.style.visibility="visible";c.RemoveAndDeleteChildren();
   var bg=$.CreatePanel("Panel",c,"bg");bg.style.width="100%";bg.style.height="100%";bg.style.backgroundColor="rgba(0,0,0,0.35)";bg.style.borderRadius="3px";
   var content=$.CreatePanel("Panel",c,"content");content.style.flowChildren="right";content.style.height="100%";content.style.width="fit-children";content.style.padding="0px 4px";content.style.verticalAlign="center";
   if(money>=0){var cash=$.CreatePanel("Label",content,"cash");cash.text="$"+money;cash.style.color="#9fce8cff";cash.style.fontWeight="bold";cash.style.fontSize="13px";cash.style.verticalAlign="center";cash.style.marginRight="6px";}
   var primary=[],pistols=[],equip=[];weapons.forEach(function(w){if(w.type===0)return;if(w.type>=2&&w.type<=6)primary.push(w);else if(w.type===1)pistols.push(w);else equip.push(w);});primary.concat(pistols).concat(equip).forEach(function(w){icon(content,w,active);});
  },clear:function(){var s=scoreboard();if(!s)return;var c=s.FindChildrenWithClassTraverse("custom-weapons-container");for(var i=0;i<c.length;i++)c[i].DeleteAsync(0);}};
 })();
 SClient.register_handler("updateWeapons",function(m){if(m&&m.content)SWeaponManager.update(m.content.xuid,m.content.weapons,m.content.active_path,m.content.money);});
 SClient.register_handler("clearWeapons",function(m){if(m&&m.content)SWeaponManager.update(m.content.xuid,[],"",-1);});
})();
)PANORAMA";

bool RunPanoramaScript(const std::string& script) {
    if (!g_panoramaUiEngine || !g_scoreboardPanel) return false;
    void** table = Read<void**>(reinterpret_cast<std::uintptr_t>(g_panoramaUiEngine));
    const std::uintptr_t function = table ? Read<std::uintptr_t>(
        reinterpret_cast<std::uintptr_t>(table) + sizeof(void*) * 77) : 0;
    if (!function) return false;
    using RunScriptFn = void(__fastcall*)(PanoramaUiEngine*, PanoramaPanel*,
                                          const char*, const char*,
                                          std::uint64_t);
    reinterpret_cast<RunScriptFn>(function)(g_panoramaUiEngine,
                                             g_scoreboardPanel,
                                             script.c_str(), nullptr, 0);
    return true;
}

void SetScoreboardStatus(const std::string& status) {
    if (g_scoreboardStatus == status) return;
    g_scoreboardStatus = status;
    Log(L"Velocity TAB: " + std::wstring(status.begin(), status.end()));
}

bool InitializeVelocityScoreboard() {
    if (!g_panoramaInterface) {
        HMODULE panorama = GetModuleHandleW(L"panorama.dll");
        if (!panorama) {
            SetScoreboardStatus("panorama.dll is not loaded");
            return false;
        }
        auto createInterface = panorama ? reinterpret_cast<CreateInterfaceFn>(
                                             GetProcAddress(panorama, "CreateInterface"))
                                        : nullptr;
        if (!createInterface) {
            SetScoreboardStatus("CreateInterface export was not found");
            return false;
        }
        g_panoramaInterface = createInterface("PanoramaUIEngine001", nullptr);
    }
    if (!g_panoramaInterface) {
        SetScoreboardStatus("PanoramaUIEngine001 was not found");
        return false;
    }

    const auto validUiEngine = [](PanoramaUiEngine* candidate) {
        if (!candidate) return false;
        const std::uintptr_t address =
            reinterpret_cast<std::uintptr_t>(candidate);
        const auto panels = Read<PanoramaPanelData*>(address + 0x228);
        const int count = Read<int>(address + 0x230);
        return panels != nullptr && count > 0 && count <= 4096;
    };

    // The interface stores the UI engine at +0x28. Prefer this stable member
    // over calling an old vtable index; the latter returned a different object
    // after the latest game update and produced a bogus count of 8192 panels.
    PanoramaUiEngine* directEngine = Read<PanoramaUiEngine*>(
        reinterpret_cast<std::uintptr_t>(g_panoramaInterface) + 0x28);
    if (validUiEngine(directEngine)) {
        g_panoramaUiEngine = directEngine;
    } else {
        void** panoramaTable = Read<void**>(
            reinterpret_cast<std::uintptr_t>(g_panoramaInterface));
        const std::uintptr_t getEngineAddress = panoramaTable
            ? Read<std::uintptr_t>(
                  reinterpret_cast<std::uintptr_t>(panoramaTable) +
                  sizeof(void*) * 13)
            : 0;
        if (getEngineAddress) {
            using GetUiEngineFn = PanoramaUiEngine*(__fastcall*)(void*);
            PanoramaUiEngine* vfuncEngine =
                reinterpret_cast<GetUiEngineFn>(getEngineAddress)(
                    g_panoramaInterface);
            if (validUiEngine(vfuncEngine)) {
                g_panoramaUiEngine = vfuncEngine;
            }
        }
    }
    if (!validUiEngine(g_panoramaUiEngine)) {
        SetScoreboardStatus("Panorama UI engine layout changed");
        g_panoramaUiEngine = nullptr;
        return false;
    }

    const std::uintptr_t engineAddress =
        reinterpret_cast<std::uintptr_t>(g_panoramaUiEngine);
    PanoramaPanelData* panels = Read<PanoramaPanelData*>(engineAddress + 0x228);
    const int panelCount = Read<int>(engineAddress + 0x230);
    if (panelCount != g_scoreboardLastPanelCount) {
        g_scoreboardLastPanelCount = panelCount;
        SetScoreboardStatus("searching Scoreboard in " +
                            std::to_string(panelCount) + " panels");
    }
    PanoramaPanel* partialMatch = nullptr;
    std::string partialName;
    for (int index = 0; index < panelCount; ++index) {
        const PanoramaPanelData data = Read<PanoramaPanelData>(
            reinterpret_cast<std::uintptr_t>(panels) +
            static_cast<std::uintptr_t>(index) * sizeof(PanoramaPanelData));
        if (!data.panel) continue;
        const std::uintptr_t namePointer = Read<std::uintptr_t>(
            reinterpret_cast<std::uintptr_t>(data.panel) + 0x10);
        const std::string name = ReadString(namePointer, 96);
        if (name == "Scoreboard") {
            g_scoreboardPanel = data.panel;
            break;
        }
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        if (!partialMatch && lower.find("scoreboard") != std::string::npos) {
            partialMatch = data.panel;
            partialName = name;
        }
    }
    if (!g_scoreboardPanel && partialMatch) {
        g_scoreboardPanel = partialMatch;
        SetScoreboardStatus("using Panorama panel " + partialName);
    }
    if (!g_scoreboardPanel) {
        SetScoreboardStatus("Scoreboard panel not created - hold TAB");
        return false;
    }
    g_scoreboardScriptInjected = RunPanoramaScript(kVelocityScoreboardScript);
    SetScoreboardStatus(g_scoreboardScriptInjected
                            ? "Panorama script injected"
                            : "Panorama run_script failed");
    Log(L"Velocity Panorama scoreboard injected=" +
        std::to_wstring(g_scoreboardScriptInjected ? 1 : 0));
    return g_scoreboardScriptInjected;
}

std::string ScoreboardWeaponName(std::uintptr_t weapon, int* type = nullptr) {
    if (!weapon) return {};
    const std::uintptr_t vdata = Read<std::uintptr_t>(
        weapon + offsets::m_nSubclassID + 0x8);
    if (!vdata) return {};
    const std::uintptr_t namePointer = Read<std::uintptr_t>(
        vdata + offsets::m_szWeaponName);
    std::string name = ReadString(namePointer, 64);
    if (name.rfind("weapon_", 0) != 0) return {};
    name.erase(0, 7);
    if (type) *type = Read<int>(vdata + offsets::m_WeaponType);
    return name;
}

ScoreboardWeaponState ReadScoreboardWeaponState(std::uintptr_t entityList,
                                                std::uintptr_t controller) {
    ScoreboardWeaponState state{};
    if (!entityList || !controller) return state;
    const std::uintptr_t moneyServices = Read<std::uintptr_t>(
        controller + offsets::m_pInGameMoneyServices);
    if (moneyServices) {
        const int account = Read<int>(moneyServices + offsets::m_iAccount);
        if (account >= 0 && account <= 1000000) state.money = account;
    }
    const std::uint32_t pawnHandle = Read<std::uint32_t>(
        controller + offsets::m_hPlayerPawn);
    const std::uintptr_t pawn = EntityFromHandle(entityList, pawnHandle);
    if (!pawn || Read<int>(pawn + offsets::m_iHealth) <= 0) return state;

    const std::uintptr_t services = Read<std::uintptr_t>(
        pawn + offsets::m_pWeaponServices);
    if (!services) return state;
    const std::uint32_t activeHandle = Read<std::uint32_t>(
        services + offsets::m_hActiveWeapon);
    state.activeName = ScoreboardWeaponName(
        EntityFromHandle(entityList, activeHandle));
    const std::uintptr_t vector = services + offsets::m_hMyWeapons;
    const int count = std::clamp(Read<int>(vector), 0, 16);
    const std::uintptr_t memory = Read<std::uintptr_t>(vector + 0x8);
    for (int index = 0; memory && index < count; ++index) {
        const std::uint32_t handle = Read<std::uint32_t>(
            memory + static_cast<std::uintptr_t>(index) * 4);
        int type = 0;
        std::string name = ScoreboardWeaponName(
            EntityFromHandle(entityList, handle), &type);
        if (!name.empty() && type != 0) {
            state.weapons.push_back({std::move(name), type});
        }
    }
    return state;
}

void SendScoreboardWeapons(std::uintptr_t entityList,
                           std::uintptr_t controller) {
    const std::uint64_t steamId = Read<std::uint64_t>(
        controller + offsets::m_steamID);
    if (!steamId) return;
    ScoreboardWeaponState state =
        ReadScoreboardWeaponState(entityList, controller);
    const auto sortKey = [](int type) {
        if (type >= 2 && type <= 6) return 0;
        if (type == 1) return 1;
        return 2;
    };
    std::stable_sort(state.weapons.begin(), state.weapons.end(),
                     [&](const ScoreboardWeapon& left,
                         const ScoreboardWeapon& right) {
                         return sortKey(left.type) < sortKey(right.type);
                     });
    const auto cached = g_scoreboardCache.find(steamId);
    if (cached != g_scoreboardCache.end() && cached->second == state) return;
    g_scoreboardCache[steamId] = state;

    std::string weapons = "[";
    for (std::size_t index = 0; index < state.weapons.size(); ++index) {
        if (index) weapons += ',';
        weapons += "{path:\"" + state.weapons[index].name +
                   "\",type:" + std::to_string(state.weapons[index].type) + "}";
    }
    weapons += ']';
    const std::string script =
        "if(typeof(SClient)!=='undefined'){SClient.receive({type:\"updateWeapons\",content:{xuid:\"" +
        std::to_string(steamId) + "\",weapons:" + weapons +
        ",active_path:\"" + state.activeName + "\",money:" +
        std::to_string(state.money) + "}});}";
    RunPanoramaScript(script);
}

void ShutdownVelocityScoreboard() {
    if (g_scoreboardScriptInjected) {
        RunPanoramaScript(
            "if(typeof(SWeaponManager)!=='undefined'){SWeaponManager.clear();}");
    }
    g_scoreboardCache.clear();
    g_scoreboardScriptInjected = false;
    g_scoreboardInitThrottle = 0;
    g_scoreboardUpdateThrottle = 0;
    g_scoreboardTabWasDown = false;
    g_scoreboardTriedForCurrentTab = false;
    g_scoreboardTabOpenedAt = 0;
    g_scoreboardEntityList = 0;
    g_scoreboardPanel = nullptr;
    g_panoramaUiEngine = nullptr;
    g_scoreboardLastPanelCount = -1;
}

void UpdateVelocityScoreboard(std::uintptr_t entityList) {
    if (!g_settings.purchasesOnTab || !entityList) {
        if (g_scoreboardScriptInjected) ShutdownVelocityScoreboard();
        return;
    }
    const bool tabDown = (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
    if (!tabDown) {
        g_scoreboardTabWasDown = false;
        g_scoreboardTriedForCurrentTab = false;
        g_scoreboardTabOpenedAt = 0;
        return;
    }
    if (!g_scoreboardTabWasDown) {
        g_scoreboardTabWasDown = true;
        g_scoreboardTriedForCurrentTab = false;
        g_scoreboardTabOpenedAt = GetTickCount64();
    }
    if (g_scoreboardEntityList && g_scoreboardEntityList != entityList) {
        ShutdownVelocityScoreboard();
        g_scoreboardTabWasDown = true;
        g_scoreboardTabOpenedAt = GetTickCount64();
    }
    g_scoreboardEntityList = entityList;
    if (g_scoreboardScriptInjected && g_scoreboardPanel) {
        const std::uintptr_t namePointer = Read<std::uintptr_t>(
            reinterpret_cast<std::uintptr_t>(g_scoreboardPanel) + 0x10);
        const std::string panelName = LowerText(ReadString(namePointer, 64));
        if (panelName.find("scoreboard") == std::string::npos) {
            ShutdownVelocityScoreboard();
            g_scoreboardEntityList = entityList;
            g_scoreboardTabWasDown = true;
            g_scoreboardTriedForCurrentTab = true;
            g_scoreboardTabOpenedAt = GetTickCount64();
        }
    }
    if (!g_scoreboardScriptInjected) {
        // One delayed attempt per TAB opening. Never rescan the whole Panorama
        // panel table on a frame timer: that was the source of periodic stalls.
        if (!g_scoreboardTriedForCurrentTab &&
            GetTickCount64() - g_scoreboardTabOpenedAt >= 180) {
            g_scoreboardTriedForCurrentTab = true;
            InitializeVelocityScoreboard();
        }
        if (!g_scoreboardScriptInjected) return;
    }
    if (++g_scoreboardUpdateThrottle % 15 != 0) return;

    const std::uintptr_t controllerChunk = Chunk(entityList, 0);
    for (std::uint32_t index = 1; index <= 64 && controllerChunk; ++index) {
        const std::uintptr_t controller = Read<std::uintptr_t>(
            controllerChunk + offsets::entityIdentityStride * index);
        if (!controller) continue;
        SendScoreboardWeapons(entityList, controller);
    }
}

void RenderVelocityScoreboardFallback(std::uintptr_t entityList, float width,
                                      float height) {
    if (!g_settings.purchasesOnTab || !entityList ||
        (GetAsyncKeyState(VK_TAB) & 0x8000) == 0 ||
        g_menuOpen.load(std::memory_order_relaxed)) {
        return;
    }

    struct Row {
        std::string name;
        std::string weapons;
        int team{};
        int money{-1};
        int slot{};
        bool needsOverlay{};
    };
    static std::vector<Row> rows;
    static ULONGLONG nextRefresh = 0;
    const ULONGLONG now = GetTickCount64();
    if (now >= nextRefresh) {
        nextRefresh = now + 250;
        rows.clear();
        const std::uintptr_t controllerChunk = Chunk(entityList, 0);
        for (std::uint32_t index = 1; index <= 64 && controllerChunk; ++index) {
            const std::uintptr_t controller = Read<std::uintptr_t>(
                controllerChunk + offsets::entityIdentityStride * index);
            if (!controller) continue;
            const std::uint32_t pawnHandle = Read<std::uint32_t>(
                controller + offsets::m_hPlayerPawn);
            const std::uintptr_t pawn = EntityFromHandle(entityList, pawnHandle);
            if (!pawn || !IsPawn(pawn)) continue;
            const bool bot = Read<std::uint64_t>(
                                 controller + offsets::m_steamID) == 0;
            const ScoreboardWeaponState state =
                ReadScoreboardWeaponState(entityList, controller);
            std::string weaponText;
            for (const ScoreboardWeapon& weapon : state.weapons) {
                if (!weaponText.empty()) weaponText += "  ";
                weaponText += FriendlyWeapon(weapon.name);
            }
            rows.push_back({PlayerName(entityList, pawn),
                            std::move(weaponText),
                            Read<std::uint8_t>(pawn + offsets::m_iTeamNum),
                            state.money, static_cast<int>(index),
                            !g_scoreboardScriptInjected || bot});
        }
        std::stable_sort(rows.begin(), rows.end(),
                         [](const Row& left, const Row& right) {
                             if (left.team != right.team)
                                 return left.team > right.team;
                             return left.slot < right.slot;
                         });
    }
    if (rows.empty()) return;

    // Draw directly into the native scoreboard columns. This is deliberately
    // borderless: it looks like part of TAB instead of a second floating menu.
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = std::clamp(height * 0.0130f, 13.0f, 19.0f);
    const float rowStep = height * 0.0217f;
    const float weaponRight = width * 0.538f;
    const float moneyX = width * 0.553f;
    int ctRow = 0;
    int tRow = 0;
    for (const Row& row : rows) {
        if (!row.needsOverlay || (row.team != 2 && row.team != 3)) continue;
        const int teamRow = row.team == 3 ? ctRow++ : tRow++;
        if (teamRow >= 5) continue;
        const float rowY = (row.team == 3 ? height * 0.3765f
                                           : height * 0.5965f) +
                           rowStep * static_cast<float>(teamRow);
        if (!row.weapons.empty()) {
            const ImVec2 weaponSize = font->CalcTextSizeA(
                fontSize, std::numeric_limits<float>::max(), 0.0f,
                row.weapons.c_str());
            const ImVec2 position(
                std::max(width * 0.365f, weaponRight - weaponSize.x), rowY);
            draw->AddText(font, fontSize,
                          ImVec2(position.x + 1.0f, position.y + 1.0f),
                          IM_COL32(0, 0, 0, 210), row.weapons.c_str());
            draw->AddText(font, fontSize, position,
                          IM_COL32(205, 211, 219, 235),
                          row.weapons.c_str());
        }
        if (row.money >= 0) {
            const std::string money = "$" + std::to_string(row.money);
            draw->AddText(font, fontSize, ImVec2(moneyX + 1.0f, rowY + 1.0f),
                          IM_COL32(0, 0, 0, 210), money.c_str());
            draw->AddText(font, fontSize, ImVec2(moneyX, rowY),
                          IM_COL32(159, 206, 140, 255), money.c_str());
        }
    }
}

bool InitializeEngineCommand() {
    HMODULE engine = GetModuleHandleW(L"engine2.dll");
    if (!engine) return false;
    auto createInterface = reinterpret_cast<CreateInterfaceFn>(
        GetProcAddress(engine, "CreateInterface"));
    if (!createInterface) return false;
    g_engineClient = createInterface("Source2EngineToClient001", nullptr);
    constexpr const char* signature =
        "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 56 41 57 "
        "48 81 EC 70 01 00 00 0F 29 70 D8 41 0F B6 E9 8D 42 FC 4D 8B "
        "F8 8B FA 4C 8B F1 83 F8 01 76 3E BA FF FF FF FF 48 8D 0D ?? "
        "?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 75 0B";
    g_engineClientCmd = reinterpret_cast<EngineClientCmdFn>(
        FindModuleSignature(engine, signature));
    return g_engineClient && g_engineClientCmd;
}

bool InitializeGameLogEvents() {
    HMODULE client = GetModuleHandleW(kClientName);
    if (!client) return false;

    auto* managerInstruction = static_cast<std::uint8_t*>(FindModuleSignature(
        client, "48 8B 0D ?? ?? ?? ?? 49 8B F0 49 8B D9"));
    auto* floatCall = static_cast<std::uint8_t*>(FindModuleSignature(
        client, "E8 ?? ?? ?? ?? 0F 28 D8 89 5C 24 20"));
    auto* controllerVtable = static_cast<std::uint8_t*>(FindModuleSignature(
        client, "48 8D 05 ?? ?? ?? ?? 4D 8B F8 48 89 01"));

    // Velocity's interface lookup checks every loaded module.  In the current
    // CS2 build GAMEEVENTSMANAGER002 is registered by client.dll, not engine2.
    constexpr const wchar_t* managerModules[] = {
        L"client.dll", L"engine2.dll", L"matchmaking.dll"
    };
    for (const wchar_t* moduleName : managerModules) {
        HMODULE module = GetModuleHandleW(moduleName);
        auto createInterface = module ? reinterpret_cast<CreateInterfaceFn>(
                                           GetProcAddress(module,
                                                          "CreateInterface"))
                                      : nullptr;
        if (!createInterface) continue;
        g_gameEventManager = reinterpret_cast<std::uintptr_t>(
            createInterface("GAMEEVENTSMANAGER002", nullptr));
        if (g_gameEventManager) break;
    }
    if (!g_gameEventManager && managerInstruction) {
        const std::int32_t managerDisplacement =
            *reinterpret_cast<std::int32_t*>(managerInstruction + 3);
        const std::uintptr_t managerStorage =
            reinterpret_cast<std::uintptr_t>(managerInstruction + 7) +
            managerDisplacement;
        g_gameEventManager = Read<std::uintptr_t>(managerStorage);
    }
    if (floatCall) {
        const std::int32_t floatDisplacement =
            *reinterpret_cast<std::int32_t*>(floatCall + 1);
        g_gameEventGetFloat = reinterpret_cast<GameEventGetFloatFn>(
            reinterpret_cast<std::uintptr_t>(floatCall + 5) +
            floatDisplacement);
    }

    if (controllerVtable) {
        const std::int32_t controllerDisplacement =
            *reinterpret_cast<std::int32_t*>(controllerVtable + 3);
        const std::uintptr_t eventVtable =
            reinterpret_cast<std::uintptr_t>(controllerVtable + 7) +
            controllerDisplacement;
        g_gameEventGetController = reinterpret_cast<GameEventGetControllerFn>(
            Read<std::uintptr_t>(eventVtable + 0x80));
    }

    Log(L"game log events manager=" + std::to_wstring(g_gameEventManager) +
        L" getFloat=" +
        std::to_wstring(g_gameEventGetFloat ? 1 : 0) +
        L" getController=" +
        std::to_wstring(g_gameEventGetController ? 1 : 0));
    if (!g_gameEventManager) return false;

    void** table = Read<void**>(g_gameEventManager);
    const std::uintptr_t addListenerAddress = Read<std::uintptr_t>(
        reinterpret_cast<std::uintptr_t>(table) + sizeof(void*) * 3);
    if (!table || !addListenerAddress) return false;
    using AddListenerFn = bool(__fastcall*)(std::uintptr_t, void*, const char*,
                                            bool);
    const auto addListener =
        reinterpret_cast<AddListenerFn>(addListenerAddress);
    constexpr const char* events[] = {
        "item_purchase", "round_start", "vote_started", "vote_cast",
        "vote_passed", "vote_failed", "player_disconnect", "round_end"
    };
    int registered = 0;
    for (std::size_t index = 0; index < std::size(events); ++index) {
        GameLogListenerEntry& entry = g_gameLogListeners[index];
        entry.name = events[index];
        entry.vtable[0] = nullptr;
        entry.vtable[1] = reinterpret_cast<void*>(&OnGameLogEvent);
        entry.vtable[2] = reinterpret_cast<void*>(&BulletImpactDebugId);
        entry.listener.vtable = entry.vtable;
        entry.listener.debugId = static_cast<int>(index + 1);
        entry.registered = addListener(g_gameEventManager, &entry.listener,
                                       entry.name, false);
        if (entry.registered) {
            ++registered;
        }
    }
    g_gameLogListenerCount.store(registered, std::memory_order_relaxed);
    g_gameLogRegistered.store(
        registered == static_cast<int>(std::size(events)),
        std::memory_order_relaxed);
    Log(L"game log listeners registered=" + std::to_wstring(registered) +
        L"/" + std::to_wstring(std::size(events)));
    return registered > 0;
}

void ShutdownGameLogEvents() {
    if (g_gameLogListenerCount.load(std::memory_order_relaxed) > 0 &&
        g_gameEventManager) {
        void** table = Read<void**>(g_gameEventManager);
        const std::uintptr_t removeListenerAddress = Read<std::uintptr_t>(
            reinterpret_cast<std::uintptr_t>(table) + sizeof(void*) * 5);
        if (table && removeListenerAddress) {
            using RemoveListenerFn = void(__fastcall*)(std::uintptr_t, void*);
            const auto removeListener =
                reinterpret_cast<RemoveListenerFn>(removeListenerAddress);
            for (GameLogListenerEntry& entry : g_gameLogListeners) {
                if (entry.registered) {
                    removeListener(g_gameEventManager, &entry.listener);
                    entry.registered = false;
                }
            }
        }
    }
    g_gameLogRegistered = false;
    g_gameLogListenerCount = 0;
    g_bulletImpactRegistered = false;
    g_gameEventManager = 0;
    g_gameEventGetFloat = nullptr;
    g_gameEventGetController = nullptr;
    {
        const std::lock_guard<std::mutex> lock(g_tracerMutex);
        g_bulletTracers.clear();
    }
    {
        const std::lock_guard<std::mutex> lock(g_gameLogMutex);
        g_roundPurchases.clear();
        g_gameLogs.clear();
        g_activeVoteTitle.clear();
        g_activeVoteReason.clear();
    }
}

bool InstallPresentHook() {
    if (!FindPresentAddress()) return false;
    if (MH_Initialize() != MH_OK) return false;
    if (MH_CreateHook(g_presentAddress, reinterpret_cast<void*>(&HookPresent),
                      reinterpret_cast<void**>(&g_originalPresent)) != MH_OK) {
        MH_Uninitialize();
        return false;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        MH_RemoveHook(g_presentAddress);
        MH_Uninitialize();
        return false;
    }
    Log(L"DirectX Present hook enabled");
    return true;
}

DWORD WINAPI MainThread(void* parameter) {
    const HMODULE module = static_cast<HMODULE>(parameter);
    OpenLog();
    LoadSettings();
    Log(L"=== internal renderer started ===");

    HANDLE singleton = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        Log(L"another internal instance is active");
        if (singleton) CloseHandle(singleton);
        if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
        FreeLibraryAndExitThread(module, 0);
    }
    EnumWindows(CloseOldOverlay, 0);

    for (int i = 0; i < 300 && !g_client; ++i) {
        g_client = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(kClientName));
        if (!g_client) Sleep(100);
    }
    if (InitializeInputSystem()) {
        Log(L"InputSystemVersion001 initialized");
    } else {
        Log(L"warning: InputSystemVersion001 was not found");
    }
    if (InitializeEngineCommand()) {
        Log(L"Velocity engine_client_cmd initialized");
    } else {
        Log(L"warning: Velocity engine_client_cmd signature not found");
    }
    if (!InitializeVelocityHudChat()) {
        Log(L"warning: Velocity HUD chat signatures not found");
    }
    if (InitializeGameLogEvents()) {
        Log(L"purchase and vote logs initialized");
    } else {
        Log(L"warning: purchase and vote event listener unavailable");
    }
    Log(L"BulletServices tracer source enabled");
    if (!g_client || !InstallPresentHook()) {
        Log(L"failed to install DirectX 11 Present hook");
        ShutdownVelocityScoreboard();
        ShutdownGameLogEvents();
        ReleaseMutex(singleton);
        CloseHandle(singleton);
        if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
        FreeLibraryAndExitThread(module, 1);
    }
    Log(L"DirectX 11 Present hook installed");

    ULONGLONG lastScan = 0;
    while (!g_unloadRequested.load(std::memory_order_relaxed) &&
           (GetAsyncKeyState(VK_END) & 0x8000) == 0) {
        if ((GetAsyncKeyState(VK_INSERT) & 1) != 0) {
            SetMenuOpen(!g_menuOpen.load(std::memory_order_relaxed));
        }
        const ULONGLONG now = GetTickCount64();
        if (now - lastScan >= 500) {
            lastScan = now;
            const std::uintptr_t entityList =
                Read<std::uintptr_t>(g_client + offsets::dwEntityList);
            const std::uintptr_t localPawn =
                Read<std::uintptr_t>(g_client + offsets::dwLocalPlayerPawn);
            const int highest =
                Read<int>(entityList + offsets::highestEntityIndex);
            if (entityList && localPawn) {
                std::vector<std::uintptr_t> pawns;
                int chunksRead = 0;
                bool localFound = false;
                ScanPawns(entityList, highest, pawns, chunksRead, localPawn,
                          localFound);
                {
                    const std::lock_guard<std::mutex> lock(g_pawnMutex);
                    g_pawns.swap(pawns);
                    g_chunksRead = chunksRead;
                    g_localFound = localFound;
                }
            }
        }
        Sleep(10);
    }

    SaveSettings();
    SetMenuOpen(false);
    ReleasePrimaryAttack(true);
    ShutdownVelocityScoreboard();
    ShutdownGameLogEvents();
    g_running.store(false, std::memory_order_relaxed);
    MH_DisableHook(MH_ALL_HOOKS);
    MH_RemoveHook(g_presentAddress);
    MH_Uninitialize();
    Sleep(50);
    ReleaseRenderer();
    Log(L"=== internal renderer stopped ===");
    ReleaseMutex(singleton);
    CloseHandle(singleton);
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    FreeLibraryAndExitThread(module, 0);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, MainThread, module, 0, nullptr);
        if (!thread) return FALSE;
        CloseHandle(thread);
    }
    return TRUE;
}
