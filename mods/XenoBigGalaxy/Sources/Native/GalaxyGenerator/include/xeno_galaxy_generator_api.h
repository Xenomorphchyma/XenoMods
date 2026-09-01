#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

static const std::uint32_t XGG_API_V1 = 1;

// pushfd + pushad layout at the original Delphi TThread.Execute entry.
struct XggSavedRegistersV1
{
    std::uint32_t edi;
    std::uint32_t esi;
    std::uint32_t ebp;
    std::uint32_t originalEsp;
    std::uint32_t ebx;
    std::uint32_t edx;
    std::uint32_t ecx;
    std::uint32_t eax;
    std::uint32_t eflags;
};

typedef int (WINAPI* XggSectorSelectorV1)(
    int starIndex,
    int plannedSector,
    int sectorCount,
    void* galaxy,
    void* userData);

// Return TRUE only after the callback has created the complete game world.
// FALSE executes the original generator through a verified trampoline.
typedef BOOL (WINAPI* XggFullGeneratorV1)(
    const XggSavedRegistersV1* registers,
    void* userData);

struct XggApiTableV1
{
    std::uint32_t size;
    std::uint32_t version;
    HMODULE gameModule;
    void* originalFullGenerator;
    BOOL (WINAPI* registerSectorSelector)(
        XggSectorSelectorV1 callback,
        void* userData);
    BOOL (WINAPI* registerFullGenerator)(
        XggFullGeneratorV1 callback,
        void* userData);
    int (WINAPI* getSectorCount)();
    int (WINAPI* getPlannedSector)(int starIndex);
};

typedef BOOL (WINAPI* XenoGalaxyGeneratorGetApiFn)(XggApiTableV1* api);
