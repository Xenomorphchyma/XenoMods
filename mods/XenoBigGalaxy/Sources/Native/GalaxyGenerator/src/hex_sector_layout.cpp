#define NOMINMAX
#include "hex_sector_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace xgg
{
    namespace
    {
        int RowCount(int count, int columns)
        {
            return (count + columns - 1) / columns;
        }

        int ChooseColumns(const HexLayoutSettings& settings, float inset)
        {
            if (settings.columns > 0)
            {
                return std::clamp(settings.columns, 1, settings.sectorCount);
            }

            const float width = static_cast<float>(settings.mapWidth) - 2.0f * inset;
            const float height = static_cast<float>(settings.mapHeight) - 2.0f * inset;
            const float wantedRatio = std::sqrt(3.0f) * 0.5f;
            float bestScore = std::numeric_limits<float>::max();
            int best = 1;
            for (int columns = 1; columns <= settings.sectorCount; ++columns)
            {
                const int rows = RowCount(settings.sectorCount, columns);
                if (columns < 2 || rows < 2)
                {
                    continue;
                }
                const float xStep = width / (static_cast<float>(columns) - 0.5f);
                const float yStep = height / static_cast<float>(rows - 1);
                const float shapeScore = std::fabs(
                    std::log(std::max(0.0001f, yStep / xStep) / wantedRatio));
                const int unused = rows * columns - settings.sectorCount;
                const float unusedScore = 0.04f * static_cast<float>(unused) /
                    static_cast<float>(settings.sectorCount);
                const float score = shapeScore + unusedScore;
                if (score < bestScore)
                {
                    bestScore = score;
                    best = columns;
                }
            }
            return best;
        }
    }

    HexLayoutResult BuildHexSectorLayout(const HexLayoutSettings& settings)
    {
        HexLayoutResult result;
        if (settings.sectorCount < 1 || settings.mapWidth < 32 || settings.mapHeight < 32)
        {
            result.warning = L"invalid sector count or map dimensions";
            return result;
        }

        const float maximumInset = 0.2f * static_cast<float>(
            std::min(settings.mapWidth, settings.mapHeight));
        const float inset = std::clamp(settings.edgeInset, 0.0f, maximumInset);
        result.columns = ChooseColumns(settings, inset);
        result.rows = RowCount(settings.sectorCount, result.columns);

        const float usableWidth = static_cast<float>(settings.mapWidth) - 2.0f * inset;
        const float usableHeight = static_cast<float>(settings.mapHeight) - 2.0f * inset;
        const float xStep = result.columns > 1
            ? usableWidth / (static_cast<float>(result.columns) - 0.5f)
            : 0.0f;
        const float yStep = result.rows > 1
            ? usableHeight / static_cast<float>(result.rows - 1)
            : 0.0f;

        result.centers.reserve(static_cast<size_t>(settings.sectorCount));
        int remaining = settings.sectorCount;
        for (int row = 0; row < result.rows && remaining > 0; ++row)
        {
            const int inRow = std::min(result.columns, remaining);
            const float stagger = (row & 1) != 0 ? xStep * 0.5f : 0.0f;
            const float occupiedWidth = inRow > 1
                ? static_cast<float>(inRow - 1) * xStep
                : 0.0f;
            const float fullRowWidth = result.columns > 1
                ? static_cast<float>(result.columns - 1) * xStep
                : 0.0f;
            const float centerIncompleteRow = 0.5f * (fullRowWidth - occupiedWidth);
            const float startX = inset + stagger + centerIncompleteRow;
            const float y = result.rows > 1
                ? inset + static_cast<float>(row) * yStep
                : static_cast<float>(settings.mapHeight) * 0.5f;

            for (int column = 0; column < inRow; ++column)
            {
                float x = result.columns > 1
                    ? startX + static_cast<float>(column) * xStep
                    : static_cast<float>(settings.mapWidth) * 0.5f;
                x = std::clamp(x, inset, static_cast<float>(settings.mapWidth) - inset);
                result.centers.push_back({ x, y });

                const bool horizontalEdge = row == 0 || row == result.rows - 1;
                const bool verticalEdge = x <= inset + 0.25f * xStep ||
                    x >= static_cast<float>(settings.mapWidth) - inset - 0.25f * xStep;
                if (horizontalEdge || verticalEdge)
                {
                    ++result.edgeCells;
                }
                if (horizontalEdge && verticalEdge)
                {
                    ++result.cornerCells;
                }
            }
            remaining -= inRow;
        }

        if (result.centers.size() != static_cast<size_t>(settings.sectorCount))
        {
            result.centers.clear();
            result.warning = L"hex layout did not produce every requested center";
        }
        return result;
    }
}
