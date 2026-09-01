#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "live_game_adapter.h"
#include "nextday_policy.h"
#include "runtime_scan.h"
#include "../include/xeno_nextday_api.h"
#include "../include/xeno_plugin_api.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>

namespace
{
    const wchar_t* kPluginId = L"XenoNextDay";
    const wchar_t* kVersion = L"0.4.1";

    enum class RuntimeMode
    {
        Compatible,
        Profile,
        DominatorCatchUp,
        DominatorFast
    };

    struct InstalledHook
    {
        const wchar_t* name = nullptr;
        unsigned char* address = nullptr;
        size_t length = 0;
        void* target = nullptr;
        unsigned char original[16] = {};
        void* trampoline = nullptr;
        bool installed = false;
    };

    struct StarExecutionPlan
    {
        int historicalSteps = 0;
        int firstHistoricalDay = 0;
        bool runCurrent = true;
        bool dominatorOnly = false;
    };

    XenoHostLogFn g_hostLog = nullptr;
    xnd::RuntimePoints g_points;
    xnd::LiveGameLayout g_liveLayout;
    RuntimeMode g_mode = RuntimeMode::Compatible;
    xnd::PolicySettings g_policySettings;
    int g_logEveryTurns = 20;
    InstalledHook g_nextDayHook;
    InstalledHook g_starHook;
    void* g_nextDayTrampoline = nullptr;
    void* g_starTrampoline = nullptr;
    LARGE_INTEGER g_frequency = {};
    LARGE_INTEGER g_turnStarted = {};
    void* g_currentGalaxy = nullptr;
    int g_currentGalaxyDay = 0;
    void* g_lastGalaxy = nullptr;
    int g_lastGalaxyDay = -1;
    bool g_currentGalaxyLayoutSafe = false;
    std::unordered_map<void*, xnd::CatchUpState> g_pendingCatchUp;
    SRWLOCK g_registrationLock = SRWLOCK_INIT;
    XenoNextDayFullReplacementV1 g_fullReplacement = nullptr;
    void* g_fullReplacementUser = nullptr;
    XenoNextDayStarPolicyV1 g_starPolicy = nullptr;
    void* g_starPolicyUser = nullptr;

    std::atomic<std::uint64_t> g_turns{0};
    std::atomic<std::uint64_t> g_turnMicroseconds{0};
    std::atomic<std::uint64_t> g_starCalls{0};
    std::atomic<std::uint64_t> g_starMicroseconds{0};
    std::atomic<std::uint64_t> g_coldStarCalls{0};
    std::atomic<std::uint64_t> g_skippedStarCalls{0};
    std::atomic<std::uint64_t> g_dominatorEligibleCalls{0};
    std::atomic<std::uint64_t> g_replayedStarCalls{0};
    std::atomic<std::uint64_t> g_layoutRejectedCalls{0};
    std::atomic<std::uint64_t> g_fastAggregatedDays{0};

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

    const wchar_t* ModeName(RuntimeMode mode)
    {
        switch (mode)
        {
        case RuntimeMode::Compatible:
            return L"Compatible";
        case RuntimeMode::Profile:
            return L"Profile";
        case RuntimeMode::DominatorCatchUp:
            return L"DominatorCatchUp";
        case RuntimeMode::DominatorFast:
            return L"DominatorFast";
        }
        return L"Unknown";
    }

    std::uint64_t ElapsedMicroseconds(
        const LARGE_INTEGER& begin,
        const LARGE_INTEGER& end)
    {
        if (g_frequency.QuadPart <= 0 || end.QuadPart < begin.QuadPart)
        {
            return 0;
        }
        return static_cast<std::uint64_t>(
            (end.QuadPart - begin.QuadPart) * 1000000LL / g_frequency.QuadPart);
    }

    bool BuildTrampoline(InstalledHook& hook, std::wstring& error)
    {
        if (!hook.address || hook.length < 5 || hook.length > sizeof(hook.original))
        {
            error = std::wstring(L"invalid hook metadata: ") + hook.name;
            return false;
        }
        const size_t allocationSize = hook.length + 5;
        auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
            nullptr,
            allocationSize,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE));
        if (!trampoline)
        {
            error = std::wstring(L"VirtualAlloc failed for ") + hook.name +
                L": " + std::to_wstring(GetLastError());
            return false;
        }
        std::memcpy(trampoline, hook.address, hook.length);
        trampoline[hook.length] = 0xE9;
        const intptr_t relative =
            (hook.address + hook.length) - (trampoline + hook.length + 5);
        if (relative < std::numeric_limits<std::int32_t>::min() ||
            relative > std::numeric_limits<std::int32_t>::max())
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            error = std::wstring(L"trampoline is out of range: ") + hook.name;
            return false;
        }
        const std::int32_t encoded = static_cast<std::int32_t>(relative);
        std::memcpy(trampoline + hook.length + 1, &encoded, sizeof(encoded));
        FlushInstructionCache(GetCurrentProcess(), trampoline, allocationSize);
        hook.trampoline = trampoline;
        return true;
    }

    bool InstallHook(InstalledHook& hook, std::wstring& error)
    {
        if (!hook.target || !BuildTrampoline(hook, error))
        {
            return false;
        }
        const intptr_t relative = reinterpret_cast<unsigned char*>(hook.target) -
            (hook.address + 5);
        if (relative < std::numeric_limits<std::int32_t>::min() ||
            relative > std::numeric_limits<std::int32_t>::max())
        {
            VirtualFree(hook.trampoline, 0, MEM_RELEASE);
            hook.trampoline = nullptr;
            error = std::wstring(L"hook target is out of range: ") + hook.name;
            return false;
        }
        std::memcpy(hook.original, hook.address, hook.length);
        DWORD oldProtect = 0;
        if (!VirtualProtect(
                hook.address, hook.length, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            VirtualFree(hook.trampoline, 0, MEM_RELEASE);
            hook.trampoline = nullptr;
            error = std::wstring(L"VirtualProtect failed for ") + hook.name +
                L": " + std::to_wstring(GetLastError());
            return false;
        }
        hook.address[0] = 0xE9;
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
            L" trampoline=" + Hex(hook.trampoline));
        return true;
    }

    void RestoreHook(InstalledHook& hook)
    {
        if (hook.installed)
        {
            DWORD oldProtect = 0;
            if (VirtualProtect(
                    hook.address, hook.length, PAGE_EXECUTE_READWRITE, &oldProtect))
            {
                std::memcpy(hook.address, hook.original, hook.length);
                FlushInstructionCache(GetCurrentProcess(), hook.address, hook.length);
                DWORD ignored = 0;
                VirtualProtect(hook.address, hook.length, oldProtect, &ignored);
            }
            hook.installed = false;
        }
        if (hook.trampoline)
        {
            VirtualFree(hook.trampoline, 0, MEM_RELEASE);
            hook.trampoline = nullptr;
        }
    }

    bool ReadStarSnapshot(void* star, xnd::StarSnapshot& snapshot)
    {
        __try
        {
            if (!star)
            {
                return false;
            }
            const auto* bytes = static_cast<const unsigned char*>(star);
            snapshot.star = star;
            snapshot.stableId = *reinterpret_cast<const std::int32_t*>(bytes + 0x08);
            snapshot.inactiveDays = *reinterpret_cast<const std::int32_t*>(bytes + 0x70);
            return snapshot.inactiveDays >= 0 && snapshot.inactiveDays < 10000000;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    BOOL CallFullReplacementGuarded(
        XenoNextDayFullReplacementV1 callback,
        void* galaxy,
        void* user,
        BOOL* faulted)
    {
        __try
        {
            return callback(galaxy, user);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *faulted = TRUE;
            return FALSE;
        }
    }

    int CallStarPolicyGuarded(
        XenoNextDayStarPolicyV1 callback,
        const XenoNextDayStarContextV1* context,
        void* user,
        BOOL* faulted)
    {
        __try
        {
            return callback(context, user);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *faulted = TRUE;
            return XENO_NEXTDAY_DEFAULT;
        }
    }

    bool IsDominatorMode()
    {
        return g_mode == RuntimeMode::DominatorCatchUp ||
            g_mode == RuntimeMode::DominatorFast;
    }

    void ResetCatchUp(const wchar_t* reason)
    {
        if (g_pendingCatchUp.empty())
        {
            return;
        }
        g_pendingCatchUp.clear();
        Log(std::wstring(L"catch_up=reset reason=") + reason);
    }

    BOOL WINAPI BeginGalaxyNextDay(void* galaxy)
    {
        g_currentGalaxy = galaxy;
        int day = 0;
        const bool validDay = xnd::ReadGalaxyDay(galaxy, day);
        g_currentGalaxyDay = validDay ? day : 0;
        g_currentGalaxyLayoutSafe = validDay &&
            xnd::IsExpectedGalaxy(g_liveLayout, galaxy);
        QueryPerformanceCounter(&g_turnStarted);

        if (g_mode == RuntimeMode::DominatorCatchUp)
        {
            const bool continuous = g_lastGalaxy == nullptr ||
                (g_lastGalaxy == galaxy && g_lastGalaxyDay >= 0 &&
                 g_currentGalaxyDay == g_lastGalaxyDay + 1);
            if (!continuous)
            {
                ResetCatchUp(L"galaxy_or_day_changed");
            }
            if (!g_currentGalaxyLayoutSafe)
            {
                ResetCatchUp(L"layout_not_verified");
            }
        }
        g_lastGalaxy = galaxy;
        g_lastGalaxyDay = g_currentGalaxyDay;

        XenoNextDayFullReplacementV1 replacement = nullptr;
        void* user = nullptr;
        AcquireSRWLockShared(&g_registrationLock);
        replacement = g_fullReplacement;
        user = g_fullReplacementUser;
        ReleaseSRWLockShared(&g_registrationLock);
        if (!replacement)
        {
            return TRUE;
        }
        BOOL faulted = FALSE;
        const BOOL handled = CallFullReplacementGuarded(
            replacement, galaxy, user, &faulted);
        if (faulted)
        {
            Log(L"full_replacement=exception fallback=original");
        }
        if (handled)
        {
            ResetCatchUp(L"external_full_replacement");
        }
        return handled ? FALSE : TRUE;
    }

    void WINAPI EndGalaxyNextDay()
    {
        LARGE_INTEGER ended = {};
        QueryPerformanceCounter(&ended);
        const std::uint64_t elapsed = ElapsedMicroseconds(g_turnStarted, ended);
        const std::uint64_t turns = g_turns.fetch_add(1) + 1;
        g_turnMicroseconds.fetch_add(elapsed);

        for (auto iterator = g_pendingCatchUp.begin();
             iterator != g_pendingCatchUp.end();)
        {
            if (iterator->second.lastSeenDay != g_currentGalaxyDay)
            {
                iterator = g_pendingCatchUp.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }

        if (g_logEveryTurns > 0 &&
            turns % static_cast<std::uint64_t>(g_logEveryTurns) == 0)
        {
            const std::uint64_t stars = g_starCalls.load();
            const std::uint64_t starTime = g_starMicroseconds.load();
            std::wostringstream message;
            message << L"profile turns=" << turns
                    << L" last_turn_us=" << elapsed
                    << L" avg_turn_us=" << (g_turnMicroseconds.load() / turns)
                    << L" original_star_calls=" << stars
                    << L" avg_star_us=" << (stars ? starTime / stars : 0)
                    << L" dominator_eligible=" << g_dominatorEligibleCalls.load()
                    << L" deferred=" << g_skippedStarCalls.load()
                    << L" replayed=" << g_replayedStarCalls.load()
                    << L" fast_days=" << g_fastAggregatedDays.load()
                    << L" layout_rejects=" << g_layoutRejectedCalls.load();
            Log(message.str());
        }
        g_currentGalaxy = nullptr;
        g_currentGalaxyLayoutSafe = false;
    }

    StarExecutionPlan PrepareStarNextDay(void* star, BOOL activeStar)
    {
        StarExecutionPlan plan;
        xnd::StarSnapshot snapshot;
        const bool validSnapshot = ReadStarSnapshot(star, snapshot);

        auto pending = g_pendingCatchUp.find(star);
        if (pending != g_pendingCatchUp.end() &&
            (!validSnapshot || pending->second.stableId != snapshot.stableId))
        {
            g_pendingCatchUp.erase(pending);
            pending = g_pendingCatchUp.end();
        }

        // The player's current system always runs. If it was previously
        // deferred, historical inactive calls are replayed before today's
        // active call.
        if (activeStar)
        {
            if (pending != g_pendingCatchUp.end())
            {
                plan.historicalSteps = pending->second.pendingSteps;
                plan.firstHistoricalDay = pending->second.firstDeferredDay;
                g_pendingCatchUp.erase(pending);
            }
            return plan;
        }

        xnd::LiveStarInspection inspection;
        const bool cadenceEnabled = g_policySettings.farCadenceDays > 1;
        const bool eligible = IsDominatorMode() && cadenceEnabled &&
            g_currentGalaxyLayoutSafe &&
            xnd::InspectDominatorOnlyStar(
                g_liveLayout,
                star,
                g_policySettings.farAfterInactiveDays,
                inspection);
        if (eligible)
        {
            plan.dominatorOnly = true;
            g_dominatorEligibleCalls.fetch_add(1);
            g_coldStarCalls.fetch_add(1);
        }
        else if (IsDominatorMode() && cadenceEnabled &&
                 g_currentGalaxyLayoutSafe &&
                 inspection.reason != xnd::LiveStarReason::TooNear &&
                 inspection.reason != xnd::LiveStarReason::EmptyShipList &&
                 inspection.reason != xnd::LiveStarReason::NonDominatorShip)
        {
            g_layoutRejectedCalls.fetch_add(1);
        }

        bool scheduled = !eligible || xnd::ShouldRunColdStar(
            snapshot, g_currentGalaxyDay, g_policySettings);
        const bool builtinScheduled = scheduled;

        XenoNextDayStarPolicyV1 policy = nullptr;
        void* user = nullptr;
        AcquireSRWLockShared(&g_registrationLock);
        policy = g_starPolicy;
        user = g_starPolicyUser;
        ReleaseSRWLockShared(&g_registrationLock);
        if (policy && validSnapshot)
        {
            XenoNextDayStarContextV1 context = {};
            context.size = sizeof(context);
            context.galaxy = g_currentGalaxy;
            context.star = star;
            context.starId = snapshot.stableId;
            context.inactiveDays = snapshot.inactiveDays;
            context.galaxyDay = g_currentGalaxyDay;
            context.activeStar = FALSE;
            context.scheduledByBuiltinPolicy = builtinScheduled ? TRUE : FALSE;
            context.dominatorOnly = eligible ? TRUE : FALSE;
            BOOL faulted = FALSE;
            const int decision = CallStarPolicyGuarded(
                policy, &context, user, &faulted);
            if (faulted)
            {
                Log(L"star_policy=exception fallback=builtin");
            }
            else if (decision == XENO_NEXTDAY_RUN)
            {
                scheduled = true;
            }
            else if (decision == XENO_NEXTDAY_SKIP)
            {
                scheduled = false;
            }
        }

        if (g_mode == RuntimeMode::DominatorCatchUp)
        {
            if (!scheduled)
            {
                xnd::CatchUpState& state = g_pendingCatchUp[star];
                const xnd::CatchUpPlan catchUp = xnd::AdvanceCatchUp(
                    state,
                    validSnapshot ? snapshot.stableId : 0,
                    g_currentGalaxyDay,
                    false);
                plan.runCurrent = catchUp.runCurrent;
                g_skippedStarCalls.fetch_add(1);
                return plan;
            }
            if (pending != g_pendingCatchUp.end())
            {
                const xnd::CatchUpPlan catchUp = xnd::AdvanceCatchUp(
                    pending->second,
                    snapshot.stableId,
                    g_currentGalaxyDay,
                    true);
                plan.historicalSteps = catchUp.historicalSteps;
                plan.firstHistoricalDay = catchUp.firstHistoricalDay;
                g_pendingCatchUp.erase(pending);
            }
            return plan;
        }

        if (!scheduled)
        {
            if (eligible)
            {
                if (!xnd::IncrementInactiveDays(star))
                {
                    Log(L"fast_aggregate=failed fallback=original");
                    return plan;
                }
                g_fastAggregatedDays.fetch_add(1);
            }
            plan.runCurrent = false;
            g_skippedStarCalls.fetch_add(1);
        }
        return plan;
    }

    bool CallOriginalStarStep(void* star, BOOL activeStar)
    {
        void* function = g_starTrampoline;
        if (!function || !star)
        {
            return false;
        }
        __try
        {
            __asm {
                mov eax, star
                mov edx, activeStar
                call dword ptr [function]
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void WINAPI DispatchStarNextDay(void* star, BOOL activeStar)
    {
        const StarExecutionPlan plan = PrepareStarNextDay(star, activeStar);
        const int plannedCalls = plan.historicalSteps + (plan.runCurrent ? 1 : 0);
        if (plannedCalls <= 0)
        {
            return;
        }

        LARGE_INTEGER started = {};
        const bool measure = g_mode != RuntimeMode::Compatible;
        if (measure)
        {
            QueryPerformanceCounter(&started);
        }

        int savedDay = g_currentGalaxyDay;
        bool virtualizeDay = plan.historicalSteps > 0 &&
            g_currentGalaxyLayoutSafe &&
            xnd::ReadGalaxyDay(g_currentGalaxy, savedDay);
        bool changedDay = false;
        int replayed = 0;
        for (int index = 0; index < plan.historicalSteps; ++index)
        {
            if (virtualizeDay && xnd::WriteGalaxyDay(
                    g_currentGalaxy, plan.firstHistoricalDay + index))
            {
                changedDay = true;
            }
            else if (virtualizeDay)
            {
                virtualizeDay = false;
            }
            g_starCalls.fetch_add(1);
            if (!CallOriginalStarStep(star, FALSE))
            {
                Log(L"catch_up=original_exception replay_aborted");
                break;
            }
            ++replayed;
        }
        if (changedDay)
        {
            xnd::WriteGalaxyDay(g_currentGalaxy, savedDay);
        }
        g_replayedStarCalls.fetch_add(static_cast<std::uint64_t>(replayed));

        if (plan.runCurrent)
        {
            g_starCalls.fetch_add(1);
            if (!CallOriginalStarStep(star, activeStar))
            {
                Log(L"star_next_day=original_exception");
            }
        }

        if (measure)
        {
            LARGE_INTEGER ended = {};
            QueryPerformanceCounter(&ended);
            g_starMicroseconds.fetch_add(ElapsedMicroseconds(started, ended));
        }
    }

    __declspec(naked) void GalaxyNextDayStub()
    {
        __asm {
            pushfd
            pushad
            push eax
            call BeginGalaxyNextDay
            test eax, eax
            jz skip_original
            popad
            popfd
            call dword ptr [g_nextDayTrampoline]
            jmp finish
        skip_original:
            popad
            popfd
        finish:
            pushfd
            pushad
            call EndGalaxyNextDay
            popad
            popfd
            ret
        }
    }

    __declspec(naked) void StarNextDayStub()
    {
        __asm {
            pushfd
            pushad
            movzx ecx, dl
            push ecx
            push eax
            call DispatchStarNextDay
            popad
            popfd
            ret
        }
    }

    RuntimeMode ReadMode(const XenoPluginHostV1* host)
    {
        wchar_t value[64] = L"Compatible";
        if (host->configGetString)
        {
            host->configGetString(
                host->configPath,
                L"NextDay",
                L"Mode",
                L"Compatible",
                value,
                static_cast<DWORD>(_countof(value)));
        }
        std::wstring normalized(value);
        std::transform(
            normalized.begin(), normalized.end(), normalized.begin(), towlower);
        if (normalized == L"profile")
        {
            return RuntimeMode::Profile;
        }
        if (normalized == L"dominatorcatchup" ||
            normalized == L"dominator-catch-up" ||
            normalized == L"catchup")
        {
            return RuntimeMode::DominatorCatchUp;
        }
        if (normalized == L"dominatorfast" ||
            normalized == L"dominator-fast")
        {
            return RuntimeMode::DominatorFast;
        }
        if (normalized == L"experimentalcadence" ||
            normalized == L"experimental-cadence")
        {
            return RuntimeMode::DominatorFast;
        }
        return RuntimeMode::Compatible;
    }

    int ReadInt(
        const XenoPluginHostV1* host,
        const wchar_t* key,
        int fallback,
        int minimum,
        int maximum)
    {
        return host->configGetInt
            ? host->configGetInt(
                host->configPath,
                L"NextDay",
                key,
                fallback,
                minimum,
                maximum)
            : fallback;
    }

    bool InstallRuntimeHooks(std::wstring& error)
    {
        g_starHook = {
            L"star-next-day",
            g_points.starNextDay,
            9,
            reinterpret_cast<void*>(&StarNextDayStub)
        };
        if (!InstallHook(g_starHook, error))
        {
            return false;
        }
        g_starTrampoline = g_starHook.trampoline;

        g_nextDayHook = {
            L"galaxy-next-day",
            g_points.nextDay,
            9,
            reinterpret_cast<void*>(&GalaxyNextDayStub)
        };
        if (!InstallHook(g_nextDayHook, error))
        {
            RestoreHook(g_starHook);
            g_starTrampoline = nullptr;
            return false;
        }
        g_nextDayTrampoline = g_nextDayHook.trampoline;
        return true;
    }

    BOOL WINAPI RegisterFullReplacement(
        XenoNextDayFullReplacementV1 replacement,
        void* user)
    {
        if (!replacement || !g_nextDayTrampoline)
        {
            return FALSE;
        }
        AcquireSRWLockExclusive(&g_registrationLock);
        const bool available = g_fullReplacement == nullptr;
        if (available)
        {
            g_fullReplacementUser = user;
            g_fullReplacement = replacement;
        }
        ReleaseSRWLockExclusive(&g_registrationLock);
        if (available)
        {
            Log(L"full_replacement=registered");
        }
        return available ? TRUE : FALSE;
    }

    BOOL WINAPI RegisterStarPolicy(XenoNextDayStarPolicyV1 policy, void* user)
    {
        if (!policy || !g_starTrampoline)
        {
            return FALSE;
        }
        AcquireSRWLockExclusive(&g_registrationLock);
        const bool available = g_starPolicy == nullptr;
        if (available)
        {
            g_starPolicyUser = user;
            g_starPolicy = policy;
        }
        ReleaseSRWLockExclusive(&g_registrationLock);
        if (available)
        {
            Log(L"star_policy=registered");
        }
        return available ? TRUE : FALSE;
    }

    BOOL WINAPI GetStats(XenoNextDayStatsV1* stats)
    {
        if (!stats || stats->size < XENO_NEXTDAY_STATS_V1_BASE_SIZE)
        {
            return FALSE;
        }
        stats->turns = g_turns.load();
        stats->totalTurnMicroseconds = g_turnMicroseconds.load();
        stats->starCalls = g_starCalls.load();
        stats->starMicroseconds = g_starMicroseconds.load();
        stats->coldStarCalls = g_coldStarCalls.load();
        stats->skippedStarCalls = g_skippedStarCalls.load();
        if (stats->size >= sizeof(XenoNextDayStatsV1))
        {
            stats->dominatorEligibleCalls = g_dominatorEligibleCalls.load();
            stats->replayedStarCalls = g_replayedStarCalls.load();
            stats->layoutRejectedCalls = g_layoutRejectedCalls.load();
            stats->fastAggregatedDays = g_fastAggregatedDays.load();
        }
        return TRUE;
    }

    BOOL WINAPI CallOriginalNextDay(void* galaxy)
    {
        void* function = g_nextDayTrampoline;
        if (!function || !galaxy)
        {
            return FALSE;
        }
        __try
        {
            __asm {
                mov eax, galaxy
                call dword ptr [function]
            }
            return TRUE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return FALSE;
        }
    }

    BOOL WINAPI CallOriginalStarNextDay(void* star, BOOL activeStar)
    {
        return CallOriginalStarStep(star, activeStar) ? TRUE : FALSE;
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
    lstrcpynW(info->version, kVersion, static_cast<int>(_countof(info->version)));
    lstrcpynW(
        info->description,
        L"Verified SRHD NextDay adapter with dominator-only catch-up and fast modes",
        static_cast<int>(_countof(info->description)));
    if (info->size >= sizeof(XenoPluginInfoV1))
    {
        info->exclusiveCapabilities = XENO_NEXTDAY_EXCLUSIVE_CAPABILITY;
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
    QueryPerformanceFrequency(&g_frequency);
    g_mode = ReadMode(host);
    g_policySettings.farAfterInactiveDays = ReadInt(
        host, L"FarAfterInactiveDays", 30, 1, 100000);
    g_policySettings.farCadenceDays = ReadInt(
        host, L"FarCadenceDays", 1, 1, 30);
    g_logEveryTurns = ReadInt(host, L"LogEveryTurns", 20, 0, 100000);

    std::wstring error;
    if (!xnd::DiscoverRuntimePoints(host->gameModule, g_points, error))
    {
        Log(L"runtime=failed " + error);
        return 2;
    }
    if (!xnd::DiscoverLiveGameLayout(host->gameModule, g_liveLayout, error))
    {
        Log(L"live_layout=failed " + error);
        if (IsDominatorMode())
        {
            g_mode = RuntimeMode::Profile;
            Log(L"mode=fallback_profile reason=live_layout_not_verified");
        }
    }
    else
    {
        std::wostringstream layout;
        layout << L"live_layout=verified tstar_rva=0x" << std::hex
               << std::uppercase << g_liveLayout.starClass.vmtRva
               << L" tkling_rva=0x" << g_liveLayout.dominatorClass.vmtRva;
        Log(layout.str());
    }
    Log(L"next_day=" + Hex(g_points.nextDay));
    Log(L"star_next_day=" + Hex(g_points.starNextDay));
    if (!InstallRuntimeHooks(error))
    {
        Log(L"runtime=failed " + error);
        return 3;
    }

    std::wostringstream settings;
    settings << L"runtime=installed version=" << kVersion
             << L" mode=" << ModeName(g_mode)
             << L" far_after=" << g_policySettings.farAfterInactiveDays
             << L" far_cadence=" << g_policySettings.farCadenceDays
             << L" log_every=" << g_logEveryTurns;
    Log(settings.str());
    if (g_mode == RuntimeMode::DominatorCatchUp &&
        g_policySettings.farCadenceDays > 1)
    {
        Log(L"catch_up=enabled original_calls_are_replayed_on_main_thread");
    }
    if (g_mode == RuntimeMode::DominatorFast &&
        g_policySettings.farCadenceDays > 1)
    {
        Log(L"warning=dominator_fast_changes_remote_simulation_rate");
    }
    return 0;
}

extern "C" BOOL WINAPI XenoNextDay_GetApi(XenoNextDayApiV1* api)
{
    if (!api || api->size < sizeof(XenoNextDayApiV1))
    {
        return FALSE;
    }
    api->apiVersion = XENO_NEXTDAY_API_V1;
    api->registerFullReplacement = &RegisterFullReplacement;
    api->registerStarPolicy = &RegisterStarPolicy;
    api->getStats = &GetStats;
    api->callOriginalNextDay = &CallOriginalNextDay;
    api->callOriginalStarNextDay = &CallOriginalStarNextDay;
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
