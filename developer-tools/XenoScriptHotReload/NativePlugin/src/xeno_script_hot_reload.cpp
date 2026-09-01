#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "runtime_scan.h"
#include "xeno_plugin_api.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr wchar_t kPluginId[] = L"xeno.script_hot_reload";
    constexpr UINT kReloadMessage = WM_APP + 0x358;
    constexpr WPARAM kProbe = 1;
    constexpr WPARAM kChangedFiles = 2;
    constexpr WPARAM kManualAll = 3;

    struct FileStamp
    {
        ULONGLONG writeTime = 0;
        ULONGLONG size = 0;

        bool operator==(const FileStamp& other) const
        {
            return writeTime == other.writeTime && size == other.size;
        }
    };

    struct Settings
    {
        bool autoReload = true;
        bool reloadOnGalaxyLoad = false;
        int pollMilliseconds = 800;
        int hotkey = VK_F10;
        std::wstring watchRoot;
        std::wstring scriptPrefix = L"Mod_";
        std::vector<std::wstring> scriptNames;
    };

    xsr::RuntimePoints g_runtime;
    Settings g_settings;
    XenoHostLogFn g_log = nullptr;
    SRWLOCK g_pendingLock = SRWLOCK_INIT;
    std::vector<std::wstring> g_pending;
    std::atomic<HWND> g_window(nullptr);
    WNDPROC g_originalWindowProc = nullptr;
    void* g_candidateGalaxy = nullptr;
    void* g_processedGalaxy = nullptr;
    int g_candidateCount = -1;
    int g_stableProbes = 0;
    bool g_initialSnapshotReady = false;
    std::map<std::wstring, FileStamp> g_snapshot;

    void Log(const std::wstring& message)
    {
        if (g_log)
        {
            g_log(kPluginId, message.c_str());
        }
    }

    std::wstring Lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return value;
    }

    bool EqualInsensitive(const std::wstring& left, const std::wstring& right)
    {
        return _wcsicmp(left.c_str(), right.c_str()) == 0;
    }

    bool StartsWithInsensitive(const std::wstring& value, const std::wstring& prefix)
    {
        return prefix.empty() ||
            (value.size() >= prefix.size() &&
             _wcsnicmp(value.c_str(), prefix.c_str(), prefix.size()) == 0);
    }

    std::vector<std::wstring> SplitNames(const std::wstring& value)
    {
        std::vector<std::wstring> result;
        size_t start = 0;
        while (start <= value.size())
        {
            const size_t end = value.find(L';', start);
            std::wstring item = value.substr(
                start,
                end == std::wstring::npos ? std::wstring::npos : end - start);
            const size_t first = item.find_first_not_of(L" \t\r\n");
            const size_t last = item.find_last_not_of(L" \t\r\n");
            if (first != std::wstring::npos)
            {
                result.push_back(item.substr(first, last - first + 1));
            }
            if (end == std::wstring::npos)
            {
                break;
            }
            start = end + 1;
        }
        return result;
    }

    bool IsAllowed(const std::wstring& scriptName)
    {
        if (!g_settings.scriptNames.empty())
        {
            for (const auto& configured : g_settings.scriptNames)
            {
                if (EqualInsensitive(scriptName, configured))
                {
                    return true;
                }
            }
            return false;
        }
        return StartsWithInsensitive(scriptName, g_settings.scriptPrefix);
    }

    void QueueScript(const std::wstring& scriptName)
    {
        if (scriptName.empty() || !IsAllowed(scriptName))
        {
            return;
        }
        AcquireSRWLockExclusive(&g_pendingLock);
        const auto found = std::find_if(g_pending.begin(), g_pending.end(),
            [&](const std::wstring& value) { return EqualInsensitive(value, scriptName); });
        if (found == g_pending.end())
        {
            g_pending.push_back(scriptName);
        }
        ReleaseSRWLockExclusive(&g_pendingLock);
    }

    std::vector<std::wstring> TakePending()
    {
        std::vector<std::wstring> result;
        AcquireSRWLockExclusive(&g_pendingLock);
        result.swap(g_pending);
        ReleaseSRWLockExclusive(&g_pendingLock);
        return result;
    }

    void* GetGalaxy()
    {
        __try
        {
            return g_runtime.galaxySlot ? *g_runtime.galaxySlot : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool GetScriptList(void* galaxy, void**& items, int& count)
    {
        items = nullptr;
        count = 0;
        __try
        {
            auto* list = *reinterpret_cast<unsigned char**>(
                static_cast<unsigned char*>(galaxy) + 0x150);
            if (!list)
            {
                return true;
            }
            count = *reinterpret_cast<int*>(list + 8);
            items = *reinterpret_cast<void***>(list + 4);
            return count >= 0 && count <= 65536 && (count == 0 || items != nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            items = nullptr;
            count = 0;
            return false;
        }
    }

    bool GetScriptName(void* script, std::wstring& name)
    {
        name.clear();
        __try
        {
            const wchar_t* text = *reinterpret_cast<const wchar_t**>(
                static_cast<unsigned char*>(script) + 8);
            if (!text)
            {
                return false;
            }
            const int length = *(reinterpret_cast<const int*>(text) - 1);
            if (length <= 0 || length > 2048)
            {
                return false;
            }
            name.assign(text, static_cast<size_t>(length));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            name.clear();
            return false;
        }
    }

    bool IsScriptIdle(void* script)
    {
        __try
        {
            auto* threads = *reinterpret_cast<unsigned char**>(
                static_cast<unsigned char*>(script) + 0x20);
            return !threads || *reinterpret_cast<int*>(threads + 8) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void* ReadListItem(void** items, int index)
    {
        __try
        {
            return items[index];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool InvokeRestart(void* script)
    {
#if defined(_M_IX86)
        unsigned char result = 0;
        void* target = g_runtime.restartScript;
        __try
        {
            __asm
            {
                mov eax, script
                xor edx, edx
                xor ecx, ecx
                call target
                mov result, al
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            result = 0;
        }
        return result != 0;
#else
        (void)script;
        return false;
#endif
    }

    void* FindActiveScript(const std::wstring& requested, bool& busy)
    {
        busy = false;
        void** items = nullptr;
        int count = 0;
        if (!GetScriptList(GetGalaxy(), items, count))
        {
            return nullptr;
        }
        for (int index = 0; index < count; ++index)
        {
            void* script = ReadListItem(items, index);
            std::wstring name;
            if (!script || !GetScriptName(script, name) ||
                !EqualInsensitive(name, requested))
            {
                continue;
            }
            busy = !IsScriptIdle(script);
            return script;
        }
        return nullptr;
    }

    std::vector<std::wstring> ActiveAllowedNames()
    {
        std::vector<std::wstring> result;
        void** items = nullptr;
        int count = 0;
        if (!GetScriptList(GetGalaxy(), items, count))
        {
            return result;
        }
        for (int index = 0; index < count; ++index)
        {
            void* script = ReadListItem(items, index);
            std::wstring name;
            if (script && GetScriptName(script, name) && IsAllowed(name))
            {
                result.push_back(name);
            }
        }
        return result;
    }

    void ReloadNames(const std::vector<std::wstring>& names)
    {
        if (!GetGalaxy())
        {
            for (const auto& name : names) QueueScript(name);
            return;
        }
        int restarted = 0;
        int busyCount = 0;
        for (const auto& name : names)
        {
            bool busy = false;
            void* script = FindActiveScript(name, busy);
            if (!script)
            {
                // An inactive script will be read from disk normally next time.
                continue;
            }
            if (busy)
            {
                ++busyCount;
                QueueScript(name);
                continue;
            }
            if (InvokeRestart(script))
            {
                ++restarted;
            }
            else
            {
                QueueScript(name);
                Log(L"restart failed for " + name + L"; kept pending");
            }
        }
        if (restarted != 0 || busyCount != 0)
        {
            std::wostringstream text;
            text << L"reload result: restarted=" << restarted
                 << L" busy_pending=" << busyCount;
            Log(text.str());
        }
    }

    void ProbeGalaxy()
    {
        if (!g_settings.reloadOnGalaxyLoad)
        {
            return;
        }
        void* galaxy = GetGalaxy();
        void** items = nullptr;
        int count = 0;
        if (!galaxy || !GetScriptList(galaxy, items, count))
        {
            g_candidateGalaxy = nullptr;
            g_candidateCount = -1;
            g_stableProbes = 0;
            return;
        }
        if (galaxy != g_candidateGalaxy || count != g_candidateCount)
        {
            g_candidateGalaxy = galaxy;
            g_candidateCount = count;
            g_stableProbes = 0;
            return;
        }
        if (galaxy == g_processedGalaxy || ++g_stableProbes < 2)
        {
            return;
        }
        g_processedGalaxy = galaxy;
        const auto names = ActiveAllowedNames();
        Log(L"galaxy load detected; scheduling " + std::to_wstring(names.size()) +
            L" cached script(s)");
        ReloadNames(names);
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == kReloadMessage)
        {
            if (wParam == kProbe)
            {
                ProbeGalaxy();
                const auto pending = TakePending();
                if (!pending.empty()) ReloadNames(pending);
            }
            else if (wParam == kChangedFiles)
            {
                ReloadNames(TakePending());
            }
            else if (wParam == kManualAll)
            {
                const auto names = ActiveAllowedNames();
                Log(L"manual reload requested for " + std::to_wstring(names.size()) +
                    L" active script(s)");
                ReloadNames(names);
            }
            return 0;
        }
        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
            static_cast<int>(wParam) == g_settings.hotkey)
        {
            PostMessageW(window, kReloadMessage, kManualAll, 0);
        }
        return g_originalWindowProc
            ? CallWindowProcW(g_originalWindowProc, window, message, wParam, lParam)
            : DefWindowProcW(window, message, wParam, lParam);
    }

    BOOL CALLBACK FindWindowCallback(HWND window, LPARAM parameter)
    {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId != GetCurrentProcessId() || GetWindow(window, GW_OWNER) != nullptr)
        {
            return TRUE;
        }
        if (!IsWindowVisible(window))
        {
            return TRUE;
        }
        *reinterpret_cast<HWND*>(parameter) = window;
        return FALSE;
    }

    HWND FindGameWindow()
    {
        HWND result = nullptr;
        EnumWindows(&FindWindowCallback, reinterpret_cast<LPARAM>(&result));
        return result;
    }

    void EnsureWindowSubclass()
    {
        HWND window = g_window.load();
        if (!window || !IsWindow(window))
        {
            window = FindGameWindow();
            if (!window)
            {
                return;
            }
            SetLastError(ERROR_SUCCESS);
            const auto previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc)));
            if (!previous && GetLastError() != ERROR_SUCCESS)
            {
                return;
            }
            g_originalWindowProc = previous;
            g_window.store(window);
            Log(L"game window attached; manual reload hotkey VK=" +
                std::to_wstring(g_settings.hotkey));
        }
    }

    bool HasScrExtension(const std::wstring& path)
    {
        const size_t dot = path.find_last_of(L'.');
        return dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, L".scr") == 0;
    }

    std::wstring FileStem(const std::wstring& path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        const size_t start = slash == std::wstring::npos ? 0 : slash + 1;
        const size_t dot = path.find_last_of(L'.');
        const size_t end = dot == std::wstring::npos || dot < start ? path.size() : dot;
        return path.substr(start, end - start);
    }

    void ScanDirectory(
        const std::wstring& directory,
        std::map<std::wstring, FileStamp>& files)
    {
        WIN32_FIND_DATAW data = {};
        const std::wstring mask = directory + L"\\*";
        HANDLE find = FindFirstFileW(mask.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
        {
            return;
        }
        do
        {
            if (wcscmp(data.cFileName, L".") == 0 ||
                wcscmp(data.cFileName, L"..") == 0)
            {
                continue;
            }
            const std::wstring path = directory + L"\\" + data.cFileName;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
                {
                    ScanDirectory(path, files);
                }
                continue;
            }
            if (!HasScrExtension(path))
            {
                continue;
            }
            ULARGE_INTEGER time = {};
            time.LowPart = data.ftLastWriteTime.dwLowDateTime;
            time.HighPart = data.ftLastWriteTime.dwHighDateTime;
            ULARGE_INTEGER size = {};
            size.LowPart = data.nFileSizeLow;
            size.HighPart = data.nFileSizeHigh;
            files.emplace(Lower(path), FileStamp{time.QuadPart, size.QuadPart});
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }

    void DetectChangedScripts()
    {
        if (!g_settings.autoReload || g_settings.watchRoot.empty())
        {
            return;
        }
        std::map<std::wstring, FileStamp> current;
        ScanDirectory(g_settings.watchRoot, current);
        if (g_initialSnapshotReady)
        {
            for (const auto& file : current)
            {
                const auto old = g_snapshot.find(file.first);
                if (old == g_snapshot.end() || !(old->second == file.second))
                {
                    QueueScript(FileStem(file.first));
                }
            }
        }
        else
        {
            g_initialSnapshotReady = true;
            Log(L"SCR watcher baseline: " + std::to_wstring(current.size()) + L" file(s)");
        }
        g_snapshot.swap(current);
    }

    DWORD WINAPI WorkerThread(void*)
    {
        for (;;)
        {
            EnsureWindowSubclass();
            DetectChangedScripts();
            HWND window = g_window.load();
            if (window && IsWindow(window))
            {
                PostMessageW(window, kReloadMessage, kProbe, 0);
                AcquireSRWLockShared(&g_pendingLock);
                const bool hasPending = !g_pending.empty();
                ReleaseSRWLockShared(&g_pendingLock);
                if (hasPending)
                {
                    PostMessageW(window, kReloadMessage, kChangedFiles, 0);
                }
            }
            Sleep(static_cast<DWORD>(g_settings.pollMilliseconds));
        }
    }

    std::wstring ReadString(
        const XenoPluginHostV1* host,
        const wchar_t* key,
        const wchar_t* fallback)
    {
        wchar_t buffer[4096] = {};
        if (host->size >= sizeof(XenoPluginHostV1) && host->configGetString)
        {
            host->configGetString(
                host->configPath, L"HotReload", key, fallback,
                buffer, static_cast<DWORD>(_countof(buffer)));
            return buffer;
        }
        return fallback;
    }

    bool ReadBool(
        const XenoPluginHostV1* host,
        const wchar_t* key,
        bool fallback)
    {
        return host->size >= sizeof(XenoPluginHostV1) && host->configGetBool
            ? host->configGetBool(
                host->configPath, L"HotReload", key, fallback ? TRUE : FALSE) != FALSE
            : fallback;
    }

    int ReadInt(
        const XenoPluginHostV1* host,
        const wchar_t* key,
        int fallback,
        int minimum,
        int maximum)
    {
        return host->size >= sizeof(XenoPluginHostV1) && host->configGetInt
            ? host->configGetInt(
                host->configPath, L"HotReload", key,
                fallback, minimum, maximum)
            : fallback;
    }
}

extern "C" BOOL WINAPI XenoPlugin_Query(XenoPluginInfoV1* info)
{
    if (!info || info->size < XENO_PLUGIN_INFO_V1_BASE_SIZE)
    {
        return FALSE;
    }
    info->requiredHostApi = XENO_NATIVE_HOST_API_V1;
    wcsncpy_s(info->id, kPluginId, _TRUNCATE);
    wcsncpy_s(info->version, L"0.1.0", _TRUNCATE);
    wcsncpy_s(
        info->description,
        L"Developer hot reload for active SCR instances cached in SAV",
        _TRUNCATE);
    if (info->size >= sizeof(XenoPluginInfoV1))
    {
        info->exclusiveCapabilities = 0;
    }
    return TRUE;
}

extern "C" DWORD WINAPI XenoPlugin_Initialize(const XenoPluginHostV1* host)
{
    if (!host || host->size < XENO_PLUGIN_HOST_V1_BASE_SIZE ||
        host->apiVersion < XENO_NATIVE_HOST_API_V1 || !host->gameModule)
    {
        return ERROR_INVALID_PARAMETER;
    }
    g_log = host->log;
    std::wstring error;
    if (!xsr::DiscoverRuntimePoints(host->gameModule, g_runtime, error))
    {
        Log(L"disabled: " + error);
        return ERROR_REVISION_MISMATCH;
    }

    g_settings.autoReload = ReadBool(host, L"AutoReloadChangedScr", true);
    g_settings.reloadOnGalaxyLoad = ReadBool(host, L"ReloadOnGalaxyLoad", false);
    g_settings.pollMilliseconds = ReadInt(host, L"PollMilliseconds", 800, 250, 10000);
    g_settings.hotkey = ReadInt(host, L"ManualHotkeyVirtualKey", VK_F10, 1, 255);
    g_settings.scriptPrefix = ReadString(host, L"ScriptPrefix", L"Mod_");
    g_settings.scriptNames = SplitNames(ReadString(host, L"ScriptNames", L""));

    std::wstring root = ReadString(host, L"WatchRoot", L"Mods");
    const bool absolute = root.size() >= 3 && root[1] == L':' &&
        (root[2] == L'\\' || root[2] == L'/');
    if (!absolute)
    {
        root = std::wstring(host->gameRoot ? host->gameRoot : L"") + L"\\" + root;
    }
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
    {
        root.pop_back();
    }
    g_settings.watchRoot = root;

    HANDLE worker = CreateThread(nullptr, 0, &WorkerThread, nullptr, 0, nullptr);
    if (!worker)
    {
        return GetLastError();
    }
    CloseHandle(worker);

    std::wostringstream text;
    text << L"initialized: auto=" << (g_settings.autoReload ? 1 : 0)
         << L" reload_on_galaxy=" << (g_settings.reloadOnGalaxyLoad ? 1 : 0)
         << L" watch=" << g_settings.watchRoot
         << L" prefix=" << g_settings.scriptPrefix
         << L" explicit_names=" << g_settings.scriptNames.size();
    Log(text.str());
    return ERROR_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
