#pragma once

#include "../include/xeno_plugin_api.h"

namespace xdr
{
    struct UserConfig
    {
        int recruitmentCheckTurns = 100;
        int recruitmentChancePercent = 10;
        int maximumSpecialists = 3;
        int minimumFreeCargo = 20;
        int maximumRecruitHullDamage = 40;
        int replacementDelayTurns = 1000;

        int camouflageCostPercent = 100;
        int camouflageLifetimeTurns = 300;
        int restTurns = 300;
        int maximumSalvageTurns = 20;
        int emptyLootChecks = 3;
        int minimumLootCostPercent = 100;

        int safeLootStarRadius = 400;
        int solarCarefulRadius = 600;
        int solarApproachStep = 100;
        int solarRetreatRadius = 450;
        int solarRetreatHullDamage = 60;
        int solarStallChecks = 3;
    };

    void LoadUserConfig(const XenoPluginHostV1* host, UserConfig& config);
    int GetUserConfigValue(const UserConfig& config, int key);
}
