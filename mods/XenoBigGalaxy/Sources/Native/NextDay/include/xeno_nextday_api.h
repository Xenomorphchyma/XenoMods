#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstddef>
#include <cstdint>

static const std::uint32_t XENO_NEXTDAY_API_V1 = 1;
static const std::uint64_t XENO_NEXTDAY_EXCLUSIVE_CAPABILITY = 1ull << 1;

enum XenoNextDayDecisionV1 : int
{
    XENO_NEXTDAY_DEFAULT = -1,
    XENO_NEXTDAY_SKIP = 0,
    XENO_NEXTDAY_RUN = 1
};

struct XenoNextDayStarContextV1
{
    std::uint32_t size;
    void* galaxy;
    void* star;
    std::int32_t starId;
    std::int32_t inactiveDays;
    std::int32_t galaxyDay;
    BOOL activeStar;
    BOOL scheduledByBuiltinPolicy;
    // Optional 0.3.0 tail. Older callbacks may ignore it using size.
    BOOL dominatorOnly;
};

struct XenoNextDayStatsV1
{
    std::uint32_t size;
    std::uint64_t turns;
    std::uint64_t totalTurnMicroseconds;
    std::uint64_t starCalls;
    std::uint64_t starMicroseconds;
    std::uint64_t coldStarCalls;
    std::uint64_t skippedStarCalls;
    // Optional 0.3.0 tail. getStats accepts the original prefix size.
    std::uint64_t dominatorEligibleCalls;
    std::uint64_t replayedStarCalls;
    std::uint64_t layoutRejectedCalls;
    std::uint64_t fastAggregatedDays;
};

static const std::uint32_t XENO_NEXTDAY_STATS_V1_BASE_SIZE =
    static_cast<std::uint32_t>(
        offsetof(XenoNextDayStatsV1, dominatorEligibleCalls));

typedef BOOL (WINAPI* XenoNextDayFullReplacementV1)(void* galaxy, void* user);
typedef int (WINAPI* XenoNextDayStarPolicyV1)(
    const XenoNextDayStarContextV1* context,
    void* user);

typedef BOOL (WINAPI* XenoNextDayRegisterFullFn)(
    XenoNextDayFullReplacementV1 replacement,
    void* user);
typedef BOOL (WINAPI* XenoNextDayRegisterStarPolicyFn)(
    XenoNextDayStarPolicyV1 policy,
    void* user);
typedef BOOL (WINAPI* XenoNextDayGetStatsFn)(XenoNextDayStatsV1* stats);
typedef BOOL (WINAPI* XenoNextDayCallOriginalFn)(void* galaxy);
typedef BOOL (WINAPI* XenoNextDayCallOriginalStarFn)(void* star, BOOL activeStar);

struct XenoNextDayApiV1
{
    std::uint32_t size;
    std::uint32_t apiVersion;
    XenoNextDayRegisterFullFn registerFullReplacement;
    XenoNextDayRegisterStarPolicyFn registerStarPolicy;
    XenoNextDayGetStatsFn getStats;
    XenoNextDayCallOriginalFn callOriginalNextDay;
    XenoNextDayCallOriginalStarFn callOriginalStarNextDay;
};

extern "C" __declspec(dllexport) BOOL WINAPI XenoNextDay_GetApi(
    XenoNextDayApiV1* api);
