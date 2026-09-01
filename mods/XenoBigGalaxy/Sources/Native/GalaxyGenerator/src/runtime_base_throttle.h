#pragma once

namespace xgg
{
    struct RuntimeBaseThrottleSettings
    {
        bool enabled = true;
        int referenceStars = 73;
        int firstGameplayDay = 301;
    };

    bool ShouldRunRuntimeBaseScheduler(
        int day,
        int starCount,
        const RuntimeBaseThrottleSettings& settings);
}
