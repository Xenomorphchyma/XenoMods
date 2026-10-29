// XenoPBEngineFix: repairs the map cache file names of the PBEngine
// planetary battles engine (Matrix\MatrixGame.dll).
//
// PBEngine stores a per-map cache as
//   Documents\SpaceRangersHD\Cache\<map>.cmap.<8 letters A-P>
// where the letters encode the map's UniqID. The wide path handed to
// CreateFileW sometimes carries leftovers of an older string after the eight
// letters ("...FNBBDCBJ4.txt", "...BHAKNFNA\BPele"). Creating or reopening the
// cache then fails, MatrixGame throws, the battle aborts and the game quits.
// How often this happens depends on heap layout; under D3D9On12 it is common.
//
// The plugin points MatrixGame.dll's CreateFileW/CreateFileA imports at
// wrappers that cut such a name right after the eight letters. Every other
// path is passed through untouched. No game or engine code is patched.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <string>

#include "xeno_plugin_api.h"

namespace
{
    const wchar_t* kPluginId = L"XenoPBEngineFix";
    const wchar_t* kPluginVersion = L"1.0.0";
    constexpr size_t kCacheSuffixLength = 8;
    constexpr LONG kMaxRepairLogs = 16;

    typedef HANDLE (WINAPI* CreateFileWFn)(
        LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    typedef HANDLE (WINAPI* CreateFileAFn)(
        LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

    XenoHostLogFn g_log = nullptr;
    CreateFileWFn g_createFileW = nullptr;
    CreateFileAFn g_createFileA = nullptr;
    volatile LONG g_repairLogs = 0;

    void Log(const std::wstring& message)
    {
        if (g_log)
        {
            g_log(kPluginId, message.c_str());
        }
    }

    template <typename Char>
    Char Lower(Char value)
    {
        return value >= 'A' && value <= 'Z' ? static_cast<Char>(value - 'A' + 'a') : value;
    }

    template <typename Char>
    bool ContainsInsensitive(const Char* text, size_t length, const char* needle)
    {
        const size_t needleLength = std::strlen(needle);
        for (size_t start = 0; start + needleLength <= length; ++start)
        {
            size_t index = 0;
            while (index < needleLength &&
                Lower(text[start + index]) == static_cast<Char>(needle[index]))
            {
                ++index;
            }
            if (index == needleLength)
            {
                return true;
            }
        }
        return false;
    }

    // Length the path should have, or 0 when it is not a damaged cache name.
    template <typename Char>
    size_t RepairedLength(const Char* path)
    {
        if (!path)
        {
            return 0;
        }
        size_t length = 0;
        while (path[length])
        {
            ++length;
        }
        if (!ContainsInsensitive(path, length, "\\cache\\"))
        {
            return 0;
        }
        static const char marker[] = ".cmap.";
        const size_t markerLength = sizeof(marker) - 1;
        for (size_t start = length; start-- > 0;)
        {
            if (start + markerLength > length)
            {
                continue;
            }
            size_t index = 0;
            while (index < markerLength &&
                Lower(path[start + index]) == static_cast<Char>(marker[index]))
            {
                ++index;
            }
            if (index != markerLength)
            {
                continue;
            }
            const size_t suffix = start + markerLength;
            if (suffix + kCacheSuffixLength >= length)
            {
                return 0; // intact, or too short to repair safely
            }
            for (size_t letter = 0; letter < kCacheSuffixLength; ++letter)
            {
                const Char value = path[suffix + letter];
                if (value < 'A' || value > 'P')
                {
                    return 0;
                }
            }
            return suffix + kCacheSuffixLength;
        }
        return 0;
    }

    void LogRepair(const std::wstring& original, size_t kept)
    {
        if (InterlockedIncrement(&g_repairLogs) <= kMaxRepairLogs)
        {
            Log(L"cache_name=repaired kept=" + original.substr(0, kept) +
                L" dropped=" + original.substr(kept));
        }
    }

    HANDLE WINAPI CreateFileWHook(LPCWSTR name, DWORD access, DWORD share,
        LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile)
    {
        const size_t kept = RepairedLength(name);
        if (!kept)
        {
            return g_createFileW(name, access, share, security, disposition, flags, templateFile);
        }
        const std::wstring original(name);
        LogRepair(original, kept);
        const std::wstring repaired = original.substr(0, kept);
        return g_createFileW(
            repaired.c_str(), access, share, security, disposition, flags, templateFile);
    }

    HANDLE WINAPI CreateFileAHook(LPCSTR name, DWORD access, DWORD share,
        LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile)
    {
        const size_t kept = RepairedLength(name);
        if (!kept)
        {
            return g_createFileA(name, access, share, security, disposition, flags, templateFile);
        }
        const std::string original(name);
        const int size = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
        std::wstring wide(size > 0 ? size - 1 : 0, L'\0');
        if (size > 1)
        {
            MultiByteToWideChar(CP_ACP, 0, name, -1, &wide[0], size);
        }
        // Code-page text: one byte per character, so the cut index carries over.
        LogRepair(wide, kept < wide.size() ? kept : wide.size());
        const std::string repaired = original.substr(0, kept);
        return g_createFileA(
            repaired.c_str(), access, share, security, disposition, flags, templateFile);
    }

    // Points the kernel32 CreateFileW/CreateFileA imports of MatrixGame.dll at
    // the wrappers. Returns how many import slots now use them.
    int HookImports(HMODULE module)
    {
        auto* base = reinterpret_cast<unsigned char*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE)
        {
            return 0;
        }
        const IMAGE_DATA_DIRECTORY& directory =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!directory.VirtualAddress)
        {
            return 0;
        }
        int hooked = 0;
        for (auto* entry = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
                 base + directory.VirtualAddress);
            entry->Name; ++entry)
        {
            if (_stricmp(reinterpret_cast<const char*>(base + entry->Name), "KERNEL32.dll") != 0 ||
                !entry->OriginalFirstThunk)
            {
                continue;
            }
            const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(
                base + entry->OriginalFirstThunk);
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + entry->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++slots)
            {
                if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32)
                {
                    continue;
                }
                const char* name = reinterpret_cast<const char*>(
                    reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                        base + names->u1.AddressOfData)->Name);
                DWORD replacement = 0;
                if (std::strcmp(name, "CreateFileW") == 0)
                {
                    replacement = reinterpret_cast<DWORD>(&CreateFileWHook);
                }
                else if (std::strcmp(name, "CreateFileA") == 0)
                {
                    replacement = reinterpret_cast<DWORD>(&CreateFileAHook);
                }
                if (!replacement)
                {
                    continue;
                }
                if (slots->u1.Function == replacement)
                {
                    ++hooked;
                    continue;
                }
                DWORD oldProtect = 0;
                if (VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function),
                        PAGE_READWRITE, &oldProtect))
                {
                    InterlockedExchange(
                        reinterpret_cast<volatile LONG*>(&slots->u1.Function),
                        static_cast<LONG>(replacement));
                    VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function),
                        oldProtect, &oldProtect);
                    ++hooked;
                }
            }
        }
        return hooked;
    }

    // MatrixGame.dll is loaded after the plugins and may be reloaded when the
    // mod list is applied, so the imports are checked periodically.
    DWORD WINAPI WatchMatrixGame(void*)
    {
        HMODULE lastModule = nullptr;
        for (;;)
        {
            const HMODULE module = GetModuleHandleW(L"MatrixGame.dll");
            if (module)
            {
                const int hooked = HookImports(module);
                if (module != lastModule)
                {
                    lastModule = module;
                    Log(L"matrixgame=hooked imports=" + std::to_wstring(hooked));
                }
            }
            Sleep(500);
        }
    }
}

extern "C" BOOL WINAPI XenoPlugin_Query(XenoPluginInfoV1* info)
{
    if (!info || info->size < XENO_PLUGIN_INFO_V1_BASE_SIZE)
    {
        return FALSE;
    }
    info->requiredHostApi = XENO_NATIVE_HOST_API_V1;
    lstrcpynW(info->id, kPluginId, 64);
    lstrcpynW(info->version, kPluginVersion, 32);
    lstrcpynW(info->description, L"Fixes PBEngine map cache file names in planetary battles", 160);
    if (info->size >= sizeof(XenoPluginInfoV1))
    {
        info->exclusiveCapabilities = 0;
    }
    return TRUE;
}

extern "C" DWORD WINAPI XenoPlugin_Initialize(const XenoPluginHostV1* host)
{
    if (!host || host->size < XENO_PLUGIN_HOST_V1_BASE_SIZE ||
        host->apiVersion < XENO_NATIVE_HOST_API_V1)
    {
        return 1;
    }
    g_log = host->log;
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    g_createFileW = kernel32
        ? reinterpret_cast<CreateFileWFn>(GetProcAddress(kernel32, "CreateFileW"))
        : nullptr;
    g_createFileA = kernel32
        ? reinterpret_cast<CreateFileAFn>(GetProcAddress(kernel32, "CreateFileA"))
        : nullptr;
    if (!g_createFileW || !g_createFileA)
    {
        Log(L"runtime=blocked reason=createfile_missing");
        return 2;
    }
    HANDLE thread = CreateThread(nullptr, 0, WatchMatrixGame, nullptr, 0, nullptr);
    if (!thread)
    {
        Log(L"runtime=blocked reason=thread_failed");
        return 3;
    }
    CloseHandle(thread);
    Log(std::wstring(L"runtime=installed version=") + kPluginVersion);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
