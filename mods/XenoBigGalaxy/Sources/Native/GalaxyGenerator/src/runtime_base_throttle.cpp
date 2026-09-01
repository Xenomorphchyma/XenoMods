#include "runtime_base_throttle.h"

#include <cstdint>

namespace xgg
{
    bool ShouldRunRuntimeBaseScheduler(
        int day,
        int starCount,
        const RuntimeBaseThrottleSettings& settings)
    {
        if (!settings.enabled || day < settings.firstGameplayDay ||
            starCount <= settings.referenceStars || starCount <= 0 ||
            settings.referenceStars <= 0)
        {
            return true;
        }

        const std::int64_t slot =
            static_cast<std::int64_t>(day) - settings.firstGameplayDay;
        const std::int64_t previous =
            (slot * settings.referenceStars) / starCount;
        const std::int64_t current =
            ((slot + 1) * settings.referenceStars) / starCount;
        return current != previous;
    }
}
