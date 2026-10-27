#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../src/generator_plan.h"
#include "../src/hex_sector_layout.h"
#include "../src/layout_geometry.h"
#include "../src/runtime_base_throttle.h"
#include "../src/runtime_scan.h"
#include "../include/xeno_plugin_api.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" BOOL WINAPI XenoPlugin_Query(XenoPluginInfoV1* info);
extern "C" int __stdcall XggDecideLayoutRetry(void* galaxy, int attempts);

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::vector<unsigned char> MapPeImage(const wchar_t* path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            throw std::runtime_error("could not open test executable");
        }
        std::vector<unsigned char> file(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        Require(file.size() >= sizeof(IMAGE_DOS_HEADER), "test PE is too small");
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
        Require(dos->e_magic == IMAGE_DOS_SIGNATURE, "test PE has no MZ header");
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            file.data() + dos->e_lfanew);
        Require(nt->Signature == IMAGE_NT_SIGNATURE, "test PE has no PE header");
        std::vector<unsigned char> image(nt->OptionalHeader.SizeOfImage, 0);
        const size_t headers = std::min<size_t>(
            nt->OptionalHeader.SizeOfHeaders, file.size());
        std::copy_n(file.data(), headers, image.data());
        const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
        for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index)
        {
            const IMAGE_SECTION_HEADER& section = sections[index];
            if (!section.SizeOfRawData)
            {
                continue;
            }
            Require(
                section.PointerToRawData + section.SizeOfRawData <= file.size(),
                "test PE section exceeds file");
            Require(
                section.VirtualAddress + section.SizeOfRawData <= image.size(),
                "test PE section exceeds mapped image");
            std::copy_n(
                file.data() + section.PointerToRawData,
                section.SizeOfRawData,
                image.data() + section.VirtualAddress);
        }
        return image;
    }

    void TestPlan()
    {
        xgg::PlanSettings settings;
        settings.starCount = 200;
        settings.sectorCount = 55;
        settings.firstCustomSector = 20;
        settings.preserveVanillaStars = true;
        settings.sectors.resize(55);
        xgg::PlanResult result = xgg::BuildPlan(settings);
        Require(result.starToSector.size() == 200, "plan star count mismatch");
        Require(result.starToSector[0] == 0, "vanilla star 0 mismatch");
        Require(result.starToSector[64] == 10, "vanilla star 64 mismatch");
        Require(result.starToSector[65] == 18, "vanilla star 65 mismatch");
        Require(result.starToSector[69] == 12, "vanilla star 69 mismatch");
        Require(result.starToSector[70] == 19, "vanilla star 70 mismatch");
        for (int star = 72; star < 200; ++star)
        {
            Require(
                result.starToSector[static_cast<size_t>(star)] >= 20 &&
                result.starToSector[static_cast<size_t>(star)] < 55,
                "custom star escaped custom sectors");
        }

        const std::vector<int> vanillaBaseline(
            result.starToSector.begin(), result.starToSector.begin() + 72);
        settings.sectorCount = 30;
        settings.sectors.clear();
        settings.sectors.resize(30);
        xgg::PlanResult thirtySectorResult = xgg::BuildPlan(settings);
        Require(thirtySectorResult.starToSector.size() == 200,
            "30-sector plan star count mismatch");
        Require(thirtySectorResult.sectorLoads.size() == 30,
            "30-sector plan sector count mismatch");
        for (int star = 0; star < 72; ++star)
        {
            Require(
                thirtySectorResult.starToSector[static_cast<size_t>(star)] ==
                    vanillaBaseline[static_cast<size_t>(star)],
                "30-sector plan changed a vanilla star assignment");
        }
        for (int star = 72; star < 200; ++star)
        {
            Require(
                thirtySectorResult.starToSector[static_cast<size_t>(star)] >= 20 &&
                thirtySectorResult.starToSector[static_cast<size_t>(star)] < 30,
                "30-sector custom star escaped sectors 20..29");
        }

        settings.sectorCount = 55;
        settings.sectors.clear();
        settings.sectors.resize(55);

        settings.explicitAssignments.emplace_back(0, 33);
        settings.sectors[20].minimumStars = 10;
        settings.sectors[21].maximumStars = 1;
        result = xgg::BuildPlan(settings);
        Require(result.starToSector[0] == 33, "explicit override was ignored");
        Require(result.sectorLoads[20] >= 10, "minimum sector load was ignored");
        Require(result.sectorLoads[21] <= 1, "maximum sector load was ignored");
    }

    void TestHexLayout()
    {
        xgg::HexLayoutSettings settings;
        settings.sectorCount = 20;
        settings.mapWidth = 1600;
        settings.mapHeight = 1000;
        settings.edgeInset = 8.0f;
        xgg::HexLayoutResult layout = xgg::BuildHexSectorLayout(settings);
        Require(layout.centers.size() == 20, "hex layout sector count mismatch");
        Require(layout.columns == 5, "hex layout auto column count mismatch");
        Require(layout.rows == 4, "hex layout row count mismatch");
        Require(layout.edgeCells > 0, "hex layout has no clipped edge cells");
        Require(layout.cornerCells > 0, "hex layout has no clipped corner cells");
        for (size_t index = 0; index < layout.centers.size(); ++index)
        {
            const xgg::HexPoint& point = layout.centers[index];
            Require(point.x >= 0.0f && point.x <= 1600.0f,
                "hex center escaped map width");
            Require(point.y >= 0.0f && point.y <= 1000.0f,
                "hex center escaped map height");
            for (size_t other = 0; other < index; ++other)
            {
                const float dx = point.x - layout.centers[other].x;
                const float dy = point.y - layout.centers[other].y;
                Require(std::fabs(dx) > 0.01f || std::fabs(dy) > 0.01f,
                    "hex layout produced duplicate centers");
            }
        }
        Require(layout.centers[5].x > layout.centers[0].x,
            "hex rows were not staggered");

        settings.sectorCount = 55;
        settings.columns = 9;
        layout = xgg::BuildHexSectorLayout(settings);
        Require(layout.centers.size() == 55, "custom hex layout count mismatch");
        Require(layout.columns == 9, "custom hex column count was ignored");
        Require(layout.rows == 7, "custom hex row count mismatch");
    }

    void AddRing(
        std::vector<xgg::OutlinePoint>& points,
        std::vector<xgg::OutlineRing>& rings,
        int sector,
        std::initializer_list<xgg::OutlinePoint> ring)
    {
        rings.push_back({ sector, points.size(), ring.size() });
        points.insert(points.end(), ring.begin(), ring.end());
    }

    void TestLayoutGeometry()
    {
        // Two squares sharing the edge x=10 plus a T-junction vertex on it.
        std::vector<xgg::OutlinePoint> points;
        std::vector<xgg::OutlineRing> rings;
        AddRing(points, rings, 0, { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } });
        AddRing(points, rings, 1, { { 10, 0 }, { 20, 0 }, { 20, 10 }, { 10, 10 }, { 10, 5 } });
        Require(xgg::FindOutlineCrossings(points, rings, 0).crossings == 0,
            "adjacent sectors with shared border must not cross");

        // A third sector poking through the shared border crosses both.
        AddRing(points, rings, 2, { { 8, 4 }, { 12, 4 }, { 12, 6 }, { 8, 6 } });
        const xgg::OutlineCrossingReport overlap =
            xgg::FindOutlineCrossings(points, rings, 0);
        Require(overlap.crossings > 0, "overlapping sector must be reported");
        Require(overlap.firstSector == 0 && overlap.secondSector == 2,
            "crossing pair must name the overlapping sector");
        Require(xgg::FindOutlineCrossings(points, rings, 1).crossings == 1,
            "stopAfter must limit the scan");

        // A bow-tie polygon crosses itself.
        points.clear();
        rings.clear();
        AddRing(points, rings, 0, { { 0, 0 }, { 10, 10 }, { 10, 0 }, { 0, 10 } });
        Require(xgg::FindOutlineCrossings(points, rings, 0).crossings == 1,
            "self-crossing outline must be reported");

        // Separate chain polygons of one sector that cross are reported too.
        points.clear();
        rings.clear();
        AddRing(points, rings, 4, { { 0, 0 }, { 6, 0 }, { 6, 6 }, { 0, 6 } });
        AddRing(points, rings, 4, { { 3, 3 }, { 9, 3 }, { 9, 9 }, { 3, 9 } });
        Require(xgg::FindOutlineCrossings(points, rings, 0).crossings == 2,
            "crossing chain polygons must be reported");
    }

    // Minimal in-memory TGalaxy/TConstellation/TPolygon2D/TList objects with
    // the field offsets the runtime reads.
    struct FakeList
    {
        void* vmt = nullptr;
        void** items = nullptr;
        int count = 0;
    };

    struct FakePolygon
    {
        void* vmt = nullptr;
        FakePolygon* next = nullptr;
        FakePolygon* previous = nullptr;
        FakeList* points = nullptr;
    };

    struct FakeGalaxy
    {
        std::vector<std::vector<float>> coordinates;
        std::vector<std::vector<void*>> pointItems;
        std::vector<FakeList> pointLists;
        std::vector<FakePolygon> polygons;
        std::vector<std::vector<unsigned char>> constellations;
        std::vector<void*> constellationItems;
        FakeList constellationList;
        std::vector<unsigned char> galaxy = std::vector<unsigned char>(0x200, 0);

        explicit FakeGalaxy(const std::vector<std::vector<float>>& squares)
        {
            const size_t count = squares.size();
            coordinates = squares;
            pointItems.resize(count);
            pointLists.resize(count);
            polygons.resize(count);
            constellations.assign(count, std::vector<unsigned char>(0x100, 0));
            for (size_t index = 0; index < count; ++index)
            {
                for (size_t point = 0; point + 1 < coordinates[index].size(); point += 2)
                {
                    pointItems[index].push_back(&coordinates[index][point]);
                }
                pointLists[index].items = pointItems[index].data();
                pointLists[index].count = static_cast<int>(pointItems[index].size());
                polygons[index].points = &pointLists[index];
                FakePolygon* polygon = &polygons[index];
                std::memcpy(constellations[index].data() + 0x80, &polygon, sizeof(polygon));
                constellationItems.push_back(constellations[index].data());
            }
            constellationList.items = constellationItems.data();
            constellationList.count = static_cast<int>(count);
            FakeList* list = &constellationList;
            std::memcpy(galaxy.data() + 0x164, &list, sizeof(list));
        }
    };

    void TestLayoutRetryDecision()
    {
        FakeGalaxy clean({ { 0, 0, 10, 0, 10, 10, 0, 10 }, { 10, 0, 20, 0, 20, 10, 10, 10 } });
        FakeGalaxy crossing({ { 0, 0, 10, 0, 10, 10, 0, 10 }, { 5, 5, 15, 5, 15, 15, 5, 15 } });
        // Without a loaded config the stock RetryLimit of 1 applies: the
        // first attempt always keeps the stock retry behaviour.
        Require(XggDecideLayoutRetry(crossing.galaxy.data(), 1) == 0,
            "first attempt must defer to the stock check");
        Require(XggDecideLayoutRetry(crossing.galaxy.data(), 2) == 0,
            "crossing borders must be regenerated");
        Require(XggDecideLayoutRetry(clean.galaxy.data(), 3) == 1,
            "clean borders must be accepted");
        Require(XggDecideLayoutRetry(crossing.galaxy.data(), 1001) == 1,
            "stock attempt cap must stop the retries");
        std::vector<unsigned char> broken(0x200, 0);
        Require(XggDecideLayoutRetry(broken.data(), 2) == 1,
            "unreadable galaxy must keep the old accept behaviour");
    }

    void TestRuntimeBaseThrottle()
    {
        xgg::RuntimeBaseThrottleSettings settings;
        Require(
            xgg::ShouldRunRuntimeBaseScheduler(300, 200, settings),
            "prehistory scheduler was throttled");
        Require(
            xgg::ShouldRunRuntimeBaseScheduler(500, 73, settings),
            "vanilla-size scheduler was throttled");

        int allowed = 0;
        bool previousAllowed = false;
        for (int day = 301; day < 501; ++day)
        {
            const bool currentAllowed =
                xgg::ShouldRunRuntimeBaseScheduler(day, 200, settings);
            if (currentAllowed)
            {
                ++allowed;
                Require(!previousAllowed,
                    "configured-star-count throttle allowed adjacent scheduler days");
            }
            previousAllowed = currentAllowed;
        }
        Require(
            allowed == 73,
            "configured-star-count throttle did not preserve the expected cadence");

        settings.enabled = false;
        Require(
            xgg::ShouldRunRuntimeBaseScheduler(301, 200, settings),
            "disabled runtime-base throttle changed the scheduler");
    }

    struct OldPluginInfoV1
    {
        std::uint32_t size;
        std::uint32_t requiredHostApi;
        wchar_t id[64];
        wchar_t version[32];
        wchar_t description[160];
    };

    void TestPluginMetadata()
    {
        static_assert(
            sizeof(OldPluginInfoV1) == XENO_PLUGIN_INFO_V1_BASE_SIZE,
            "old plugin metadata layout changed");

        XenoPluginInfoV1 info = {};
        info.size = sizeof(info);
        Require(XenoPlugin_Query(&info) != FALSE, "full plugin query failed");
        Require(
            info.exclusiveCapabilities == 0,
            "galaxy plugin must not claim an order-dependent exclusive capability");
        Require(wcscmp(info.id, L"XenoGalaxyGenerator") == 0, "plugin id mismatch");
        Require(wcscmp(info.version, L"1.6.0") == 0, "plugin version mismatch");

        OldPluginInfoV1 oldInfo = {};
        oldInfo.size = sizeof(oldInfo);
        Require(
            XenoPlugin_Query(reinterpret_cast<XenoPluginInfoV1*>(&oldInfo)) != FALSE,
            "old host metadata query failed");
        Require(
            wcscmp(oldInfo.id, L"XenoGalaxyGenerator") == 0,
            "old host plugin id mismatch");
    }

    void TestExecutable(const wchar_t* path)
    {
        std::vector<unsigned char> image = MapPeImage(path);
        xgg::RuntimePoints points;
        std::wstring error;
        if (!xgg::DiscoverRuntimePoints(image.data(), points, error))
        {
            std::wcerr << L"signature_error=" << error << L" file=" << path << L"\n";
            throw std::runtime_error("runtime signature discovery failed");
        }
        Require(points.sector != nullptr, "sector hook was not discovered");
        Require(points.distribution != nullptr, "distribution hook was not discovered");
        Require(points.fullGenerator != nullptr, "full generator was not discovered");
        Require(points.geometryCall != nullptr, "geometry call was not discovered");
        Require(points.geometryOriginalTarget != nullptr,
            "geometry function was not discovered");
        xgg::RuntimeBaseSchedulerPoint baseScheduler;
        Require(
            xgg::DiscoverRuntimeBaseSchedulerPoint(
                image.data(), baseScheduler, error),
            "runtime-base scheduler point was not discovered");
        Require(baseScheduler.call != nullptr,
            "runtime-base scheduler call was not discovered");
        Require(baseScheduler.originalTarget != nullptr,
            "runtime-base scheduler function was not discovered");

        std::vector<unsigned char> compatibilityImage = MapPeImage(path);
        xgg::RuntimeBaseSchedulerPoint compatibilityBaseScheduler;
        Require(
            xgg::DiscoverRuntimeBaseSchedulerPoint(
                compatibilityImage.data(), compatibilityBaseScheduler, error),
            "compatibility runtime-base scheduler point was not discovered");
        std::fill_n(
            compatibilityBaseScheduler.call,
            5,
            static_cast<unsigned char>(0x90));
        xgg::RuntimePoints compatibilityPoints;
        Require(
            xgg::DiscoverRuntimePoints(
                compatibilityImage.data(), compatibilityPoints, error),
            "core generator incorrectly depends on the optional base scheduler signature");
        Require(points.configRootSlot != nullptr, "BlockPar root was not discovered");
        Require(points.mapWidthSlot != nullptr, "map width slot was not discovered");
        Require(points.mapHeightSlot != nullptr, "map height slot was not discovered");
        std::wcout << L"signature_test=ok file=" << path
                   << L" full_rva=0x" << std::hex << std::uppercase
                   << (points.fullGenerator - image.data())
                   << L" geometry_call_rva=0x"
                   << (points.geometryCall - image.data())
                   << L" runtime_base_scheduler_rva=0x"
                   << (baseScheduler.originalTarget - image.data())
                   << std::dec << L"\n";
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        TestPlan();
        std::cout << "plan_tests=ok\n";
        TestHexLayout();
        std::cout << "hex_layout_tests=ok\n";
        TestLayoutGeometry();
        TestLayoutRetryDecision();
        std::cout << "layout_geometry_tests=ok\n";
        TestRuntimeBaseThrottle();
        std::cout << "runtime_base_throttle_tests=ok\n";
        TestPluginMetadata();
        std::cout << "plugin_metadata_tests=ok\n";
        for (int index = 1; index < argc; ++index)
        {
            TestExecutable(argv[index]);
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "native_tests=failed " << error.what() << "\n";
        return 1;
    }
}
