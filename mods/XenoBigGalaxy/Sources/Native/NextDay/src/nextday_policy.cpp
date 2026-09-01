#include "nextday_policy.h"

#include <algorithm>
#include <cstdint>

namespace xnd
{
    bool IsColdStar(const StarSnapshot& star, const PolicySettings& settings)
    {
        return settings.farCadenceDays > 1 &&
            star.inactiveDays >= settings.farAfterInactiveDays;
    }

    bool ShouldRunColdStar(
        const StarSnapshot& star,
        int galaxyDay,
        const PolicySettings& settings)
    {
        if (!IsColdStar(star, settings))
        {
            return true;
        }
        const std::uint32_t cadence = static_cast<std::uint32_t>(
            std::max(settings.farCadenceDays, 1));
        const std::uint32_t phase =
            static_cast<std::uint32_t>(star.stableId) +
            static_cast<std::uint32_t>(galaxyDay);
        return phase % cadence == 0;
    }

    CatchUpPlan AdvanceCatchUp(
        CatchUpState& state,
        std::int32_t stableId,
        int galaxyDay,
        bool scheduled)
    {
        if (state.pendingSteps > 0 && state.stableId != stableId)
        {
            state = {};
        }
        if (!scheduled)
        {
            if (state.pendingSteps == 0)
            {
                state.stableId = stableId;
                state.firstDeferredDay = galaxyDay;
            }
            ++state.pendingSteps;
            state.lastSeenDay = galaxyDay;
            CatchUpPlan plan;
            plan.runCurrent = false;
            return plan;
        }

        CatchUpPlan plan;
        plan.historicalSteps = state.pendingSteps;
        plan.firstHistoricalDay = state.firstDeferredDay;
        state = {};
        return plan;
    }
}
