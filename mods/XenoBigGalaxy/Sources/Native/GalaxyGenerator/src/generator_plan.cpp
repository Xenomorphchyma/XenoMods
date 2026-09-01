#include "generator_plan.h"

#include <algorithm>
#include <limits>

namespace xgg
{
    namespace
    {
        bool HasCapacity(const SectorRule& rule, int load)
        {
            return rule.maximumStars <= 0 || load < rule.maximumStars;
        }

        int ClampSector(int value, int sectorCount)
        {
            return value >= 0 && value < sectorCount ? value : -1;
        }
    }

    int VanillaSectorForStar(int starIndex, int sectorCount)
    {
        if (sectorCount <= 0)
        {
            return 0;
        }
        if (starIndex < 65)
        {
            return starIndex % std::min(18, sectorCount);
        }
        if (starIndex <= 68)
        {
            return std::min(18, sectorCount - 1);
        }
        if (starIndex == 69)
        {
            return std::min(12, sectorCount - 1);
        }
        if (starIndex == 70 || starIndex == 71)
        {
            return std::min(19, sectorCount - 1);
        }
        return starIndex % sectorCount;
    }

    PlanResult BuildPlan(const PlanSettings& input)
    {
        PlanSettings settings = input;
        settings.starCount = std::max(1, settings.starCount);
        settings.sectorCount = std::max(1, settings.sectorCount);
        settings.firstCustomSector = std::clamp(
            settings.firstCustomSector, 0, settings.sectorCount);
        settings.sectors.resize(static_cast<size_t>(settings.sectorCount));

        PlanResult result;
        result.starToSector.assign(static_cast<size_t>(settings.starCount), -1);
        result.sectorLoads.assign(static_cast<size_t>(settings.sectorCount), 0);

        const int preservedCount = settings.preserveVanillaStars
            ? std::min(72, settings.starCount)
            : 0;
        for (int star = 0; star < preservedCount; ++star)
        {
            const int sector = VanillaSectorForStar(star, settings.sectorCount);
            result.starToSector[static_cast<size_t>(star)] = sector;
            ++result.sectorLoads[static_cast<size_t>(sector)];
        }

        // Explicit entries intentionally run after the compatibility mapping,
        // so a DAT author can override even one of the original 72 systems.
        for (const auto& assignment : settings.explicitAssignments)
        {
            const int star = assignment.first;
            const int sector = ClampSector(assignment.second, settings.sectorCount);
            if (star < 0 || star >= settings.starCount || sector < 0)
            {
                continue;
            }
            int& previous = result.starToSector[static_cast<size_t>(star)];
            if (previous >= 0)
            {
                --result.sectorLoads[static_cast<size_t>(previous)];
            }
            previous = sector;
            ++result.sectorLoads[static_cast<size_t>(sector)];
        }

        std::vector<int> eligible;
        const int firstSector = settings.preserveVanillaStars
            ? settings.firstCustomSector
            : 0;
        for (int sector = firstSector; sector < settings.sectorCount; ++sector)
        {
            const SectorRule& rule = settings.sectors[static_cast<size_t>(sector)];
            if (rule.enabled && rule.weight > 0)
            {
                eligible.push_back(sector);
            }
        }
        if (eligible.empty())
        {
            for (int sector = 0; sector < settings.sectorCount; ++sector)
            {
                eligible.push_back(sector);
            }
            result.warning = L"no enabled sector remained; all sectors were used as fallback";
        }

        size_t nextUnassigned = 0;
        auto takeNextUnassigned = [&]() -> int
        {
            while (nextUnassigned < result.starToSector.size() &&
                result.starToSector[nextUnassigned] >= 0)
            {
                ++nextUnassigned;
            }
            return nextUnassigned < result.starToSector.size()
                ? static_cast<int>(nextUnassigned++)
                : -1;
        };

        for (const int sector : eligible)
        {
            const SectorRule& rule = settings.sectors[static_cast<size_t>(sector)];
            while (result.sectorLoads[static_cast<size_t>(sector)] < rule.minimumStars &&
                HasCapacity(rule, result.sectorLoads[static_cast<size_t>(sector)]))
            {
                const int star = takeNextUnassigned();
                if (star < 0)
                {
                    break;
                }
                result.starToSector[static_cast<size_t>(star)] = sector;
                ++result.sectorLoads[static_cast<size_t>(sector)];
            }
        }

        std::vector<long long> scores(static_cast<size_t>(settings.sectorCount), 0);
        for (;;)
        {
            const int star = takeNextUnassigned();
            if (star < 0)
            {
                break;
            }

            long long totalWeight = 0;
            int selected = -1;
            long long selectedScore = std::numeric_limits<long long>::min();
            for (const int sector : eligible)
            {
                const SectorRule& rule = settings.sectors[static_cast<size_t>(sector)];
                if (!HasCapacity(rule, result.sectorLoads[static_cast<size_t>(sector)]))
                {
                    continue;
                }
                const int weight = settings.mode == DistributionMode::RoundRobin
                    ? 1
                    : std::clamp(rule.weight, 1, 100000);
                totalWeight += weight;
                scores[static_cast<size_t>(sector)] += weight;
                if (scores[static_cast<size_t>(sector)] > selectedScore)
                {
                    selected = sector;
                    selectedScore = scores[static_cast<size_t>(sector)];
                }
            }

            if (selected < 0)
            {
                selected = eligible[static_cast<size_t>(star) % eligible.size()];
                result.warning = L"sector maximums were exhausted; remaining stars ignored maximums";
            }
            else
            {
                scores[static_cast<size_t>(selected)] -= totalWeight;
            }
            result.starToSector[static_cast<size_t>(star)] = selected;
            ++result.sectorLoads[static_cast<size_t>(selected)];
        }
        return result;
    }
}
