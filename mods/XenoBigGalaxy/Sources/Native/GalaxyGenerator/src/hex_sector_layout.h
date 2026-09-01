#pragma once

#include <string>
#include <vector>

namespace xgg
{
    struct HexPoint
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct HexLayoutSettings
    {
        int sectorCount = 20;
        int mapWidth = 0;
        int mapHeight = 0;
        int columns = 0;
        float edgeInset = 8.0f;
    };

    struct HexLayoutResult
    {
        std::vector<HexPoint> centers;
        int columns = 0;
        int rows = 0;
        int edgeCells = 0;
        int cornerCells = 0;
        std::wstring warning;
    };

    HexLayoutResult BuildHexSectorLayout(const HexLayoutSettings& settings);
}
