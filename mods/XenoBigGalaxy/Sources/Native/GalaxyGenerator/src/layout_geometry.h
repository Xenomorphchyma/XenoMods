#pragma once

#include <cstddef>
#include <vector>

namespace xgg
{
    struct OutlinePoint
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    // One closed TPolygon2D of a sector outline chain; its points are
    // points[first .. first + count).
    struct OutlineRing
    {
        int sector = -1;
        size_t first = 0;
        size_t count = 0;
    };

    struct OutlineCrossingReport
    {
        int crossings = 0;
        int firstSector = -1;
        int secondSector = -1;
    };

    // Counts proper crossings between outline edges: edges of different
    // sectors, of different polygons in one chain, and of one polygon with
    // itself. Shared vertices, T-junctions and collinear shared borders are
    // normal for adjacent sectors and are not crossings. Stops counting once
    // stopAfter crossings are found (0 counts all).
    OutlineCrossingReport FindOutlineCrossings(
        const std::vector<OutlinePoint>& points,
        const std::vector<OutlineRing>& rings,
        int stopAfter);
}
