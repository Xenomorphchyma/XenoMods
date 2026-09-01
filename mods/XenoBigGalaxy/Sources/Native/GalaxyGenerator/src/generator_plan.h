#pragma once

#include <string>
#include <utility>
#include <vector>

namespace xgg
{
    enum class DistributionMode
    {
        Weighted,
        RoundRobin,
        Explicit
    };

    struct SectorRule
    {
        bool enabled = true;
        int weight = 1;
        int minimumStars = 0;
        int maximumStars = 0; // Zero means unlimited.
    };

    struct PlanSettings
    {
        int starCount = 73;
        int sectorCount = 20;
        int firstCustomSector = 20;
        bool preserveVanillaStars = true;
        DistributionMode mode = DistributionMode::Weighted;
        std::vector<SectorRule> sectors;
        std::vector<std::pair<int, int>> explicitAssignments;
    };

    struct PlanResult
    {
        std::vector<int> starToSector;
        std::vector<int> sectorLoads;
        std::wstring warning;
    };

    int VanillaSectorForStar(int starIndex, int sectorCount);
    PlanResult BuildPlan(const PlanSettings& settings);
}
