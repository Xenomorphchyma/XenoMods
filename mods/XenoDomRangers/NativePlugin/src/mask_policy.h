#pragma once

namespace xdr
{
    // Mirrors the vanilla Player chameleon decision. A result of true means
    // that a TKling treats the target as a matching Dominator contact.
    bool IsConfusedByChameleon(
        int attackerSeries,
        bool camouflageActive,
        int camouflageSeries,
        bool detected,
        int logicMode);

    // Identifies the exact byte pattern written by this plugin. It is used only
    // while rebuilding a script-confirmed active session after loading a save;
    // an ordinary first application still preserves the ship's real baseline.
    bool IsPersistedManagedMask(
        bool resumeExisting,
        int requestedSeries,
        bool camouflageActive,
        int camouflageSeries,
        bool detected,
        int logicMode);
}
