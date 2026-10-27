#include "layout_geometry.h"

#include <algorithm>
#include <cmath>

namespace xgg
{
    namespace
    {
        // Same tolerance as the game's PointsNearlyEqualF.
        constexpr double kPointTolerance = 0.1;
        constexpr double kParallelEpsilon = 1e-9;

        struct Bounds
        {
            double minX;
            double minY;
            double maxX;
            double maxY;
        };

        bool Near(double ax, double ay, const OutlinePoint& point)
        {
            return std::fabs(ax - point.x) < kPointTolerance &&
                std::fabs(ay - point.y) < kPointTolerance;
        }

        bool SegmentsCross(
            const OutlinePoint& a1,
            const OutlinePoint& a2,
            const OutlinePoint& b1,
            const OutlinePoint& b2)
        {
            const double rx = static_cast<double>(a2.x) - a1.x;
            const double ry = static_cast<double>(a2.y) - a1.y;
            const double sx = static_cast<double>(b2.x) - b1.x;
            const double sy = static_cast<double>(b2.y) - b1.y;
            const double denominator = rx * sy - ry * sx;
            if (std::fabs(denominator) < kParallelEpsilon)
            {
                return false;
            }
            const double qx = static_cast<double>(b1.x) - a1.x;
            const double qy = static_cast<double>(b1.y) - a1.y;
            const double t = (qx * sy - qy * sx) / denominator;
            const double u = (qx * ry - qy * rx) / denominator;
            if (t < 0.0 || t > 1.0 || u < 0.0 || u > 1.0)
            {
                return false;
            }
            const double x = a1.x + t * rx;
            const double y = a1.y + t * ry;
            return !Near(x, y, a1) && !Near(x, y, a2) &&
                !Near(x, y, b1) && !Near(x, y, b2);
        }

        Bounds RingBounds(const std::vector<OutlinePoint>& points, const OutlineRing& ring)
        {
            Bounds bounds = { points[ring.first].x, points[ring.first].y,
                points[ring.first].x, points[ring.first].y };
            for (size_t index = 1; index < ring.count; ++index)
            {
                const OutlinePoint& point = points[ring.first + index];
                bounds.minX = std::min<double>(bounds.minX, point.x);
                bounds.minY = std::min<double>(bounds.minY, point.y);
                bounds.maxX = std::max<double>(bounds.maxX, point.x);
                bounds.maxY = std::max<double>(bounds.maxY, point.y);
            }
            return bounds;
        }

        bool BoundsOverlap(const Bounds& first, const Bounds& second)
        {
            return first.minX <= second.maxX + kPointTolerance &&
                second.minX <= first.maxX + kPointTolerance &&
                first.minY <= second.maxY + kPointTolerance &&
                second.minY <= first.maxY + kPointTolerance;
        }
    }

    OutlineCrossingReport FindOutlineCrossings(
        const std::vector<OutlinePoint>& points,
        const std::vector<OutlineRing>& rings,
        int stopAfter)
    {
        OutlineCrossingReport report;
        std::vector<Bounds> bounds;
        bounds.reserve(rings.size());
        for (const OutlineRing& ring : rings)
        {
            bounds.push_back(ring.count >= 2 && ring.first + ring.count <= points.size()
                ? RingBounds(points, ring)
                : Bounds{ 1.0, 1.0, -1.0, -1.0 });
        }
        for (size_t first = 0; first < rings.size(); ++first)
        {
            const OutlineRing& a = rings[first];
            if (a.count < 2 || a.first + a.count > points.size())
            {
                continue;
            }
            for (size_t second = first; second < rings.size(); ++second)
            {
                const OutlineRing& b = rings[second];
                if (b.count < 2 || b.first + b.count > points.size() ||
                    !BoundsOverlap(bounds[first], bounds[second]))
                {
                    continue;
                }
                for (size_t edgeA = 0; edgeA < a.count; ++edgeA)
                {
                    const OutlinePoint& a1 = points[a.first + edgeA];
                    const OutlinePoint& a2 = points[a.first + (edgeA + 1) % a.count];
                    // A ring is compared with itself once per edge pair.
                    const size_t startB = first == second ? edgeA + 1 : 0;
                    for (size_t edgeB = startB; edgeB < b.count; ++edgeB)
                    {
                        const OutlinePoint& b1 = points[b.first + edgeB];
                        const OutlinePoint& b2 = points[b.first + (edgeB + 1) % b.count];
                        if (!SegmentsCross(a1, a2, b1, b2))
                        {
                            continue;
                        }
                        if (report.crossings == 0)
                        {
                            report.firstSector = a.sector;
                            report.secondSector = b.sector;
                        }
                        ++report.crossings;
                        if (stopAfter > 0 && report.crossings >= stopAfter)
                        {
                            return report;
                        }
                    }
                }
            }
        }
        return report;
    }
}
