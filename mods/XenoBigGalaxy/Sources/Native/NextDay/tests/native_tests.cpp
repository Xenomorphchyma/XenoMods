#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../include/xeno_nextday_api.h"
#include "../include/xeno_plugin_api.h"
#include "../src/live_game_adapter.h"
#include "../src/nextday_policy.h"
#include "../src/runtime_scan.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" BOOL WINAPI XenoPlugin_Query(XenoPluginInfoV1* info);

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

    void TestPolicy()
    {
        xnd::PolicySettings settings;
        settings.farAfterInactiveDays = 30;
        settings.farCadenceDays = 4;
        xnd::StarSnapshot nearStar = {
            reinterpret_cast<void*>(0x1000), 7, 29
        };
        xnd::StarSnapshot coldStar = {
            reinterpret_cast<void*>(0x2000), 7, 30
        };
        Require(!xnd::IsColdStar(nearStar, settings), "near star became cold");
        Require(xnd::IsColdStar(coldStar, settings), "cold star was not detected");
        Require(xnd::ShouldRunColdStar(coldStar, 1, settings), "cadence run phase failed");
        Require(!xnd::ShouldRunColdStar(coldStar, 2, settings), "cadence skip phase failed");

        xnd::CatchUpState catchUp;
        xnd::CatchUpPlan plan = xnd::AdvanceCatchUp(
            catchUp, 77, 100, false);
        Require(!plan.runCurrent && catchUp.pendingSteps == 1,
            "first deferred day was not recorded");
        plan = xnd::AdvanceCatchUp(catchUp, 77, 101, false);
        Require(!plan.runCurrent && catchUp.pendingSteps == 2,
            "second deferred day was not recorded");
        plan = xnd::AdvanceCatchUp(catchUp, 77, 102, true);
        Require(plan.runCurrent && plan.historicalSteps == 2 &&
                plan.firstHistoricalDay == 100,
            "catch-up replay range mismatch");
        Require(catchUp.pendingSteps == 0,
            "catch-up state was not cleared after replay");
        xnd::AdvanceCatchUp(catchUp, 77, 200, false);
        xnd::AdvanceCatchUp(catchUp, 88, 201, false);
        Require(catchUp.stableId == 88 && catchUp.pendingSteps == 1 &&
                catchUp.firstDeferredDay == 201,
            "catch-up state survived star identity change");
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
        Require(info.exclusiveCapabilities == XENO_NEXTDAY_EXCLUSIVE_CAPABILITY,
            "NextDay capability was not declared");
        Require(wcscmp(info.id, L"XenoNextDay") == 0, "plugin id mismatch");
        Require(wcscmp(info.version, L"0.4.1") == 0, "plugin version mismatch");

        OldPluginInfoV1 oldInfo = {};
        oldInfo.size = sizeof(oldInfo);
        Require(
            XenoPlugin_Query(reinterpret_cast<XenoPluginInfoV1*>(&oldInfo)) != FALSE,
            "old host metadata query failed");
        Require(wcscmp(oldInfo.id, L"XenoNextDay") == 0,
            "old host plugin id mismatch");

        XenoNextDayApiV1 api = {};
        api.size = sizeof(api);
        Require(XenoNextDay_GetApi(&api) != FALSE, "NextDay API query failed");
        Require(api.apiVersion == XENO_NEXTDAY_API_V1, "NextDay API version mismatch");
        Require(api.registerFullReplacement != nullptr, "full replacement API is missing");
        Require(api.registerStarPolicy != nullptr, "star policy API is missing");
        Require(api.callOriginalNextDay(nullptr) == FALSE,
            "uninitialized original NextDay call was accepted");

        struct OldStatsV1
        {
            std::uint32_t size;
            std::uint64_t turns;
            std::uint64_t totalTurnMicroseconds;
            std::uint64_t starCalls;
            std::uint64_t starMicroseconds;
            std::uint64_t coldStarCalls;
            std::uint64_t skippedStarCalls;
        };
        static_assert(sizeof(OldStatsV1) == XENO_NEXTDAY_STATS_V1_BASE_SIZE,
            "old stats prefix changed");
        OldStatsV1 oldStats = {};
        oldStats.size = sizeof(oldStats);
        Require(api.getStats(reinterpret_cast<XenoNextDayStatsV1*>(&oldStats)) != FALSE,
            "old stats prefix was rejected");
    }

    template <typename T>
    void WriteValue(std::vector<unsigned char>& bytes, size_t offset, T value)
    {
        Require(offset + sizeof(value) <= bytes.size(), "synthetic object overflow");
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    void TestLiveAdapter(std::vector<unsigned char>& image)
    {
        xnd::LiveGameLayout layout;
        std::wstring error;
        Require(xnd::DiscoverLiveGameLayout(image.data(), layout, error),
            "live game layout was not discovered");
        Require(layout.starClass.instanceSize == 0x114, "TStar size mismatch");
        Require(layout.dominatorClass.instanceSize == 0x4DC, "TKling size mismatch");
        Require(layout.dominatorClass.parentSelfSlotRva == layout.shipClass.selfSlotRva,
            "TKling parent mismatch");

        std::vector<unsigned char> galaxy(layout.galaxyClass.instanceSize, 0);
        WriteValue(galaxy, 0, layout.galaxyClass.vmt);
        WriteValue<std::int32_t>(galaxy, 0x4C, 1234);
        Require(xnd::IsExpectedGalaxy(layout, galaxy.data()),
            "synthetic TGalaxy VMT was rejected");

        std::vector<unsigned char> star(layout.starClass.instanceSize, 0);
        std::vector<unsigned char> dominatorA(layout.dominatorClass.instanceSize, 0);
        std::vector<unsigned char> dominatorB(layout.dominatorClass.instanceSize, 0);
        std::vector<unsigned char> nonDominator(layout.shipClass.instanceSize, 0);
        std::vector<unsigned char> list(16, 0);
        void* items[2] = {dominatorA.data(), dominatorB.data()};

        WriteValue(star, 0, layout.starClass.vmt);
        WriteValue<std::int32_t>(star, 0x08, 77);
        WriteValue<std::int32_t>(star, 0x70, 100);
        WriteValue(dominatorA, 0, layout.dominatorClass.vmt);
        WriteValue(dominatorB, 0, layout.dominatorClass.vmt);
        WriteValue(nonDominator, 0, layout.shipClass.vmt);
        WriteValue(list, 0x04, items);
        WriteValue<std::int32_t>(list, 0x08, 2);
        WriteValue(star, 0x2C, list.data());

        xnd::LiveStarInspection inspection;
        Require(xnd::InspectDominatorOnlyStar(
                layout, star.data(), 30, inspection),
            "dominator-only star was rejected");
        Require(inspection.stableId == 77 && inspection.shipCount == 2,
            "dominator-only snapshot mismatch");

        items[1] = nonDominator.data();
        Require(!xnd::InspectDominatorOnlyStar(
                layout, star.data(), 30, inspection),
            "mixed star was accepted");
        Require(inspection.reason == xnd::LiveStarReason::NonDominatorShip,
            "mixed star rejection reason mismatch");
        items[1] = dominatorB.data();

        WriteValue<std::int32_t>(star, 0x70, 29);
        Require(!xnd::InspectDominatorOnlyStar(
                layout, star.data(), 30, inspection),
            "near star was accepted");
        Require(inspection.reason == xnd::LiveStarReason::TooNear,
            "near star rejection reason mismatch");
        Require(xnd::IncrementInactiveDays(star.data()),
            "inactive day aggregation failed");
        Require(!xnd::InspectDominatorOnlyStar(
                layout, star.data(), 31, inspection),
            "inactive day aggregation advanced too far");

        int day = 0;
        Require(xnd::ReadGalaxyDay(galaxy.data(), day) && day == 1234,
            "galaxy day read failed");
        Require(xnd::WriteGalaxyDay(galaxy.data(), 1200),
            "galaxy day virtualization failed");
        Require(xnd::ReadGalaxyDay(galaxy.data(), day) && day == 1200,
            "galaxy day restore test failed");
    }

    void TestExecutable(const wchar_t* path)
    {
        std::vector<unsigned char> image = MapPeImage(path);
        TestLiveAdapter(image);
        xnd::RuntimePoints points;
        std::wstring error;
        if (!xnd::DiscoverRuntimePoints(image.data(), points, error))
        {
            std::wcerr << L"signature_error=" << error << L" file=" << path << L"\n";
            throw std::runtime_error("runtime signature discovery failed");
        }
        Require(points.nextDay != nullptr, "TGalaxy.NextDay was not discovered");
        Require(points.starNextDay != nullptr, "TStar.NextDay was not discovered");
        std::wcout << L"signature_test=ok file=" << path
                   << L" next_day_rva=0x" << std::hex << std::uppercase
                   << (points.nextDay - image.data())
                   << L" star_next_day_rva=0x"
                   << (points.starNextDay - image.data()) << std::dec << L"\n";
        std::wcout << L"live_adapter_test=ok file=" << path << L"\n";
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        TestPolicy();
        std::cout << "policy_tests=ok\n";
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
