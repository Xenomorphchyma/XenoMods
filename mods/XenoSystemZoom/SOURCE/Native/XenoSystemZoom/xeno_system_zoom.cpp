#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include "xeno_plugin_api.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace {
const wchar_t* const kPluginId = L"XenoSystemZoom";
const wchar_t* const kVersion = L"0.3.0";
unsigned char* g_base = nullptr;
size_t g_imageSize = 0;
unsigned char* g_boundsSite = nullptr;
void* g_boundsTrampoline = nullptr;
unsigned char* g_transformSite = nullptr;
void* g_transformTrampoline = nullptr;
XenoHostLogFn g_hostLog = nullptr;
std::atomic<int> g_zoomPercent(100);
int g_stepPercent = 18;
int g_minPercent = 50;
int g_maxPercent = 250;
int g_centerX = 512;
int g_centerY = 384;
bool g_enabled = true;
HWND g_window = nullptr;
WNDPROC g_originalWndProc = nullptr;
volatile LONG g_lastLoggedZoom = 100;
void* g_boundsSelf = nullptr;
void* g_boundsOutput = nullptr;
void* g_transformSelf = nullptr;
volatile LONG g_transformLogged = 0;

void Log(const std::wstring& message) { if (g_hostLog) g_hostLog(kPluginId, message.c_str()); }
std::wstring Hex(std::uintptr_t value) { std::wostringstream out; out << L"0x" << std::hex << std::uppercase << value; return out.str(); }

struct ImageSection { unsigned char* begin; size_t size; };
bool ReadImage(void* module, std::vector<ImageSection>& sections) {
    auto* base = static_cast<unsigned char*>(module);
    if (!base) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
    g_base = base; g_imageSize = nt->OptionalHeader.SizeOfImage;
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
        size_t size = std::max<size_t>(section->Misc.VirtualSize, section->SizeOfRawData);
        if (section->VirtualAddress >= g_imageSize || size > g_imageSize - section->VirtualAddress) return false;
        sections.push_back({base + section->VirtualAddress, size});
    }
    return !sections.empty();
}
unsigned char* FindMasked(const std::vector<ImageSection>& sections, const unsigned char* bytes,
                          const unsigned char* mask, size_t length, int* matches) {
    unsigned char* result = nullptr; int count = 0;
    for (const ImageSection& section : sections) {
        if (section.size < length) continue;
        for (size_t i = 0; i <= section.size - length; ++i) {
            bool equal = true;
            for (size_t j = 0; j < length; ++j) {
                if (mask[j] && section.begin[i + j] != bytes[j]) { equal = false; break; }
            }
            if (equal) { if (!result) result = section.begin + i; ++count; }
        }
    }
    if (matches) *matches = count;
    return result;
}
bool InImage(const void* pointer, size_t length = 1) {
    const std::uintptr_t p = reinterpret_cast<std::uintptr_t>(pointer);
    const std::uintptr_t b = reinterpret_cast<std::uintptr_t>(g_base);
    return p >= b && length <= g_imageSize && p <= b + g_imageSize - length;
}

bool DiscoverSites(void* module, std::wstring& error) {
    std::vector<ImageSection> sections;
    if (!ReadImage(module, sections)) { error = L"invalid PE32 image"; return false; }
    // TStarFieldGI::GetBounds. The method writes left/top/right/bottom to edx.
    static const unsigned char boundsBytes[] = {
        0x55,0x8B,0xEC,0x83,0xC4,0xF8,0x89,0x55,0xF8,0x89,0x45,0xFC,
        0x8B,0x45,0xFC,0x8B,0x40,0x20,0x8B,0x55,0xFC,0x2B,0x42,0x30
    };
    static const unsigned char boundsMask[] = {
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
    };
    static const unsigned char transformBytes[] = {
        0x8B,0x45,0xFC,0x8B,0x88,0xD4,0x00,0x00,0x00,0xB2,0x01,0xA1,
        0x5C,0x56,0x4B,0x00,0xE8,0xA4,0x33,0xE4,0xFF,0x8B,0x55,0xFC,
        0x89,0x82,0x08,0x01,0x00,0x00,0x68,0x00,0x00,0x24,0x40,0x6A,
        0x00,0x8B,0x45,0xFC,0x8B,0x80,0x08,0x01,0x00,0x00,0x8B,0x10,
        0xFF,0x52,0x10,0xA1
    };
    static const unsigned char transformMask[] = {
        1,1,0,1,1,0,0,0,0,1,0,1,0,0,0,0,1,0,0,0,0,1,1,0,1,1,
        0,0,0,0,1,0,0,0,0,1,0,1,1,0,1,1,0,0,0,0,1,1,1,1,0,1
    };
    int boundsMatches = 0, transformMatches = 0;
    g_boundsSite = FindMasked(sections, boundsBytes, boundsMask, sizeof(boundsBytes), &boundsMatches);
    g_transformSite = FindMasked(sections, transformBytes, transformMask, sizeof(transformBytes), &transformMatches);
    if (!g_boundsSite || boundsMatches != 1 || !g_transformSite || transformMatches != 1 ||
        !InImage(g_boundsSite, 6) || !InImage(g_transformSite, 9)) {
        error = L"StarMap render signatures are not unique"; return false;
    }
    return true;
}

void* MakeTrampoline(unsigned char* site, size_t length) {
    if (!site || length < 5) return nullptr;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr, length + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return nullptr;
    std::memcpy(trampoline, site, length);
    const std::intptr_t relative = (site + length) - (trampoline + length + 5);
    if (relative < INT32_MIN || relative > INT32_MAX) { VirtualFree(trampoline, 0, MEM_RELEASE); return nullptr; }
    trampoline[length] = 0xE9;
    const std::int32_t encoded = static_cast<std::int32_t>(relative);
    std::memcpy(trampoline + length + 1, &encoded, sizeof(encoded));
    FlushInstructionCache(GetCurrentProcess(), trampoline, length + 5);
    return trampoline;
}
bool PatchJump(unsigned char* site, void* destination, size_t length) {
    if (!site || !destination || length < 5) return false;
    const std::intptr_t relative = static_cast<unsigned char*>(destination) - (site + 5);
    if (relative < INT32_MIN || relative > INT32_MAX) return false;
    DWORD oldProtection = 0;
    if (!VirtualProtect(site, length, PAGE_EXECUTE_READWRITE, &oldProtection)) return false;
    site[0] = 0xE9;
    const std::int32_t encoded = static_cast<std::int32_t>(relative);
    std::memcpy(site + 1, &encoded, sizeof(encoded));
    for (size_t i = 5; i < length; ++i) site[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), site, length);
    DWORD ignored = 0; VirtualProtect(site, length, oldProtection, &ignored);
    return true;
}
int ClampInt(int value, int minimum, int maximum) { return std::max(minimum, std::min(maximum, value)); }
int ClampCoord(long long value) { return static_cast<int>(std::max<long long>(-1000000, std::min<long long>(1000000, value))); }

void ApplyBounds(void* object, void* output) {
    if (!object || !output || !g_enabled || g_zoomPercent.load() == 100) return;
    auto* values = static_cast<LONG*>(output);
        const LONG left = values[0];
        const LONG top = values[1];
        const LONG right = values[2];
        const LONG bottom = values[3];
        if (left < -100000 || left > 100000 || top < -100000 || top > 100000 ||
            right < -100000 || right > 100000 || bottom < -100000 || bottom > 100000 ||
            right < left || bottom < top) return;
        const long long scale = g_zoomPercent.load();
        auto scaleCoord = [&](LONG coordinate, int center) {
            return ClampCoord(static_cast<long long>(center) + (static_cast<long long>(coordinate) - center) * scale / 100);
        };
        values[0] = scaleCoord(left, g_centerX);
        values[1] = scaleCoord(top, g_centerY);
        values[2] = scaleCoord(right, g_centerX);
        values[3] = scaleCoord(bottom, g_centerY);
    if (InterlockedExchange(&g_transformLogged, 1) == 0)
        Log(L"render_transform=active bounds_output=edx zoom=" + std::to_wstring(g_zoomPercent.load()));
}

void ApplyTransform(void* object) {
    if (!object || !g_enabled || g_zoomPercent.load() == 100) return;
    auto* bytes = static_cast<unsigned char*>(object);
    const float originX = *reinterpret_cast<float*>(bytes + 0xF0);
    const float originY = *reinterpret_cast<float*>(bytes + 0xF4);
    const float spanX = *reinterpret_cast<float*>(bytes + 0xF8);
    const float spanY = *reinterpret_cast<float*>(bytes + 0xFC);
    if (!std::isfinite(originX) || !std::isfinite(originY) || !std::isfinite(spanX) || !std::isfinite(spanY) ||
        spanX <= 1.0f || spanY <= 1.0f || spanX > 1000000000.0f || spanY > 1000000000.0f) return;
    const float factor = 100.0f / static_cast<float>(g_zoomPercent.load());
    const float centerX = originX + spanX * 0.5f;
    const float centerY = originY + spanY * 0.5f;
    const float newSpanX = spanX * factor;
    const float newSpanY = spanY * factor;
    *reinterpret_cast<float*>(bytes + 0xF0) = centerX - newSpanX * 0.5f;
    *reinterpret_cast<float*>(bytes + 0xF4) = centerY - newSpanY * 0.5f;
    *reinterpret_cast<float*>(bytes + 0xF8) = newSpanX;
    *reinterpret_cast<float*>(bytes + 0xFC) = newSpanY;
    if (InterlockedExchange(&g_transformLogged, 1) == 0) {
        std::wostringstream message;
        message << L"camera_transform=active zoom=" << g_zoomPercent.load()
                << L" span=" << spanX << L"x" << spanY
                << L" -> " << newSpanX << L"x" << newSpanY;
        Log(message.str());
    }
}

extern "C" void BoundsAfterOriginal();
extern "C" void BoundsStub();
extern "C" void TransformAfterOriginal();
extern "C" void TransformStub();
__declspec(naked) void BoundsStub() {
    __asm {
        mov dword ptr [g_boundsSelf], eax
        mov dword ptr [g_boundsOutput], edx
        push offset BoundsAfterOriginal
        jmp dword ptr [g_boundsTrampoline]
    }
}
__declspec(naked) void BoundsAfterOriginal() {
    __asm {
        pushfd
        pushad
        mov eax, dword ptr [g_boundsOutput]
        push eax
        mov eax, dword ptr [g_boundsSelf]
        push eax
        call ApplyBounds
        add esp, 8
        popad
        popfd
        ret
    }
}
__declspec(naked) void TransformStub() {
    __asm {
        mov dword ptr [g_transformSelf], eax
        pushfd
        pushad
        mov eax, dword ptr [g_transformSelf]
        push eax
        call ApplyTransform
        add esp, 4
        popad
        popfd
        push offset TransformAfterOriginal
        jmp dword ptr [g_transformTrampoline]
    }
}
__declspec(naked) void TransformAfterOriginal() {
    __asm { ret }
}
bool PatchRender() {
    g_boundsTrampoline = MakeTrampoline(g_boundsSite, 6);
    g_transformTrampoline = MakeTrampoline(g_transformSite, 9);
    if (!g_boundsTrampoline || !g_transformTrampoline) return false;
    return PatchJump(g_boundsSite, reinterpret_cast<void*>(&BoundsStub), 6) &&
           PatchJump(g_transformSite, reinterpret_cast<void*>(&TransformStub), 9);
}

void SetZoomPercent(int value) {
    value = ClampInt(value, g_minPercent, g_maxPercent);
    const int previous = g_zoomPercent.exchange(value);
    if (previous == value) return;
    if (InterlockedExchange(&g_lastLoggedZoom, value) != value)
        Log(std::wstring(L"zoom_percent=") + std::to_wstring(value));
    if (g_window) InvalidateRect(g_window, nullptr, FALSE);
}
void ChangeZoom(int direction) {
    const int delta = std::max(1, g_stepPercent);
    SetZoomPercent(g_zoomPercent.load() + (direction > 0 ? delta : -delta));
}
BOOL CALLBACK FindGameWindow(HWND window, LPARAM data) {
    DWORD processId = 0; GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId() || GetWindow(window, GW_OWNER) != nullptr) return TRUE;
    RECT rect = {}; if (!GetClientRect(window, &rect) || rect.right < 640 || rect.bottom < 480) return TRUE;
    *reinterpret_cast<HWND*>(data) = window; return FALSE;
}
LRESULT CALLBACK ZoomWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    WNDPROC original = g_originalWndProc;
    LRESULT result = original ? CallWindowProcW(original, window, message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    if (!g_enabled) return result;
    if (message == WM_MOUSEWHEEL) ChangeZoom(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1 : -1);
    else if (message == WM_KEYUP) {
        if (wParam == VK_HOME || wParam == 'R' || wParam == VK_NUMPAD0) SetZoomPercent(100);
        else if (wParam == VK_ADD || wParam == '=') ChangeZoom(1);
        else if (wParam == VK_SUBTRACT || wParam == '-') ChangeZoom(-1);
    }
    return result;
}
DWORD WINAPI WindowInstallerThread(void*) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        HWND window = nullptr; EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&window));
        if (window) {
            WNDPROC current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
            if (current) {
                SetLastError(0);
                WNDPROC previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ZoomWndProc)));
                if (previous || GetLastError() == 0) { g_window = window; g_originalWndProc = previous ? previous : current; Log(L"window_hook=installed"); return 0; }
            }
        }
        Sleep(50);
    }
    Log(L"window_hook=skipped reason=window_not_found"); return 0;
}
}

extern "C" BOOL WINAPI XenoPlugin_Query(XenoPluginInfoV1* info) {
    if (!info || info->size < XENO_PLUGIN_INFO_V1_BASE_SIZE) return FALSE;
    info->requiredHostApi = XENO_NATIVE_HOST_API_V1;
    lstrcpynW(info->id, L"XenoSystemZoom", static_cast<int>(_countof(info->id)));
    lstrcpynW(info->version, L"0.3.0", static_cast<int>(_countof(info->version)));
    lstrcpynW(info->description, L"Adjustable StarMap system-world zoom", static_cast<int>(_countof(info->description)));
    if (info->size >= sizeof(XenoPluginInfoV1)) info->exclusiveCapabilities = 0;
    return TRUE;
}
extern "C" DWORD WINAPI XenoPlugin_Initialize(const XenoPluginHostV1* host) {
    if (!host || host->apiVersion < XENO_NATIVE_HOST_API_V1 || host->size < sizeof(XenoPluginHostV1) || !host->gameModule) return 1;
    g_hostLog = host->log;
    g_enabled = host->configGetBool ? host->configGetBool(host->configPath, L"Zoom", L"Enabled", TRUE) != FALSE : true;
    if (!g_enabled) { Log(L"runtime=disabled_by_ini"); return 0; }
    auto getInt = [&](const wchar_t* key, int fallback, int minimum, int maximum) { return host->configGetInt ? host->configGetInt(host->configPath, L"Zoom", key, fallback, minimum, maximum) : fallback; };
    g_stepPercent = getInt(L"StepPercent", 18, 1, 100);
    g_minPercent = getInt(L"MinPercent", 50, 10, 100);
    g_maxPercent = getInt(L"MaxPercent", 250, 100, 1000);
    g_centerX = getInt(L"CenterX", 512, 0, 10000);
    g_centerY = getInt(L"CenterY", 384, 0, 10000);
    g_zoomPercent.store(getInt(L"DefaultPercent", 100, g_minPercent, g_maxPercent));
    std::wstring error;
    if (!DiscoverSites(host->gameModule, error)) { Log(L"runtime=skipped reason=" + error); return 2; }
    if (!PatchRender()) { Log(L"runtime=failed reason=patch_failed"); return 3; }
    Log(L"runtime=installed bounds_hook=" + Hex(reinterpret_cast<std::uintptr_t>(g_boundsSite)) + L" camera_hook=" + Hex(reinterpret_cast<std::uintptr_t>(g_transformSite)) + L" range=" + std::to_wstring(g_minPercent) + L"-" + std::to_wstring(g_maxPercent));
    HANDLE thread = CreateThread(nullptr, 0, WindowInstallerThread, nullptr, 0, nullptr); if (thread) CloseHandle(thread);
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) { if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance); return TRUE; }
