#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <oleauto.h>

#include "generator_plan.h"
#include "hex_sector_layout.h"
#include "runtime_base_throttle.h"
#include "runtime_scan.h"
#include "../include/xeno_galaxy_generator_api.h"
#include "../include/xeno_plugin_api.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "oleaut32.lib")

namespace
{
    const wchar_t* kPluginId = L"XenoGalaxyGenerator";
    XenoHostLogFn g_hostLog = nullptr;
    HMODULE g_pluginModule = nullptr;
    HMODULE g_gameModule = nullptr;
    xgg::RuntimePoints g_points;
    xgg::RuntimeBaseSchedulerPoint g_runtimeBaseSchedulerPoint;
    xgg::RuntimeBaseThrottleSettings g_runtimeBaseThrottleSettings;
    std::vector<int> g_starPlan;
    volatile LONG g_planReady = 0;
    volatile LONG g_hexEnabled = 0;
    volatile LONG g_hexLogPending = 0;
    DWORD g_sectorCount = 20;
    DWORD g_retryLimit = 1;
    DWORD g_contourFallback = 1;
    int g_starCount = 73;
    int g_hexColumns = 0;
    float g_hexEdgeInset = 8.0f;
    uintptr_t g_sectorContinue = 0;
    uintptr_t g_contourContinue = 0;
    uintptr_t g_contourRemove = 0;
    uintptr_t g_contourOriginalTarget = 0;
    uintptr_t g_distributionComplete = 0;
    uintptr_t g_retryContinue = 0;
    uintptr_t g_geometryOriginalTarget = 0;
    uintptr_t g_runtimeBaseSchedulerOriginalTarget = 0;
    uintptr_t g_fullTrampoline = 0;
    volatile LONG g_runtimeBaseSchedulerDecision = 1;
    XggSectorSelectorV1 g_sectorSelector = nullptr;
    void* g_sectorSelectorUser = nullptr;
    XggFullGeneratorV1 g_fullGenerator = nullptr;
    void* g_fullGeneratorUser = nullptr;
    SRWLOCK g_registrationLock = SRWLOCK_INIT;

    struct InstalledHook
    {
        const wchar_t* name;
        unsigned char* address;
        size_t length;
        void* target;
        std::vector<unsigned char> original;
        bool installed;
    };

    std::vector<InstalledHook> g_runtimeHooks;
    InstalledHook g_geometryHook = {};
    InstalledHook g_runtimeBaseSchedulerHook = {};
    InstalledHook g_fullHook = {};

    void Log(const std::wstring& message)
    {
        if (g_hostLog)
        {
            g_hostLog(kPluginId, message.c_str());
        }
    }

    std::wstring Hex(const void* value)
    {
        std::wostringstream output;
        output << L"0x" << std::hex << std::uppercase << std::setfill(L'0')
               << std::setw(8) << reinterpret_cast<std::uintptr_t>(value);
        return output.str();
    }

    std::wstring PluginIniPath()
    {
        if (!g_pluginModule)
        {
            return {};
        }
        wchar_t path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(
            g_pluginModule,
            path,
            static_cast<DWORD>(_countof(path)));
        if (!length || length >= _countof(path))
        {
            return {};
        }
        std::wstring result(path, length);
        const size_t extension = result.find_last_of(L'.');
        if (extension == std::wstring::npos)
        {
            return {};
        }
        result.replace(extension, std::wstring::npos, L".ini");
        return result;
    }

    void LoadPluginConfiguration()
    {
        g_runtimeBaseThrottleSettings = {};
        const std::wstring path = PluginIniPath();
        if (path.empty())
        {
            return;
        }
        g_runtimeBaseThrottleSettings.enabled = GetPrivateProfileIntW(
            L"RuntimeBases", L"Enabled", 1, path.c_str()) != 0;
        g_runtimeBaseThrottleSettings.referenceStars = std::clamp(
            static_cast<int>(GetPrivateProfileIntW(
                L"RuntimeBases", L"ReferenceStars", 73, path.c_str())),
            1,
            4096);
        g_runtimeBaseThrottleSettings.firstGameplayDay = std::clamp(
            static_cast<int>(GetPrivateProfileIntW(
                L"RuntimeBases", L"FirstGameplayDay", 301, path.c_str())),
            0,
            1000000);
    }

    bool WriteNearBranch(
        InstalledHook& hook,
        unsigned char opcode,
        std::wstring& error)
    {
        if (!hook.address || !hook.target || hook.length < 5)
        {
            error = std::wstring(L"invalid hook: ") + hook.name;
            return false;
        }
        const intptr_t relative = reinterpret_cast<unsigned char*>(hook.target) -
            (hook.address + 5);
        if (relative < std::numeric_limits<std::int32_t>::min() ||
            relative > std::numeric_limits<std::int32_t>::max())
        {
            error = std::wstring(L"hook target is out of range: ") + hook.name;
            return false;
        }
        hook.original.assign(hook.address, hook.address + hook.length);
        DWORD oldProtect = 0;
        if (!VirtualProtect(
                hook.address, hook.length, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            error = std::wstring(L"VirtualProtect failed for ") + hook.name + L": " +
                std::to_wstring(GetLastError());
            return false;
        }
        hook.address[0] = opcode;
        const std::int32_t encoded = static_cast<std::int32_t>(relative);
        std::memcpy(hook.address + 1, &encoded, sizeof(encoded));
        for (size_t index = 5; index < hook.length; ++index)
        {
            hook.address[index] = 0x90;
        }
        FlushInstructionCache(GetCurrentProcess(), hook.address, hook.length);
        DWORD ignored = 0;
        VirtualProtect(hook.address, hook.length, oldProtect, &ignored);
        hook.installed = true;
        Log(std::wstring(L"hook_") + hook.name + L"=" + Hex(hook.address) +
            L" -> " + Hex(hook.target));
        return true;
    }

    bool WriteNearJump(InstalledHook& hook, std::wstring& error)
    {
        return WriteNearBranch(hook, 0xE9, error);
    }

    bool WriteNearCall(InstalledHook& hook, std::wstring& error)
    {
        return WriteNearBranch(hook, 0xE8, error);
    }

    void RestoreHook(InstalledHook& hook)
    {
        if (!hook.installed || hook.original.size() != hook.length)
        {
            return;
        }
        DWORD oldProtect = 0;
        if (VirtualProtect(
                hook.address, hook.length, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            std::memcpy(hook.address, hook.original.data(), hook.length);
            FlushInstructionCache(GetCurrentProcess(), hook.address, hook.length);
            DWORD ignored = 0;
            VirtualProtect(hook.address, hook.length, oldProtect, &ignored);
        }
        hook.installed = false;
    }

    int CallParCount(void* function, void* config, BSTR path)
    {
        int result = 0;
        __asm {
            mov eax, config
            mov edx, path
            call dword ptr [function]
            mov result, eax
        }
        return result;
    }

    void CallParGet(void* function, void* config, BSTR path, BSTR* output)
    {
        __asm {
            mov eax, config
            mov edx, path
            mov ecx, output
            call dword ptr [function]
        }
    }

    bool TryReadRaw(void* config, BSTR path, BSTR* output)
    {
        __try
        {
            if (CallParCount(g_points.parCount, config, path) < 1)
            {
                return false;
            }
            CallParGet(g_points.parGet, config, path, output);
            return *output != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void* ResolveConfigRoot()
    {
        __try
        {
            void** holder = g_points.configRootSlot
                ? *g_points.configRootSlot
                : nullptr;
            return holder ? *holder : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool ReadString(void* config, const std::wstring& path, std::wstring& value)
    {
        BSTR encodedPath = SysAllocStringLen(
            path.data(), static_cast<UINT>(path.size()));
        if (!encodedPath)
        {
            return false;
        }
        BSTR result = nullptr;
        const bool found = TryReadRaw(config, encodedPath, &result);
        SysFreeString(encodedPath);
        if (!found)
        {
            SysFreeString(result);
            return false;
        }
        value.assign(result, SysStringLen(result));
        SysFreeString(result);
        return true;
    }

    int ReadInt(void* config, const std::wstring& path, int fallback)
    {
        std::wstring text;
        if (!ReadString(config, path, text))
        {
            return fallback;
        }
        wchar_t* end = nullptr;
        const long value = std::wcstol(text.c_str(), &end, 10);
        return end && end != text.c_str() ? static_cast<int>(value) : fallback;
    }

    float ReadFloat(void* config, const std::wstring& path, float fallback)
    {
        std::wstring text;
        if (!ReadString(config, path, text))
        {
            return fallback;
        }
        wchar_t* end = nullptr;
        const float value = std::wcstof(text.c_str(), &end);
        return end && end != text.c_str() ? value : fallback;
    }

    bool ReadBool(void* config, const std::wstring& path, bool fallback)
    {
        std::wstring text;
        if (!ReadString(config, path, text))
        {
            return fallback;
        }
        std::transform(text.begin(), text.end(), text.begin(), towlower);
        if (text == L"yes" || text == L"true" || text == L"1" || text == L"on")
        {
            return true;
        }
        if (text == L"no" || text == L"false" || text == L"0" || text == L"off")
        {
            return false;
        }
        return fallback;
    }

    xgg::DistributionMode ReadMode(void* config)
    {
        std::wstring text;
        if (!ReadString(
                config, L"Constellations.Generator.DistributionMode", text))
        {
            return xgg::DistributionMode::Weighted;
        }
        std::transform(text.begin(), text.end(), text.begin(), towlower);
        if (text == L"roundrobin" || text == L"round-robin")
        {
            return xgg::DistributionMode::RoundRobin;
        }
        if (text == L"explicit")
        {
            return xgg::DistributionMode::Explicit;
        }
        return xgg::DistributionMode::Weighted;
    }

    bool ReadHexGeometryMode(void* config)
    {
        std::wstring text;
        if (!ReadString(config, L"Constellations.Generator.GeometryMode", text))
        {
            return false;
        }
        std::transform(text.begin(), text.end(), text.begin(), towlower);
        return text == L"hexgridtest" || text == L"hexgrid" || text == L"hex";
    }

    xgg::PlanSettings LoadPlanSettings(void* config)
    {
        xgg::PlanSettings settings;
        settings.starCount = std::clamp(
            ReadInt(config, L"Constellations.GalaxyCountStars", 73), 1, 4096);
        settings.sectorCount = std::clamp(
            ReadInt(config, L"Constellations.Generator.SectorCount", 20), 20, 127);
        settings.firstCustomSector = std::clamp(
            ReadInt(config, L"Constellations.Generator.FirstCustomSector", 20),
            0,
            settings.sectorCount);
        settings.preserveVanillaStars = ReadBool(
            config, L"Constellations.Generator.PreserveVanillaStars", true);
        settings.mode = ReadMode(config);
        settings.sectors.resize(static_cast<size_t>(settings.sectorCount));

        xgg::SectorRule defaults;
        defaults.enabled = ReadBool(
            config, L"Constellations.Generator.SectorDefaults.Enabled", true);
        defaults.weight = std::clamp(ReadInt(
            config, L"Constellations.Generator.SectorDefaults.Weight", 1), 0, 100000);
        defaults.minimumStars = std::clamp(ReadInt(
            config, L"Constellations.Generator.SectorDefaults.MinStars", 0),
            0,
            settings.starCount);
        defaults.maximumStars = std::clamp(ReadInt(
            config, L"Constellations.Generator.SectorDefaults.MaxStars", 0),
            0,
            settings.starCount);

        for (int sector = 0; sector < settings.sectorCount; ++sector)
        {
            const std::wstring prefix = L"Constellations.Generator.Sectors." +
                std::to_wstring(sector) + L".";
            xgg::SectorRule& rule = settings.sectors[static_cast<size_t>(sector)];
            rule.enabled = ReadBool(config, prefix + L"Enabled", defaults.enabled);
            rule.weight = std::clamp(
                ReadInt(config, prefix + L"Weight", defaults.weight), 0, 100000);
            rule.minimumStars = std::clamp(
                ReadInt(config, prefix + L"MinStars", defaults.minimumStars),
                0,
                settings.starCount);
            rule.maximumStars = std::clamp(
                ReadInt(config, prefix + L"MaxStars", defaults.maximumStars),
                0,
                settings.starCount);
        }

        for (int star = 0; star < settings.starCount; ++star)
        {
            const std::wstring path = L"Constellations.Generator.Stars." +
                std::to_wstring(star) + L".Sector";
            const int sector = ReadInt(config, path, -1);
            if (sector >= 0)
            {
                settings.explicitAssignments.emplace_back(star, sector);
            }
        }
        return settings;
    }

    void LoadGenerationConfiguration()
    {
        InterlockedExchange(&g_planReady, 0);
        void* config = ResolveConfigRoot();
        xgg::PlanSettings settings;
        if (config)
        {
            settings = LoadPlanSettings(config);
            g_retryLimit = static_cast<DWORD>(std::clamp(
                ReadInt(config, L"Constellations.Generator.RetryLimit", 1),
                1,
                100000));
            g_contourFallback = ReadBool(
                config, L"Constellations.Generator.ContourFallback", true) ? 1u : 0u;
            g_hexColumns = std::clamp(ReadInt(
                config, L"Constellations.Generator.HexColumns", 0), 0, 127);
            g_hexEdgeInset = std::clamp(ReadFloat(
                config, L"Constellations.Generator.HexEdgeInset", 8.0f),
                0.0f,
                256.0f);
            InterlockedExchange(&g_hexEnabled, ReadHexGeometryMode(config) ? 1 : 0);
        }
        else
        {
            settings.starCount = 73;
            settings.sectorCount = 20;
            settings.firstCustomSector = 20;
            settings.preserveVanillaStars = true;
            settings.sectors.resize(20);
            g_retryLimit = 1;
            g_contourFallback = 1;
            g_hexColumns = 0;
            g_hexEdgeInset = 8.0f;
            InterlockedExchange(&g_hexEnabled, 0);
            Log(L"config=unavailable_using_safe_defaults");
        }

        const xgg::PlanResult plan = xgg::BuildPlan(settings);
        g_starPlan = plan.starToSector;
        g_starCount = settings.starCount;
        g_sectorCount = static_cast<DWORD>(settings.sectorCount);
        InterlockedExchange(&g_runtimeBaseSchedulerDecision, 1);
        InterlockedExchange(&g_planReady, 1);
        InterlockedExchange(&g_hexLogPending, 1);

        std::wostringstream message;
        message << L"generation_plan=ready stars=" << settings.starCount
                << L" sectors=" << settings.sectorCount
                << L" first_custom=" << settings.firstCustomSector
                << L" preserve_vanilla="
                << (settings.preserveVanillaStars ? L"true" : L"false")
                << L" explicit=" << settings.explicitAssignments.size()
                << L" retry=" << g_retryLimit
                << L" contour_fallback=" << (g_contourFallback ? L"true" : L"false")
                << L" geometry=" << (g_hexEnabled ? L"hex_grid_test" : L"vanilla")
                << L" hex_columns=" << g_hexColumns
                << L" hex_edge_inset=" << g_hexEdgeInset
                << L" initial_base_scaling=false"
                << L" initial_base_scaling_reason=runtime_safety"
                << L" runtime_base_throttle="
                << (g_runtimeBaseThrottleSettings.enabled ? L"true" : L"false")
                << L" runtime_base_reference_stars="
                << g_runtimeBaseThrottleSettings.referenceStars
                << L" runtime_base_first_day="
                << g_runtimeBaseThrottleSettings.firstGameplayDay;
        Log(message.str());
        if (!plan.warning.empty())
        {
            Log(L"generation_plan_warning=" + plan.warning);
        }
    }

    void WINAPI BeginGalaxy(void*)
    {
        try
        {
            LoadGenerationConfiguration();
        }
        catch (...)
        {
            g_sectorCount = 20;
            g_retryLimit = 1;
            g_contourFallback = 1;
            g_starCount = 73;
            g_hexColumns = 0;
            g_hexEdgeInset = 8.0f;
            InterlockedExchange(&g_hexEnabled, 0);
            InterlockedExchange(&g_runtimeBaseSchedulerDecision, 1);
            xgg::PlanSettings fallback;
            fallback.starCount = 73;
            fallback.sectorCount = 20;
            fallback.firstCustomSector = 20;
            fallback.sectors.resize(20);
            g_starPlan = xgg::BuildPlan(fallback).starToSector;
            InterlockedExchange(&g_planReady, 1);
            Log(L"generation_plan=exception_using_safe_defaults");
        }
    }

    bool TryReadGalaxyDay(void* galaxy, int& day)
    {
        __try
        {
            if (!galaxy)
            {
                return false;
            }
            day = *reinterpret_cast<int*>(
                static_cast<unsigned char*>(galaxy) + 0x4C);
            return day >= 0 && day < 1000000;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    BOOL WINAPI EvaluateRuntimeBaseScheduler(void* galaxy)
    {
        int day = -1;
        if (!TryReadGalaxyDay(galaxy, day))
        {
            return TRUE;
        }
        return xgg::ShouldRunRuntimeBaseScheduler(
            day,
            g_starCount,
            g_runtimeBaseThrottleSettings) ? TRUE : FALSE;
    }

    int WINAPI SelectSector(int starIndex, void* galaxy)
    {
        int planned = 0;
        if (g_planReady && starIndex >= 0 &&
            static_cast<size_t>(starIndex) < g_starPlan.size())
        {
            planned = g_starPlan[static_cast<size_t>(starIndex)];
        }
        else
        {
            planned = xgg::VanillaSectorForStar(starIndex, static_cast<int>(g_sectorCount));
        }
        if (g_sectorSelector)
        {
            const int selected = g_sectorSelector(
                starIndex,
                planned,
                static_cast<int>(g_sectorCount),
                galaxy,
                g_sectorSelectorUser);
            if (selected >= 0 && selected < static_cast<int>(g_sectorCount))
            {
                planned = selected;
            }
        }
        return std::clamp(planned, 0, static_cast<int>(g_sectorCount) - 1);
    }

    bool TryReadMapDimensions(int* width, int* height)
    {
        __try
        {
            int* widthValue = g_points.mapWidthSlot ? *g_points.mapWidthSlot : nullptr;
            int* heightValue = g_points.mapHeightSlot ? *g_points.mapHeightSlot : nullptr;
            if (!width || !height || !widthValue || !heightValue)
            {
                return false;
            }
            *width = *widthValue;
            *height = *heightValue;
            return *width >= 128 && *width <= 16384 &&
                *height >= 128 && *height <= 16384;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool TryWriteHexCenters(
        void* galaxy,
        const xgg::HexPoint* centers,
        int centerCount)
    {
        __try
        {
            if (!galaxy || !centers || centerCount < 1 || centerCount > 127)
            {
                return false;
            }
            unsigned char* galaxyBytes = static_cast<unsigned char*>(galaxy);
            void* list = *reinterpret_cast<void**>(galaxyBytes + 0x164);
            if (!list)
            {
                return false;
            }
            unsigned char* listBytes = static_cast<unsigned char*>(list);
            void** items = *reinterpret_cast<void***>(listBytes + 0x04);
            const int listCount = *reinterpret_cast<int*>(listBytes + 0x08);
            if (!items || listCount != centerCount)
            {
                return false;
            }
            for (int index = 0; index < centerCount; ++index)
            {
                if (!items[index])
                {
                    return false;
                }
            }
            for (int index = 0; index < centerCount; ++index)
            {
                unsigned char* sector = static_cast<unsigned char*>(items[index]);
                *reinterpret_cast<float*>(sector + 0x0C) = centers[index].x;
                *reinterpret_cast<float*>(sector + 0x10) = centers[index].y;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void WINAPI ApplyHexLayout(void* galaxy)
    {
        if (!g_hexEnabled)
        {
            return;
        }
        int width = 0;
        int height = 0;
        if (!TryReadMapDimensions(&width, &height))
        {
            if (InterlockedExchange(&g_hexLogPending, 0))
            {
                Log(L"hex_geometry=skipped invalid_map_dimensions");
            }
            return;
        }

        try
        {
            xgg::HexLayoutSettings settings;
            settings.sectorCount = static_cast<int>(g_sectorCount);
            settings.mapWidth = width;
            settings.mapHeight = height;
            settings.columns = g_hexColumns;
            settings.edgeInset = g_hexEdgeInset;
            const xgg::HexLayoutResult layout = xgg::BuildHexSectorLayout(settings);
            const bool applied = layout.centers.size() == g_sectorCount &&
                TryWriteHexCenters(
                    galaxy,
                    layout.centers.data(),
                    static_cast<int>(layout.centers.size()));
            if (InterlockedExchange(&g_hexLogPending, 0))
            {
                std::wostringstream message;
                message << L"hex_geometry=" << (applied ? L"applied" : L"skipped")
                        << L" map=" << width << L"x" << height
                        << L" sectors=" << g_sectorCount
                        << L" columns=" << layout.columns
                        << L" rows=" << layout.rows
                        << L" edge_cells=" << layout.edgeCells
                        << L" corner_cells=" << layout.cornerCells;
                if (!layout.warning.empty())
                {
                    message << L" warning=" << layout.warning;
                }
                Log(message.str());
            }
        }
        catch (...)
        {
            if (InterlockedExchange(&g_hexLogPending, 0))
            {
                Log(L"hex_geometry=skipped exception");
            }
        }
    }

    __declspec(naked) void SectorStub()
    {
        __asm {
            pushfd
            pushad
            push eax
            call BeginGalaxy
            popad
            popfd
            push ecx
            mov ecx, dword ptr [g_sectorCount]
            mov dword ptr [eax+160h], ecx
            pop ecx
            jmp dword ptr [g_sectorContinue]
        }
    }

    __declspec(naked) void ContourStub()
    {
        __asm {
            pushfd
            cmp dword ptr [g_contourFallback], 0
            je vanilla
            popfd
            jg fallback_remove
            jmp dword ptr [g_contourContinue]
        vanilla:
            popfd
            jg original_target
            jmp dword ptr [g_contourContinue]
        fallback_remove:
            jmp dword ptr [g_contourRemove]
        original_target:
            jmp dword ptr [g_contourOriginalTarget]
        }
    }

    __declspec(naked) void DistributionStub()
    {
        __asm {
            pushfd
            pushad
            mov eax, dword ptr [ebp-04h]
            push eax
            mov eax, dword ptr [ebp-0Ch]
            push eax
            call SelectSector
            mov dword ptr [ebp-64h], eax
            popad
            popfd
            jmp dword ptr [g_distributionComplete]
        }
    }

    __declspec(naked) void RetryStub()
    {
        __asm {
            push eax
            mov eax, dword ptr [g_retryLimit]
            cmp dword ptr [ebp-0B0h], eax
            pop eax
            jmp dword ptr [g_retryContinue]
        }
    }

    __declspec(naked) void GeometryStub()
    {
        __asm {
            pushfd
            pushad
            push eax
            call ApplyHexLayout
            popad
            popfd
            jmp dword ptr [g_geometryOriginalTarget]
        }
    }

    __declspec(naked) void RuntimeBaseSchedulerStub()
    {
        __asm {
            pushfd
            pushad
            push eax
            call EvaluateRuntimeBaseScheduler
            mov dword ptr [g_runtimeBaseSchedulerDecision], eax
            popad
            popfd
            cmp dword ptr [g_runtimeBaseSchedulerDecision], 0
            je skip_scheduler
            jmp dword ptr [g_runtimeBaseSchedulerOriginalTarget]
        skip_scheduler:
            ret
        }
    }

    __declspec(naked) void FullGeneratorStub()
    {
        __asm {
            pushfd
            pushad
            mov eax, esp
            push dword ptr [g_fullGeneratorUser]
            push eax
            call dword ptr [g_fullGenerator]
            test eax, eax
            jz run_original
            popad
            popfd
            ret
        run_original:
            popad
            popfd
            jmp dword ptr [g_fullTrampoline]
        }
    }

    bool InstallRuntimeHooks(std::wstring& error)
    {
        g_sectorContinue = reinterpret_cast<uintptr_t>(g_points.sector + 10);
        g_contourContinue = reinterpret_cast<uintptr_t>(g_points.contour + 6);
        g_contourRemove = reinterpret_cast<uintptr_t>(g_points.contourRemove);
        g_contourOriginalTarget = reinterpret_cast<uintptr_t>(
            g_points.contourOriginalTarget);
        g_distributionComplete = reinterpret_cast<uintptr_t>(
            g_points.distributionComplete);
        g_retryContinue = reinterpret_cast<uintptr_t>(g_points.retry + 10);
        g_geometryOriginalTarget = reinterpret_cast<uintptr_t>(
            g_points.geometryOriginalTarget);
        g_runtimeHooks = {
            { L"sector", g_points.sector, 10,
                reinterpret_cast<void*>(&SectorStub), {}, false },
            { L"contour", g_points.contour, 6,
                reinterpret_cast<void*>(&ContourStub), {}, false },
            { L"distribution", g_points.distribution, 7,
                reinterpret_cast<void*>(&DistributionStub), {}, false },
            { L"retry", g_points.retry, 10,
                reinterpret_cast<void*>(&RetryStub), {}, false }
        };
        for (size_t index = 0; index < g_runtimeHooks.size(); ++index)
        {
            if (!WriteNearJump(g_runtimeHooks[index], error))
            {
                while (index > 0)
                {
                    RestoreHook(g_runtimeHooks[--index]);
                }
                return false;
            }
        }
        g_geometryHook = { L"geometry", g_points.geometryCall, 5,
            reinterpret_cast<void*>(&GeometryStub), {}, false };
        if (!WriteNearCall(g_geometryHook, error))
        {
            for (size_t index = g_runtimeHooks.size(); index > 0; --index)
            {
                RestoreHook(g_runtimeHooks[index - 1]);
            }
            return false;
        }
        if (g_runtimeBaseThrottleSettings.enabled &&
            g_runtimeBaseSchedulerPoint.call &&
            g_runtimeBaseSchedulerPoint.originalTarget)
        {
            g_runtimeBaseSchedulerOriginalTarget = reinterpret_cast<uintptr_t>(
                g_runtimeBaseSchedulerPoint.originalTarget);
            g_runtimeBaseSchedulerHook = {
                L"runtime-base-scheduler",
                g_runtimeBaseSchedulerPoint.call,
                5,
                reinterpret_cast<void*>(&RuntimeBaseSchedulerStub),
                {},
                false
            };
            if (!WriteNearCall(g_runtimeBaseSchedulerHook, error))
            {
                RestoreHook(g_geometryHook);
                for (size_t index = g_runtimeHooks.size(); index > 0; --index)
                {
                    RestoreHook(g_runtimeHooks[index - 1]);
                }
                return false;
            }
        }
        return true;
    }

    bool BuildFullTrampoline(std::wstring& error)
    {
        const size_t copied = 8;
        const size_t size = copied + 5;
        auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
            nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!trampoline)
        {
            error = L"VirtualAlloc failed for full generator trampoline: " +
                std::to_wstring(GetLastError());
            return false;
        }
        std::memcpy(trampoline, g_points.fullGenerator, copied);
        trampoline[copied] = 0xE9;
        const intptr_t relative = (g_points.fullGenerator + copied) -
            (trampoline + copied + 5);
        if (relative < std::numeric_limits<std::int32_t>::min() ||
            relative > std::numeric_limits<std::int32_t>::max())
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            error = L"full generator trampoline is out of x86 near-jump range";
            return false;
        }
        const std::int32_t encoded = static_cast<std::int32_t>(relative);
        std::memcpy(trampoline + copied + 1, &encoded, sizeof(encoded));
        FlushInstructionCache(GetCurrentProcess(), trampoline, size);
        g_fullTrampoline = reinterpret_cast<uintptr_t>(trampoline);
        return true;
    }

    BOOL WINAPI RegisterSectorSelector(
        XggSectorSelectorV1 callback,
        void* userData)
    {
        if (!callback)
        {
            return FALSE;
        }
        AcquireSRWLockExclusive(&g_registrationLock);
        const bool available = g_sectorSelector == nullptr;
        if (available)
        {
            g_sectorSelectorUser = userData;
            g_sectorSelector = callback;
            Log(L"external_sector_selector=registered");
        }
        ReleaseSRWLockExclusive(&g_registrationLock);
        return available ? TRUE : FALSE;
    }

    BOOL WINAPI RegisterFullGenerator(
        XggFullGeneratorV1 callback,
        void* userData)
    {
        if (!callback || !g_points.fullGenerator)
        {
            return FALSE;
        }
        AcquireSRWLockExclusive(&g_registrationLock);
        bool success = false;
        if (!g_fullGenerator)
        {
            std::wstring error;
            if (BuildFullTrampoline(error))
            {
                g_fullGeneratorUser = userData;
                g_fullGenerator = callback;
                g_fullHook = { L"full-generator", g_points.fullGenerator, 8,
                    reinterpret_cast<void*>(&FullGeneratorStub), {}, false };
                if (WriteNearJump(g_fullHook, error))
                {
                    success = true;
                    Log(L"external_full_generator=registered original=" +
                        Hex(g_points.fullGenerator));
                }
                else
                {
                    g_fullGenerator = nullptr;
                    g_fullGeneratorUser = nullptr;
                    VirtualFree(reinterpret_cast<void*>(g_fullTrampoline), 0, MEM_RELEASE);
                    g_fullTrampoline = 0;
                }
            }
            if (!success)
            {
                Log(L"external_full_generator=failed " + error);
            }
        }
        ReleaseSRWLockExclusive(&g_registrationLock);
        return success ? TRUE : FALSE;
    }

    int WINAPI GetSectorCount()
    {
        return static_cast<int>(g_sectorCount);
    }

    int WINAPI GetPlannedSector(int starIndex)
    {
        return g_planReady && starIndex >= 0 &&
            static_cast<size_t>(starIndex) < g_starPlan.size()
            ? g_starPlan[static_cast<size_t>(starIndex)]
            : -1;
    }
}

extern "C" BOOL WINAPI XenoPlugin_Query(XenoPluginInfoV1* info)
{
    if (!info || info->size < XENO_PLUGIN_INFO_V1_BASE_SIZE)
    {
        return FALSE;
    }
    info->requiredHostApi = XENO_NATIVE_HOST_API_V1;
    lstrcpynW(info->id, kPluginId, static_cast<int>(_countof(info->id)));
    lstrcpynW(info->version, L"1.4.0", static_cast<int>(_countof(info->version)));
    lstrcpynW(
        info->description,
        L"Data-driven and externally replaceable SRHD galaxy generator",
        static_cast<int>(_countof(info->description)));
    if (info->size >= sizeof(XenoPluginInfoV1))
    {
        info->exclusiveCapabilities = XENO_PLUGIN_CAP_GALAXY_GENERATOR;
    }
    return TRUE;
}

extern "C" DWORD WINAPI XenoPlugin_Initialize(const XenoPluginHostV1* host)
{
    if (!host || host->apiVersion < XENO_NATIVE_HOST_API_V1 ||
        host->size < sizeof(XenoPluginHostV1) || !host->gameModule)
    {
        return 1;
    }
    g_hostLog = host->log;
    g_gameModule = host->gameModule;
    LoadPluginConfiguration();
    std::wstring error;
    if (!xgg::DiscoverRuntimePoints(host->gameModule, g_points, error))
    {
        Log(L"runtime=failed " + error);
        return 2;
    }
    Log(L"full_generator=" + Hex(g_points.fullGenerator));
    Log(L"config_root_slot=" + Hex(g_points.configRootSlot));
    Log(L"geometry_call=" + Hex(g_points.geometryCall));
    if (g_runtimeBaseThrottleSettings.enabled)
    {
        if (xgg::DiscoverRuntimeBaseSchedulerPoint(
                host->gameModule,
                g_runtimeBaseSchedulerPoint,
                error))
        {
            Log(L"runtime_base_scheduler=" +
                Hex(g_runtimeBaseSchedulerPoint.originalTarget));
        }
        else
        {
            Log(L"runtime_base_throttle=disabled reason=signature_unavailable " + error);
            g_runtimeBaseThrottleSettings.enabled = false;
            g_runtimeBaseSchedulerPoint = {};
        }
    }
    if (!InstallRuntimeHooks(error))
    {
        Log(L"runtime=failed " + error);
        return 3;
    }
    Log(L"initial_base_scaling=disabled reason=runtime_safety");
    std::wostringstream throttle;
    throttle << L"runtime_base_throttle="
             << (g_runtimeBaseThrottleSettings.enabled ? L"enabled" : L"disabled")
             << L" reference_stars=" << g_runtimeBaseThrottleSettings.referenceStars
             << L" first_day=" << g_runtimeBaseThrottleSettings.firstGameplayDay;
    Log(throttle.str());
    Log(L"runtime=installed version=1.4.0");
    return 0;
}

extern "C" BOOL WINAPI XenoGalaxyGenerator_GetApi(XggApiTableV1* api)
{
    if (!api || api->size < sizeof(XggApiTableV1))
    {
        return FALSE;
    }
    api->version = XGG_API_V1;
    api->gameModule = g_gameModule;
    api->originalFullGenerator = g_points.fullGenerator;
    api->registerSectorSelector = &RegisterSectorSelector;
    api->registerFullGenerator = &RegisterFullGenerator;
    api->getSectorCount = &GetSectorCount;
    api->getPlannedSector = &GetPlannedSector;
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_pluginModule = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
