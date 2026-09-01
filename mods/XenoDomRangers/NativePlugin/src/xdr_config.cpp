#include "xdr_config.h"

#include <cstddef>

namespace
{
    int ReadInt(
        const XenoPluginHostV1* host,
        const wchar_t* section,
        const wchar_t* key,
        int fallback,
        int minimum,
        int maximum)
    {
        const std::size_t requiredSize =
            offsetof(XenoPluginHostV1, configGetString);
        if (!host || host->size < requiredSize || !host->configGetInt)
        {
            return fallback;
        }
        return host->configGetInt(
            host->configPath, section, key, fallback, minimum, maximum);
    }
}

namespace xdr
{
    void LoadUserConfig(const XenoPluginHostV1* host, UserConfig& config)
    {
        config = {};
        config.recruitmentCheckTurns = ReadInt(host, L"Recruitment", L"CheckIntervalTurns", 100, 10, 1000);
        config.recruitmentChancePercent = ReadInt(host, L"Recruitment", L"ChancePercent", 10, 0, 100);
        config.maximumSpecialists = ReadInt(host, L"Recruitment", L"MaximumSpecialists", 3, 0, 3);
        config.minimumFreeCargo = ReadInt(host, L"Recruitment", L"MinimumFreeCargo", 20, 0, 500);
        config.maximumRecruitHullDamage = ReadInt(host, L"Recruitment", L"MaximumHullDamage", 40, 0, 90);
        config.replacementDelayTurns = ReadInt(host, L"Recruitment", L"ReplacementDelayTurns", 1000, 0, 10000);

        config.camouflageCostPercent = ReadInt(host, L"Mission", L"CamouflageCostPercent", 100, 0, 1000);
        config.camouflageLifetimeTurns = ReadInt(host, L"Mission", L"CamouflageLifetimeTurns", 300, 10, 3000);
        config.restTurns = ReadInt(host, L"Mission", L"RestTurns", 300, 1, 3000);
        config.maximumSalvageTurns = ReadInt(host, L"Mission", L"MaximumSalvageTurns", 20, 1, 100);
        config.emptyLootChecks = ReadInt(host, L"Mission", L"EmptyLootChecks", 3, 1, 20);
        config.minimumLootCostPercent = ReadInt(host, L"Mission", L"MinimumLootCostPercent", 100, 0, 1000);

        config.safeLootStarRadius = ReadInt(host, L"Safety", L"SafeLootStarRadius", 400, 300, 2000);
        config.solarCarefulRadius = ReadInt(host, L"Safety", L"SolarCarefulRadius", 600, 350, 3000);
        config.solarApproachStep = ReadInt(host, L"Safety", L"SolarApproachStep", 100, 10, 500);
        config.solarRetreatRadius = ReadInt(host, L"Safety", L"SolarRetreatRadius", 450, 350, 2000);
        config.solarRetreatHullDamage = ReadInt(host, L"Safety", L"SolarRetreatHullDamage", 60, 10, 95);
        config.solarStallChecks = ReadInt(host, L"Safety", L"SolarStallChecks", 3, 1, 20);

        // Keep the broad normal-flight perimeter outside both the loot safety
        // radius and the retreat point. Invalid combinations are normalized
        // deterministically instead of leaking unsafe geometry into RScript.
        if (config.solarCarefulRadius < config.safeLootStarRadius + 100)
        {
            config.solarCarefulRadius = config.safeLootStarRadius + 100;
        }
        if (config.solarCarefulRadius < config.solarRetreatRadius + 100)
        {
            config.solarCarefulRadius = config.solarRetreatRadius + 100;
        }
    }

    int GetUserConfigValue(const UserConfig& config, int key)
    {
        switch (key)
        {
        case 1: return config.recruitmentCheckTurns;
        case 2: return config.recruitmentChancePercent;
        case 3: return config.maximumSpecialists;
        case 4: return config.minimumFreeCargo;
        case 5: return config.maximumRecruitHullDamage;
        case 6: return config.replacementDelayTurns;
        case 10: return config.camouflageCostPercent;
        case 11: return config.camouflageLifetimeTurns;
        case 12: return config.restTurns;
        case 13: return config.maximumSalvageTurns;
        case 14: return config.emptyLootChecks;
        case 15: return config.minimumLootCostPercent;
        case 20: return config.safeLootStarRadius;
        case 21: return config.solarCarefulRadius;
        case 22: return config.solarApproachStep;
        case 23: return config.solarRetreatRadius;
        case 24: return config.solarRetreatHullDamage;
        case 25: return config.solarStallChecks;
        default: return -1;
        }
    }
}
