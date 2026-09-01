#include "mask_policy.h"

namespace xdr
{
    bool IsConfusedByChameleon(
        int attackerSeries,
        bool camouflageActive,
        int camouflageSeries,
        bool detected,
        int logicMode)
    {
        if (attackerSeries < 0 || attackerSeries > 2)
        {
            return false;
        }
        if (detected && logicMode < 2)
        {
            return false;
        }
        if ((!camouflageActive || camouflageSeries != attackerSeries) &&
            logicMode == 0)
        {
            return false;
        }
        return true;
    }

    bool IsPersistedManagedMask(
        bool resumeExisting,
        int requestedSeries,
        bool camouflageActive,
        int camouflageSeries,
        bool detected,
        int logicMode)
    {
        return resumeExisting && requestedSeries >= 0 && requestedSeries <= 2 &&
            camouflageActive && camouflageSeries == requestedSeries &&
            !detected && logicMode == 2;
    }
}
