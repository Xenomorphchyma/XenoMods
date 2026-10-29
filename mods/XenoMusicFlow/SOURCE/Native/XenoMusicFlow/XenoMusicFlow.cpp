// XenoMusicFlow: smoother music between space, planets and stations.
//
// Every screen of the game asks TMusicControl.PlayCategory for the music of
// its place. When the playing track is not in that category the game fades it
// out in a second and starts another one, so a short stop on a planet loses
// the space track and a takeoff loses the planet track.
//
// The plugin defers such a switch while the old track goes on. Coming back to
// a place the track fits cancels it. After HoldSeconds the old track either
// plays to its end (the normal switch follows) or, when much of it is left,
// crossfades over CrossfadeSeconds into a track of the new place.
//
// Every category is treated as the music of a place (space, planets, stations
// of the game and of mods) except those in ImmediateCategories: battles, the
// main menu, endings and quest soundtracks switch at once. Videos and script
// music stop the old track first and a MusicControl playlist overrides
// everything, so both go through unchanged.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <oleauto.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "music_logic.h"
#include "music_scan.h"
#include "xeno_plugin_api.h"

#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")

namespace
{
    const wchar_t* kPluginId = L"XenoMusicFlow";
    const wchar_t* kPluginVersion = L"1.0.0";
    const wchar_t* kDefaultImmediate =
        L"Battle;Robot;ArcadeBattle;Base;Win;Loss;Song;QuestSoundtrack*;"
        L"Intro;Introduction;Outro;Logo;1C;Begin;Record;Hyper*;MusicOff";

    // TMusicControl
    constexpr size_t kCurrentUnit = 0x34;
    constexpr size_t kQueuedUnit = 0x38;
    constexpr size_t kCategoryOverride = 0x44;
    // TThreadEC (base of TMusicUnit)
    constexpr size_t kStopRequested = 0x11;
    constexpr size_t kStopEvent = 0x14;
    constexpr size_t kIdleEvent = 0x28;
    // TMusicUnit
    constexpr size_t kImmediateStop = 0x38;
    constexpr size_t kUnitBuffer = 0x3C;
    constexpr size_t kUnitStream = 0x48;
    constexpr size_t kStartPlaybackEvent = 0x4C;
    // TFileStreamEC
    constexpr size_t kReadBuffer = 0x34;
    constexpr size_t kReadAvailable = 0x38;
    constexpr size_t kReadPosition = 0x3C;
    constexpr size_t kFillAvailable = 0x44;
    constexpr size_t kFileSize = 0x4C;
    constexpr size_t kEndOfFile = 0x50;
    // TSoundBuffer
    constexpr size_t kVolumeStep = 0x50;
    // Vanilla fade step per 100 ms tick.
    constexpr float kVanillaStep = 0.1f;

    XenoHostLogFn g_log = nullptr;
    xmf::Settings g_settings;
    xmf::MusicPoints g_points;
    uintptr_t g_playTrampoline = 0;
    uintptr_t g_stopTrampoline = 0;
    uintptr_t g_rampTrampoline = 0;
    uintptr_t g_fadeTrampoline = 0;

    // Main-thread state, set from the PlayCategory hook and the timer.
    unsigned char* volatile g_manager = nullptr;
    DWORD g_mainThread = 0;
    UINT_PTR g_timer = 0;
    bool g_internalCall = false;
    // The playing track came from a deferrable category.
    bool g_originDeferrable = false;
    std::wstring g_pending;
    DWORD g_pendingSince = 0;
    bool g_letEndLogged = false;

    struct Crossfade
    {
        volatile bool active = false;
        unsigned char* outUnit = nullptr;
        unsigned char* inUnit = nullptr;
        unsigned char* volatile outBuffer = nullptr;
        unsigned char* volatile inBuffer = nullptr;
        volatile bool outSlowed = false;
        DWORD started = 0;
    };
    Crossfade g_crossfade;

    // Published by the sampler thread.
    volatile LONG g_remainingMs = -1;

    void Log(const std::wstring& message)
    {
        if (g_log) g_log(kPluginId, message.c_str());
    }

    template <typename T>
    T Field(const unsigned char* object, size_t offset)
    {
        T value;
        std::memcpy(&value, object + offset, sizeof(T));
        return value;
    }

    unsigned char* Unit(const unsigned char* manager, size_t offset)
    {
        return manager ? Field<unsigned char*>(manager, offset) : nullptr;
    }

    // TThreadEC.IsRunning without the lock: the idle event is reset while busy.
    bool UnitRunning(const unsigned char* unit)
    {
        if (!unit) return false;
        const HANDLE idle = Field<HANDLE>(unit, kIdleEvent);
        return idle && WaitForSingleObject(idle, 0) == WAIT_TIMEOUT;
    }

    // TThreadEC.RequestStop; the unit fades out on its next chunk.
    void RequestUnitStop(unsigned char* unit, bool immediate)
    {
        if (!unit) return;
        if (immediate) unit[kImmediateStop] = 1;
        unit[kStopRequested] = 1;
        if (const HANDLE stop = Field<HANDLE>(unit, kStopEvent)) SetEvent(stop);
        // A unit still waiting for its start gives up instead of playing.
        if (const HANDLE start = Field<HANDLE>(unit, kStartPlaybackEvent)) SetEvent(start);
    }

    std::wstring OverrideCategory(const unsigned char* manager)
    {
        const wchar_t* text = Field<const wchar_t*>(manager, kCategoryOverride);
        return text ? std::wstring(text) : std::wstring();
    }

    // Calls into Rangers.exe. Delphi raises SEH exceptions; keep them out of
    // the plugin's frames.
    bool CallGetFileName(unsigned char* unit, BSTR* result)
    {
        void* routine = g_points.getFileName;
        __try
        {
            __asm {
                mov eax, unit
                mov edx, result
                call routine
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallChooseMusicFile(BSTR category, BSTR current, BSTR* result)
    {
        void* routine = g_points.chooseMusicFile;
        __try
        {
            __asm {
                mov eax, category
                mov edx, current
                mov ecx, result
                call routine
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallOriginalPlayCategory(unsigned char* manager, BSTR category)
    {
        uintptr_t routine = g_playTrampoline;
        __try
        {
            __asm {
                mov eax, manager
                mov edx, category
                call routine
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::wstring UnitFileName(unsigned char* unit)
    {
        BSTR value = nullptr;
        if (!unit || !CallGetFileName(unit, &value)) return std::wstring();
        std::wstring result = value ? std::wstring(value) : std::wstring();
        if (value) SysFreeString(value);
        return result;
    }

    // ChooseMusicFile returns '' when the file already belongs to the category.
    bool TrackFitsCategory(const std::wstring& category, const std::wstring& file)
    {
        BSTR categoryText = SysAllocString(category.c_str());
        BSTR fileText = SysAllocString(file.c_str());
        BSTR chosen = nullptr;
        const bool called = CallChooseMusicFile(categoryText, fileText, &chosen);
        const bool fits = called && (!chosen || chosen[0] == 0);
        if (chosen) SysFreeString(chosen);
        SysFreeString(fileText);
        SysFreeString(categoryText);
        return fits;
    }

    void ClearPending()
    {
        g_pending.clear();
        g_letEndLogged = false;
    }

    // Ends a running crossfade: the incoming track stops, the outgoing one
    // fades at the normal speed.
    void AbortCrossfade(bool immediate, const wchar_t* reason)
    {
        if (!g_crossfade.active) return;
        g_crossfade.active = false;
        RequestUnitStop(g_crossfade.inUnit, immediate);
        if (g_crossfade.outSlowed && g_crossfade.outBuffer)
        {
            const float step = -kVanillaStep;
            std::memcpy(g_crossfade.outBuffer + kVolumeStep, &step, sizeof(step));
        }
        Log(std::wstring(L"crossfade=aborted reason=") + reason);
    }

    void StartCrossfade(unsigned char* manager)
    {
        unsigned char* current = Unit(manager, kCurrentUnit);
        unsigned char* queued = Unit(manager, kQueuedUnit);
        if (!UnitRunning(current) || !queued || UnitRunning(queued)) return;
        const std::wstring file = UnitFileName(current);
        if (file.empty())
        {
            ClearPending();
            return;
        }
        if (TrackFitsCategory(g_pending, file))
        {
            ClearPending();
            return;
        }
        const LONG remaining = g_remainingMs;
        g_crossfade.outUnit = current;
        g_crossfade.inUnit = queued;
        g_crossfade.outBuffer = Field<unsigned char*>(current, kUnitBuffer);
        g_crossfade.inBuffer = Field<unsigned char*>(queued, kUnitBuffer);
        g_crossfade.outSlowed = false;
        g_crossfade.started = GetTickCount();
        g_crossfade.active = true;

        BSTR category = SysAllocString(g_pending.c_str());
        g_internalCall = true;
        const bool called = CallOriginalPlayCategory(manager, category);
        g_internalCall = false;
        SysFreeString(category);

        // PlayFile left the new track waiting for the old one to finish;
        // start it now so both play together.
        const HANDLE start = Field<HANDLE>(queued, kStartPlaybackEvent);
        if (called && UnitRunning(queued) && start)
        {
            SetEvent(start);
            Log(L"crossfade=start from=" + file + L" to=" + g_pending +
                L" remaining_ms=" + std::to_wstring(remaining) +
                L" seconds=" + std::to_wstring(g_settings.crossfadeMs / 1000));
        }
        else
        {
            g_crossfade.active = false;
            Log(L"crossfade=skipped category=" + g_pending + (called ? L" no_track" : L" call_failed"));
        }
        g_originDeferrable = true;
        ClearPending();
    }

    void CheckCrossfadeDone(unsigned char* manager)
    {
        if (!g_crossfade.active) return;
        const bool swapped = Unit(manager, kCurrentUnit) == g_crossfade.inUnit &&
            !UnitRunning(g_crossfade.outUnit);
        const bool lost = !UnitRunning(g_crossfade.inUnit);
        const bool overdue = GetTickCount() - g_crossfade.started > g_settings.crossfadeMs + 10000;
        if (swapped || lost || overdue)
        {
            g_crossfade.active = false;
            Log(std::wstring(L"crossfade=done") + (swapped ? L"" : lost ? L" incoming_stopped" : L" timeout"));
        }
    }

    void CALLBACK MusicTimer(HWND, UINT, UINT_PTR, DWORD)
    {
        unsigned char* manager = g_manager;
        if (!manager) return;
        CheckCrossfadeDone(manager);
        if (g_pending.empty() || g_crossfade.active) return;
        if (!OverrideCategory(manager).empty() || !UnitRunning(Unit(manager, kCurrentUnit)))
        {
            // A playlist took over, or the track ended and the game's own
            // switch picks the music of the current place.
            ClearPending();
            return;
        }
        const LONG remaining = g_remainingMs;
        switch (xmf::DecidePending(GetTickCount() - g_pendingSince, remaining, g_settings))
        {
        case xmf::PendingAction::Wait:
            break;
        case xmf::PendingAction::LetEnd:
            if (!g_letEndLogged)
            {
                g_letEndLogged = true;
                Log(L"pending=let_end category=" + g_pending + L" remaining_ms=" + std::to_wstring(remaining));
            }
            break;
        case xmf::PendingAction::Crossfade:
            StartCrossfade(manager);
            break;
        }
    }

    void EnsureTimer()
    {
        if (g_timer) return;
        if (!g_mainThread) g_mainThread = GetCurrentThreadId();
        if (g_mainThread != GetCurrentThreadId()) return;
        g_timer = SetTimer(nullptr, 0, 100, MusicTimer);
        Log(g_timer ? L"timer=started" : L"timer=failed");
    }

    // Returns nonzero when the game's PlayCategory should run.
    BOOL __stdcall OnPlayCategory(unsigned char* manager, const wchar_t* categoryText)
    {
        if (g_internalCall || !manager) return TRUE;
        g_manager = manager;
        EnsureTimer();
        const std::wstring category = categoryText ? categoryText : L"";
        unsigned char* current = Unit(manager, kCurrentUnit);
        unsigned char* queued = Unit(manager, kQueuedUnit);
        if (!current || !queued) return TRUE; // music disabled in the settings

        if (!OverrideCategory(manager).empty())
        {
            AbortCrossfade(true, L"playlist");
            ClearPending();
            g_originDeferrable = false;
            return TRUE;
        }
        const bool deferrable = xmf::IsPlaceCategory(category, g_settings);
        unsigned char* playing = g_crossfade.active ? g_crossfade.inUnit : current;
        const bool transition = !g_crossfade.active && UnitRunning(queued);
        if (!deferrable || !g_originDeferrable || !UnitRunning(playing) || transition)
        {
            AbortCrossfade(true, L"switch");
            if (!g_pending.empty()) Log(L"pending=dropped category=" + g_pending + L" by=" + category);
            ClearPending();
            g_originDeferrable = deferrable;
            return TRUE;
        }
        const std::wstring file = UnitFileName(playing);
        if (file.empty()) return TRUE;
        if (TrackFitsCategory(category, file))
        {
            if (!g_pending.empty()) Log(L"pending=cancelled track_fits=" + category);
            ClearPending();
            return FALSE;
        }
        if (g_pending.empty())
        {
            g_pendingSince = GetTickCount();
            Log(L"pending=start category=" + category + L" track=" + file);
        }
        else if (g_pending != category)
        {
            Log(L"pending=changed category=" + category);
        }
        g_pending = category;
        g_letEndLogged = false;
        return FALSE;
    }

    DWORD __stdcall OnRampStep(unsigned char* buffer, DWORD intervalMs, DWORD stepBits)
    {
        if (!g_crossfade.active) return stepBits;
        float step = 0;
        std::memcpy(&step, &stepBits, sizeof(step));
        float adjusted = step;
        if (buffer == g_crossfade.outBuffer && step < 0)
        {
            // A second stop request (MusicControl "next") keeps the normal speed.
            if (!g_crossfade.outSlowed)
            {
                g_crossfade.outSlowed = true;
                adjusted = xmf::CrossfadeStep(intervalMs, step, g_settings.crossfadeMs);
            }
        }
        else if (buffer == g_crossfade.inBuffer && step > 0)
        {
            adjusted = xmf::CrossfadeStep(intervalMs, step, g_settings.crossfadeMs);
        }
        DWORD bits = 0;
        std::memcpy(&bits, &adjusted, sizeof(bits));
        return bits;
    }

    // Taking off and the loading screen between places fade the music out
    // directly; the place switch that follows goes through PlayCategory.
    // Keep a place track playing there and let that switch decide.
    BOOL __stdcall OnRequestFadeOut(unsigned char* manager, const unsigned char* returnAddress)
    {
        const xmf::PlaceFade* place = nullptr;
        for (const auto& fade : g_points.placeFades)
        {
            if (fade.returnAddress == returnAddress) place = &fade;
        }
        if (!place || !manager) return TRUE;
        g_manager = manager;
        if (!OverrideCategory(manager).empty() || !g_originDeferrable || g_crossfade.active ||
            !UnitRunning(Unit(manager, kCurrentUnit)) || UnitRunning(Unit(manager, kQueuedUnit)))
        {
            return TRUE;
        }
        static DWORD lastLog = 0;
        if (GetTickCount() - lastLog > 2000)
        {
            lastLog = GetTickCount();
            Log(std::wstring(L"fade=kept site=") +
                (place->site == xmf::FadeSite::Takeoff ? L"takeoff" : L"loading"));
        }
        return FALSE;
    }

    // Runs before the game's StopImmediately. Callers then wait until no
    // music buffer plays, so the incoming track has to stop as well.
    void __stdcall OnStopImmediately(unsigned char* manager)
    {
        if (manager) g_manager = manager;
        AbortCrossfade(true, L"stop_immediately");
        ClearPending();
        g_originDeferrable = false;
    }

    __declspec(naked) void PlayCategoryStub()
    {
        __asm {
            pushad
            // Delphi register convention: EAX=Self, EDX=Category.
            push dword ptr [esp+14h]
            push dword ptr [esp+20h]
            call OnPlayCategory
            test eax, eax
            popad
            jz skip_original
            jmp dword ptr [g_playTrampoline]
        skip_original:
            ret
        }
    }

    __declspec(naked) void VolumeRampStub()
    {
        __asm {
            pushad
            // EAX=TSoundBuffer, EDX=Interval, Step on the stack above the
            // return address (pushad frame is 20h bytes).
            push dword ptr [esp+24h]
            push dword ptr [esp+18h]
            push dword ptr [esp+24h]
            call OnRampStep
            mov dword ptr [esp+24h], eax
            popad
            jmp dword ptr [g_rampTrampoline]
        }
    }

    __declspec(naked) void StopImmediatelyStub()
    {
        __asm {
            pushad
            push dword ptr [esp+1Ch]
            call OnStopImmediately
            popad
            jmp dword ptr [g_stopTrampoline]
        }
    }

    __declspec(naked) void RequestFadeOutStub()
    {
        __asm {
            pushad
            // EAX=Self; the caller's return address sits above the pushad frame.
            push dword ptr [esp+20h]
            push dword ptr [esp+20h]
            call OnRequestFadeOut
            test eax, eax
            popad
            jz skip_original
            jmp dword ptr [g_fadeTrampoline]
        skip_original:
            ret
        }
    }

    bool InstallHook(unsigned char* address, size_t length, void* stub, uintptr_t& trampolineOut,
        std::wstring& error)
    {
        if (!address || length < 5 || length > 16)
        {
            error = L"bad hook site";
            return false;
        }
        auto* trampoline = static_cast<unsigned char*>(
            VirtualAlloc(nullptr, length + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!trampoline)
        {
            error = L"VirtualAlloc failed: " + std::to_wstring(GetLastError());
            return false;
        }
        std::memcpy(trampoline, address, length);
        trampoline[length] = 0xE9;
        const std::int32_t back = static_cast<std::int32_t>((address + length) - (trampoline + length + 5));
        std::memcpy(trampoline + length + 1, &back, sizeof(back));
        FlushInstructionCache(GetCurrentProcess(), trampoline, length + 5);

        DWORD oldProtect = 0;
        if (!VirtualProtect(address, length, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            error = L"VirtualProtect failed: " + std::to_wstring(GetLastError());
            return false;
        }
        trampolineOut = reinterpret_cast<uintptr_t>(trampoline);
        const std::int32_t jump = static_cast<std::int32_t>(static_cast<unsigned char*>(stub) - (address + 5));
        address[0] = 0xE9;
        std::memcpy(address + 1, &jump, sizeof(jump));
        for (size_t index = 5; index < length; ++index) address[index] = 0x90;
        FlushInstructionCache(GetCurrentProcess(), address, length);
        DWORD ignored = 0;
        VirtualProtect(address, length, oldProtect, &ignored);
        return true;
    }

    struct StreamSample
    {
        const void* stream;
        const void* readBuffer;
        unsigned readPosition;
        unsigned readAvailable;
        unsigned fillAvailable;
        unsigned fileSize;
        bool endOfFile;
    };

    // The unit thread may free the stream at any moment; a stale read only
    // gives a wrong sample.
    bool SampleStream(unsigned char* manager, StreamSample& sample)
    {
        __try
        {
            unsigned char* unit = g_crossfade.active ? g_crossfade.inUnit : Unit(manager, kCurrentUnit);
            if (!UnitRunning(unit)) return false;
            unsigned char* stream = Field<unsigned char*>(unit, kUnitStream);
            if (!stream) return false;
            sample.stream = stream;
            sample.readBuffer = Field<const void*>(stream, kReadBuffer);
            sample.readPosition = Field<unsigned>(stream, kReadPosition);
            sample.readAvailable = Field<unsigned>(stream, kReadAvailable);
            sample.fillAvailable = Field<unsigned>(stream, kFillAvailable);
            sample.fileSize = Field<unsigned>(stream, kFileSize);
            sample.endOfFile = stream[kEndOfFile] != 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Samples on its own thread so a busy main thread (loading a save) does
    // not miss the stream's buffer swaps.
    DWORD WINAPI SamplerThread(void*)
    {
        xmf::TrackProgress progress;
        for (;;)
        {
            Sleep(200);
            unsigned char* manager = g_manager;
            StreamSample sample = {};
            if (!manager || !SampleStream(manager, sample))
            {
                progress.Reset();
                InterlockedExchange(&g_remainingMs, -1);
                continue;
            }
            progress.Observe(sample.stream, sample.readBuffer, sample.readPosition,
                sample.readAvailable, sample.fillAvailable, sample.fileSize,
                sample.endOfFile, GetTickCount());
            const long long remaining = progress.RemainingMs();
            InterlockedExchange(&g_remainingMs, remaining < 0 ? -1
                : static_cast<LONG>((std::min)(remaining, 0x7FFFFFFFll)));
        }
    }

    int ReadSeconds(const XenoPluginHostV1* host, const wchar_t* key, int fallback, int minimum, int maximum)
    {
        return host->size >= sizeof(XenoPluginHostV1) && host->configGetInt
            ? host->configGetInt(host->configPath, L"MusicFlow", key, fallback, minimum, maximum)
            : fallback;
    }

    std::wstring ReadString(const XenoPluginHostV1* host, const wchar_t* key, const wchar_t* fallback)
    {
        wchar_t buffer[1024] = {};
        if (host->size >= sizeof(XenoPluginHostV1) && host->configGetString)
        {
            host->configGetString(host->configPath, L"MusicFlow", key, fallback, buffer, _countof(buffer));
            return buffer;
        }
        return fallback;
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
    lstrcpynW(info->description, L"Keeps music playing across short stops and crossfades between places", 160);
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
    g_settings.holdMs = ReadSeconds(host, L"HoldSeconds", 45, 0, 600) * 1000u;
    g_settings.crossfadeMs = ReadSeconds(host, L"CrossfadeSeconds", 8, 1, 60) * 1000u;
    g_settings.endGraceMs = ReadSeconds(host, L"EndGraceSeconds", 40, 0, 600) * 1000u;
    const std::wstring immediate = ReadString(host, L"ImmediateCategories", kDefaultImmediate);
    g_settings.immediatePatterns = xmf::SplitPatterns(immediate);

    std::wstring error;
    if (!xmf::FindMusicPoints(reinterpret_cast<unsigned char*>(host->gameModule), g_points, error))
    {
        Log(L"runtime=blocked reason=" + error);
        return 2;
    }
    // Hook the ramp first: until PlayCategory is hooked it never sees a crossfade.
    if (!InstallHook(g_points.volumeRamp, g_points.rampLength, &VolumeRampStub, g_rampTrampoline, error) ||
        !InstallHook(g_points.stopImmediately, g_points.stopLength, &StopImmediatelyStub, g_stopTrampoline, error) ||
        !InstallHook(g_points.requestFadeOut, g_points.fadeLength, &RequestFadeOutStub, g_fadeTrampoline, error) ||
        !InstallHook(g_points.playCategory, g_points.playLength, &PlayCategoryStub, g_playTrampoline, error))
    {
        Log(L"runtime=failed reason=" + error);
        return 3;
    }
    HANDLE thread = CreateThread(nullptr, 0, SamplerThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    size_t takeoffs = 0;
    for (const auto& fade : g_points.placeFades)
    {
        if (fade.site == xmf::FadeSite::Takeoff) ++takeoffs;
    }
    Log(std::wstring(L"runtime=installed version=") + kPluginVersion + L" profile=" + g_points.profile +
        L" takeoff_fades=" + std::to_wstring(takeoffs) +
        L" loading_fades=" + std::to_wstring(g_points.placeFades.size() - takeoffs) +
        L" hold_s=" + std::to_wstring(g_settings.holdMs / 1000) +
        L" crossfade_s=" + std::to_wstring(g_settings.crossfadeMs / 1000) +
        L" end_grace_s=" + std::to_wstring(g_settings.endGraceMs / 1000) +
        L" immediate=" + immediate);
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
