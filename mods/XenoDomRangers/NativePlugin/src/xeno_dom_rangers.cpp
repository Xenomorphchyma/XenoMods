#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "mask_policy.h"
#include "runtime_scan.h"
#include "xdr_config.h"
#include "../include/xeno_plugin_api.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace
{
#ifdef XDR_PLUGIN_TEST
    const wchar_t* kPluginId = L"XenoDomRangersTest";
    const wchar_t* kPluginDescription =
        L"Native NPC chameleon runtime for XenoDomRangersTest";
#else
    const wchar_t* kPluginId = L"XenoDomRangers";
    const wchar_t* kPluginDescription =
        L"Native NPC chameleon runtime for XenoDomRangers";
#endif
    const wchar_t* kPluginVersion = L"2.2.0";

    constexpr size_t kHookLength = 6;
    constexpr size_t kMaxSessions = 8;
    constexpr size_t kShipTypeOffset = 0x10;
    constexpr size_t kChameleonOffset = 0x461;
    constexpr size_t kChameleonSeriesOffset = 0x462;
    constexpr size_t kChameleonDetectedOffset = 0x468;
    constexpr size_t kKlingSeriesOffset = 0x4D1;
    constexpr size_t kChameleonLogicOffset = 0xDEC;
    constexpr size_t kRequiredShipBytes = kChameleonLogicOffset + 3;

    struct MaskSession
    {
        void* ship = nullptr;
        int shipId = 0;
        unsigned char rangerType = 0;
        unsigned char klingType = 0;
        unsigned char oldActive = 0;
        unsigned char oldSeries = 0;
        std::array<unsigned char, 3> oldDetected = {};
        std::array<unsigned char, 3> oldLogic = {};
        std::uint64_t lastTouched = 0;
        bool occupied = false;
    };

    XenoHostLogFn g_hostLog = nullptr;
    HMODULE g_gameModule = nullptr;
    size_t g_gameImageSize = 0;
    xdr::RuntimePoints g_points;
    std::array<MaskSession, kMaxSessions> g_sessions = {};
    SRWLOCK g_sessionsLock = SRWLOCK_INIT;
    volatile LONG g_ready = 0;
    uintptr_t g_trampoline = 0;
    std::vector<unsigned char> g_original;
    HANDLE g_hookOwner = nullptr;
    xdr::UserConfig g_userConfig;
    void* g_worldPlayer = nullptr;
    int g_worldPlayerId = 0;
    int g_worldTurn = -1;
    std::uint64_t g_sessionSerial = 0;

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

    bool IsReadableWritableRange(const void* pointer, size_t length)
    {
        if (!pointer || length == 0)
        {
            return false;
        }
        const auto start = reinterpret_cast<std::uintptr_t>(pointer);
        if (start > std::numeric_limits<std::uintptr_t>::max() - length)
        {
            return false;
        }
        MEMORY_BASIC_INFORMATION info = {};
        if (!VirtualQuery(pointer, &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) ||
            (info.Protect & PAGE_NOACCESS))
        {
            return false;
        }
        const DWORD protection = info.Protect & 0xFF;
        const bool writable = protection == PAGE_READWRITE ||
            protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READWRITE ||
            protection == PAGE_EXECUTE_WRITECOPY;
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) +
            info.RegionSize;
        return writable && start + length <= regionEnd;
    }

    bool IsGameVtable(const void* object)
    {
        if (!object || !g_gameModule || !g_gameImageSize)
        {
            return false;
        }
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(object);
        const auto image = reinterpret_cast<std::uintptr_t>(g_gameModule);
        return vtable >= image && vtable < image + g_gameImageSize;
    }

    bool IsExpectedShip(void* ship, unsigned char expectedType)
    {
        __try
        {
            if (!IsReadableWritableRange(ship, kRequiredShipBytes) ||
                !IsGameVtable(ship))
            {
                return false;
            }
            const auto* bytes = static_cast<const unsigned char*>(ship);
            return bytes[kShipTypeOffset] == expectedType;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    MaskSession* FindSession(void* ship)
    {
        for (MaskSession& session : g_sessions)
        {
            if (session.occupied && session.ship == ship)
            {
                return &session;
            }
        }
        return nullptr;
    }

    MaskSession* FindFreeSession()
    {
        for (MaskSession& session : g_sessions)
        {
            if (!session.occupied)
            {
                return &session;
            }
        }
        return nullptr;
    }

    MaskSession* FindSessionById(int shipId)
    {
        for (MaskSession& session : g_sessions)
        {
            if (session.occupied && session.shipId == shipId)
            {
                return &session;
            }
        }
        return nullptr;
    }

    MaskSession* FindOldestSession()
    {
        MaskSession* oldest = nullptr;
        for (MaskSession& session : g_sessions)
        {
            if (!session.occupied)
            {
                continue;
            }
            if (!oldest || session.lastTouched < oldest->lastTouched)
            {
                oldest = &session;
            }
        }
        return oldest;
    }

    void CaptureBaseline(
        MaskSession& session,
        unsigned char* bytes,
        int series,
        bool resumeExisting)
    {
        const bool loadedManaged = xdr::IsPersistedManagedMask(
            resumeExisting,
            series,
            bytes[kChameleonOffset] != 0,
            bytes[kChameleonSeriesOffset],
            bytes[kChameleonDetectedOffset + series] != 0,
            bytes[kChameleonLogicOffset + series]);
        if (loadedManaged)
        {
            // NPC rangers have no native chameleon baseline. The script's
            // persisted ghost flag is the authority that this exact pattern is
            // ours, so reconstruct the clean state instead of saving the mask
            // as the value that Clear would later restore.
            session.oldActive = 0;
            session.oldSeries = 0;
            session.oldDetected = {};
            session.oldLogic = {};
            return;
        }
        session.oldActive = bytes[kChameleonOffset];
        session.oldSeries = bytes[kChameleonSeriesOffset];
        std::memcpy(
            session.oldDetected.data(),
            bytes + kChameleonDetectedOffset,
            session.oldDetected.size());
        std::memcpy(
            session.oldLogic.data(),
            bytes + kChameleonLogicOffset,
            session.oldLogic.size());
    }

    void WriteActiveMask(MaskSession& session, int series)
    {
        auto* bytes = static_cast<unsigned char*>(session.ship);
        bytes[kChameleonOffset] = 1;
        bytes[kChameleonSeriesOffset] = static_cast<unsigned char>(series);
        bytes[kChameleonDetectedOffset + series] = 0;
        bytes[kChameleonLogicOffset + series] = 2;
        session.lastTouched = ++g_sessionSerial;
    }

    void RestoreMask(MaskSession& session)
    {
        if (IsExpectedShip(session.ship, session.rangerType))
        {
            auto* bytes = static_cast<unsigned char*>(session.ship);
            bytes[kChameleonOffset] = session.oldActive;
            bytes[kChameleonSeriesOffset] = session.oldSeries;
            std::memcpy(
                bytes + kChameleonDetectedOffset,
                session.oldDetected.data(),
                session.oldDetected.size());
            std::memcpy(
                bytes + kChameleonLogicOffset,
                session.oldLogic.data(),
                session.oldLogic.size());
        }
        session = {};
    }

    int __cdecl TryNpcChameleon(void* kling, void* target)
    {
        // -1 delegates to the original game function, 0/1 is a handled result.
        if (!target || !kling || !g_ready)
        {
            return -1;
        }
        int result = -1;
        AcquireSRWLockShared(&g_sessionsLock);
        MaskSession* session = FindSession(target);
        if (session)
        {
            __try
            {
                auto* targetBytes = static_cast<unsigned char*>(target);
                auto* klingBytes = static_cast<unsigned char*>(kling);
                if (IsExpectedShip(target, session->rangerType) &&
                    IsReadableWritableRange(kling, kKlingSeriesOffset + 1) &&
                    IsGameVtable(kling) &&
                    klingBytes[kShipTypeOffset] == session->klingType)
                {
                    const int series = klingBytes[kKlingSeriesOffset];
                    if (series >= 0 && series <= 2)
                    {
                        result = xdr::IsConfusedByChameleon(
                            series,
                            targetBytes[kChameleonOffset] != 0,
                            targetBytes[kChameleonSeriesOffset],
                            targetBytes[kChameleonDetectedOffset + series] != 0,
                            targetBytes[kChameleonLogicOffset + series]) ? 1 : 0;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                result = -1;
            }
        }
        ReleaseSRWLockShared(&g_sessionsLock);
        return result;
    }

    __declspec(naked) void ChameleonConfusionStub()
    {
        __asm {
            pushad
            // Delphi register calling convention: EAX=TKling, EDX=target.
            // pushad stores EDX at +14h and EAX at +1Ch; after the first push
            // the saved EAX moves to +20h relative to the new ESP.
            push dword ptr [esp+14h]
            push dword ptr [esp+20h]
            call TryNpcChameleon
            add esp, 8
            cmp eax, -1
            je run_original
            mov dword ptr [esp+1Ch], eax
            popad
            ret
        run_original:
            popad
            jmp dword ptr [g_trampoline]
        }
    }

    bool BuildAndInstallHook(std::wstring& error)
    {
        unsigned char* address = g_points.chameleonConfusion;
        if (!address)
        {
            error = L"chameleon hook address is null";
            return false;
        }
        auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
            nullptr,
            kHookLength + 5,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE));
        if (!trampoline)
        {
            error = L"VirtualAlloc failed: " + std::to_wstring(GetLastError());
            return false;
        }
        std::memcpy(trampoline, address, kHookLength);
        trampoline[kHookLength] = 0xE9;
        const intptr_t backRelative = (address + kHookLength) -
            (trampoline + kHookLength + 5);
        if (backRelative < std::numeric_limits<std::int32_t>::min() ||
            backRelative > std::numeric_limits<std::int32_t>::max())
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            error = L"trampoline is outside x86 near-jump range";
            return false;
        }
        const auto backEncoded = static_cast<std::int32_t>(backRelative);
        std::memcpy(trampoline + kHookLength + 1, &backEncoded, sizeof(backEncoded));
        FlushInstructionCache(GetCurrentProcess(), trampoline, kHookLength + 5);

        const intptr_t hookRelative =
            reinterpret_cast<unsigned char*>(&ChameleonConfusionStub) - (address + 5);
        if (hookRelative < std::numeric_limits<std::int32_t>::min() ||
            hookRelative > std::numeric_limits<std::int32_t>::max())
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            error = L"hook stub is outside x86 near-jump range";
            return false;
        }
        g_original.assign(address, address + kHookLength);
        DWORD oldProtect = 0;
        if (!VirtualProtect(address, kHookLength, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            error = L"VirtualProtect failed: " + std::to_wstring(GetLastError());
            return false;
        }
        address[0] = 0xE9;
        const auto hookEncoded = static_cast<std::int32_t>(hookRelative);
        std::memcpy(address + 1, &hookEncoded, sizeof(hookEncoded));
        address[5] = 0x90;
        FlushInstructionCache(GetCurrentProcess(), address, kHookLength);
        DWORD ignored = 0;
        VirtualProtect(address, kHookLength, oldProtect, &ignored);
        g_trampoline = reinterpret_cast<uintptr_t>(trampoline);
        return true;
    }

    size_t GetImageSize(HMODULE module)
    {
        if (!module)
        {
            return 0;
        }
        const auto* base = reinterpret_cast<const unsigned char*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return 0;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE
            ? nt->OptionalHeader.SizeOfImage
            : 0;
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
    lstrcpynW(info->version, kPluginVersion, static_cast<int>(_countof(info->version)));
    lstrcpynW(
        info->description,
        kPluginDescription,
        static_cast<int>(_countof(info->description)));
    if (info->size >= sizeof(XenoPluginInfoV1)) info->exclusiveCapabilities = 0;
    return TRUE;
}

extern "C" DWORD WINAPI XenoPlugin_Initialize(const XenoPluginHostV1* host)
{
    if (!host || host->apiVersion < XENO_NATIVE_HOST_API_V1 ||
        host->size < XENO_PLUGIN_HOST_V1_BASE_SIZE || !host->gameModule)
    {
        return 1;
    }
    g_hostLog = host->log;
    g_gameModule = host->gameModule;
    xdr::LoadUserConfig(host, g_userConfig);
    g_gameImageSize = GetImageSize(host->gameModule);
    if (!g_gameImageSize)
    {
        Log(L"runtime=failed invalid game image");
        return 2;
    }

    g_hookOwner = CreateMutexW(nullptr, FALSE, L"Local\\XenoDomRangersNpcChameleonV1");
    if (!g_hookOwner || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        Log(L"runtime=failed hook already owned by another XenoDomRangers module");
        return 3;
    }

    std::wstring error;
    if (!xdr::DiscoverRuntimePoints(host->gameModule, g_points, error))
    {
        Log(L"runtime=failed " + error);
        return 4;
    }
    if (!BuildAndInstallHook(error))
    {
        Log(L"runtime=failed " + error);
        return 5;
    }
    InterlockedExchange(&g_ready, 1);
    Log(L"runtime=installed version=2.2.0 chameleon_check=" +
        Hex(g_points.chameleonConfusion) +
        L" max_specialists=" + std::to_wstring(g_userConfig.maximumSpecialists) +
        L" recruit_chance=" + std::to_wstring(g_userConfig.recruitmentChancePercent) +
        L" salvage_turns=" + std::to_wstring(g_userConfig.maximumSalvageTurns));
    return 0;
}

extern "C" int __cdecl XenoDomRangers_IsReady()
{
    return g_ready ? 1 : 0;
}

extern "C" int __cdecl XenoDomRangers_GetConfigInt(int key)
{
    if (!g_ready) return -1;
    return xdr::GetUserConfigValue(g_userConfig, key);
}

extern "C" int __cdecl XenoDomRangers_SetNpcChameleon(
    DWORD shipValue,
    int shipId,
    int series,
    int rangerType,
    int klingType,
    int resumeExisting)
{
    if (!g_ready || !shipValue || shipId <= 1 || series < 0 || series > 2 ||
        rangerType < 0 || rangerType > 255 || klingType < 0 || klingType > 255)
    {
        return 0;
    }
    void* ship = reinterpret_cast<void*>(static_cast<std::uintptr_t>(shipValue));
    const auto expectedRanger = static_cast<unsigned char>(rangerType);
    if (!IsExpectedShip(ship, expectedRanger))
    {
        return 0;
    }

    int result = 0;
    AcquireSRWLockExclusive(&g_sessionsLock);
    MaskSession* session = FindSession(ship);
    if (session && session->shipId != shipId)
    {
        // Pointer reuse after a destroyed/reloaded ship: never write an old
        // snapshot into a new object.
        *session = {};
        session = nullptr;
    }
    if (!session)
    {
        session = FindSessionById(shipId);
        if (session)
        {
            // The same persistent ship was resolved at a new address after a
            // save load. Its original baseline remains valid; only rebind the
            // live pointer after validating the object above.
            session->ship = ship;
        }
    }
    if (!session)
    {
        session = FindFreeSession();
        if (!session)
        {
            session = FindOldestSession();
            if (session)
            {
                // Eight slots are well above the public maximum of three. If a
                // damaged script nevertheless leaks registrations, restore the
                // oldest live object before recycling its slot.
                if (session->ship && IsExpectedShip(session->ship, session->rangerType))
                {
                    RestoreMask(*session);
                }
                else
                {
                    *session = {};
                }
            }
        }
        if (session)
        {
            auto* bytes = static_cast<unsigned char*>(ship);
            session->ship = ship;
            session->shipId = shipId;
            session->rangerType = expectedRanger;
            session->klingType = static_cast<unsigned char>(klingType);
            CaptureBaseline(*session, bytes, series, resumeExisting != 0);
            session->lastTouched = ++g_sessionSerial;
            session->occupied = true;
        }
    }
    if (session && session->shipId == shipId &&
        session->rangerType == expectedRanger &&
        session->klingType == static_cast<unsigned char>(klingType))
    {
        WriteActiveMask(*session, series);
        result = 1;
    }
    ReleaseSRWLockExclusive(&g_sessionsLock);
    return result;
}

extern "C" int __cdecl XenoDomRangers_ClearNpcChameleon(
    DWORD shipValue,
    int shipId,
    int rangerType)
{
    if (!shipValue || shipId <= 1 || rangerType < 0 || rangerType > 255)
    {
        return 0;
    }
    void* ship = reinterpret_cast<void*>(static_cast<std::uintptr_t>(shipValue));
    int result = 0;
    AcquireSRWLockExclusive(&g_sessionsLock);
    MaskSession* session = FindSession(ship);
    if (!session)
    {
        session = FindSessionById(shipId);
        if (session)
        {
            session->ship = ship;
        }
    }
    if (session && session->shipId == shipId &&
        session->rangerType == static_cast<unsigned char>(rangerType))
    {
        RestoreMask(*session);
        result = 1;
    }
    else if (!session && IsExpectedShip(
        ship,
        static_cast<unsigned char>(rangerType)))
    {
        // A save can be loaded into a fresh process before its active session
        // is reasserted. Clear only the exact managed pattern and leave every
        // unrelated ship byte untouched.
        auto* bytes = static_cast<unsigned char*>(ship);
        const int series = bytes[kChameleonSeriesOffset];
        if (series >= 0 && series <= 2 && xdr::IsPersistedManagedMask(
            true,
            series,
            bytes[kChameleonOffset] != 0,
            series,
            bytes[kChameleonDetectedOffset + series] != 0,
            bytes[kChameleonLogicOffset + series]))
        {
            bytes[kChameleonOffset] = 0;
            bytes[kChameleonSeriesOffset] = 0;
            for (int index = 0; index < 3; ++index)
            {
                if (bytes[kChameleonDetectedOffset + index] == 0 &&
                    bytes[kChameleonLogicOffset + index] == 2)
                {
                    bytes[kChameleonLogicOffset + index] = 0;
                }
            }
            result = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_sessionsLock);
    return result;
}

extern "C" int __cdecl XenoDomRangers_ResetSessions()
{
    AcquireSRWLockExclusive(&g_sessionsLock);
    for (MaskSession& session : g_sessions)
    {
        // Loading a save invalidates old pointers. Drop registrations without
        // dereferencing or restoring them; the script re-registers live ships.
        session = {};
    }
    ReleaseSRWLockExclusive(&g_sessionsLock);
    return g_ready ? 1 : 0;
}

extern "C" int __cdecl XenoDomRangers_WorldTick(
    DWORD playerValue,
    int playerId,
    int turn)
{
    if (!g_ready || !playerValue || playerId <= 1 || turn < 0)
    {
        return 0;
    }
    void* player = reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(playerValue));
    AcquireSRWLockExclusive(&g_sessionsLock);
    const bool worldChanged = g_worldPlayer &&
        (g_worldPlayer != player || g_worldPlayerId != playerId ||
         turn <= g_worldTurn);
    if (worldChanged)
    {
        // Never dereference ship addresses owned by an unloaded galaxy. Active
        // script slots rebuild sessions from persistent IDs and ghost flags.
        for (MaskSession& session : g_sessions)
        {
            session = {};
        }
    }
    g_worldPlayer = player;
    g_worldPlayerId = playerId;
    g_worldTurn = turn;
    ReleaseSRWLockExclusive(&g_sessionsLock);
    return worldChanged ? 2 : 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
