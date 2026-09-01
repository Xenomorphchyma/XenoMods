#pragma once

#include <cstdint>

namespace xnd
{
    struct PolicySettings
    {
        int farAfterInactiveDays = 30;
        int farCadenceDays = 1;
    };

    struct StarSnapshot
    {
        void* star = nullptr;
        std::int32_t stableId = 0;
        std::int32_t inactiveDays = 0;
    };

    struct CatchUpState
    {
        std::int32_t stableId = 0;
        int firstDeferredDay = 0;
        int pendingSteps = 0;
        int lastSeenDay = 0;
    };

    struct CatchUpPlan
    {
        int historicalSteps = 0;
        int firstHistoricalDay = 0;
        bool runCurrent = true;
    };

    bool IsColdStar(const StarSnapshot& star, const PolicySettings& settings);
    bool ShouldRunColdStar(
        const StarSnapshot& star,
        int galaxyDay,
        const PolicySettings& settings);
    CatchUpPlan AdvanceCatchUp(
        CatchUpState& state,
        std::int32_t stableId,
        int galaxyDay,
        bool scheduled);
}
