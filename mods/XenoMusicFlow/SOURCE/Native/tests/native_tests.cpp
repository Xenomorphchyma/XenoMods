// Offline checks: decision logic, and the signatures against Rangers.exe
// copies passed on the command line (the game is never started).
#include "../XenoMusicFlow/music_logic.h"
#include "../XenoMusicFlow/music_scan.h"

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* what)
    {
        if (!condition)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL %s\n", what);
        }
    }

    void TestCategories()
    {
        const auto patterns = xmf::SplitPatterns(L" StarMap ; Nation.*,Quest;; ");
        Check(patterns.size() == 3, "split count");
        Check(xmf::CategoryMatches(L"starmap", patterns), "exact, any case");
        Check(xmf::CategoryMatches(L"Nation.MalocPirate", patterns), "prefix");
        Check(xmf::CategoryMatches(L"Nation.", patterns), "empty rest");
        Check(!xmf::CategoryMatches(L"Nation", patterns), "prefix needs the dot");
        Check(!xmf::CategoryMatches(L"StarMap2", patterns), "exact is exact");
        Check(!xmf::CategoryMatches(L"Robot", patterns), "battle passes");
        Check(!xmf::CategoryMatches(L"", patterns), "empty passes");

        xmf::Settings settings;
        settings.immediatePatterns = xmf::SplitPatterns(L"Battle;Robot;Base;QuestSoundtrack*");
        Check(xmf::IsPlaceCategory(L"StarMap", settings), "space is a place");
        Check(xmf::IsPlaceCategory(L"Nation.PelengPirate", settings), "planet is a place");
        Check(xmf::IsPlaceCategory(L"RC", settings), "game station is a place");
        Check(xmf::IsPlaceCategory(L"SSB_licensed", settings), "mod station is a place");
        Check(!xmf::IsPlaceCategory(L"battle", settings), "battle switches at once");
        Check(!xmf::IsPlaceCategory(L"QuestSoundtrack.ForQuest_Gaal", settings), "quest soundtrack");
        Check(!xmf::IsPlaceCategory(L"", settings), "playlist reset");
    }

    void TestDecisions()
    {
        xmf::Settings settings;
        settings.holdMs = 45000;
        settings.crossfadeMs = 8000;
        settings.endGraceMs = 40000;
        Check(xmf::DecidePending(44999, 200000, settings) == xmf::PendingAction::Wait, "hold");
        Check(xmf::DecidePending(45000, 200000, settings) == xmf::PendingAction::Crossfade, "long rest");
        Check(xmf::DecidePending(45000, 48000, settings) == xmf::PendingAction::LetEnd, "ends soon");
        Check(xmf::DecidePending(45000, 48001, settings) == xmf::PendingAction::Crossfade, "just enough");
        Check(xmf::DecidePending(45000, -1, settings) == xmf::PendingAction::Crossfade, "unknown rest");
        settings.holdMs = 0;
        Check(xmf::DecidePending(0, 200000, settings) == xmf::PendingAction::Crossfade, "no hold");
    }

    void TestRamp()
    {
        Check(xmf::CrossfadeStep(100, -0.1f, 8000) == -0.0125f, "slow fade out");
        Check(xmf::CrossfadeStep(100, 0.1f, 8000) == 0.0125f, "slow fade in");
        Check(xmf::CrossfadeStep(100, 0.1f, 0) == 0.1f, "no crossfade length");
    }

    void TestProgress()
    {
        // A 3 MB track at 20000 bytes/s of audio: 150 s long.
        const unsigned size = 3000000;
        const unsigned rate = 20000;
        const unsigned buffer = 266240;
        xmf::TrackProgress progress;
        int a = 0;
        int b = 0;
        const void* stream = &size;
        progress.Observe(stream, &a, 0, 0, 0, size, false, 1000);
        Check(progress.RemainingMs() == -1, "too early");

        // 60 s audible plus the decode lead -> 65 s decoded.
        const unsigned consumed = rate * 65;
        const unsigned swapped = (consumed / buffer) * buffer;
        // Walk the buffers so every swap is seen.
        unsigned done = 0;
        bool useA = true;
        unsigned now = 1000;
        while (done + buffer <= swapped)
        {
            progress.Observe(stream, useA ? static_cast<void*>(&a) : &b, buffer - 1, 1, buffer, size, false, now);
            done += buffer;
            useA = !useA;
            now += 1000;
        }
        progress.Observe(stream, useA ? static_cast<void*>(&a) : &b, consumed - swapped,
            buffer - (consumed - swapped), buffer, size, false, 61000);
        const long long remaining = progress.RemainingMs();
        Check(remaining > 88000 && remaining < 92000, "estimate 90 s left");

        // At the end of file the buffered rest gives the exact remainder:
        // 10 s of bytes still unread plus the decode lead.
        xmf::TrackProgress tail;
        tail.Observe(stream, &a, 0, 0, 0, size, false, 0);
        tail.Observe(stream, &a, rate * 135, rate * 10, 0, size, true, 130000);
        Check(tail.RemainingMs() == 15000, "tail from buffered bytes");

        // A new stream starts over.
        int other = 0;
        progress.Observe(&other, &a, 100, 0, 0, size, false, 131000);
        Check(progress.RemainingMs() == -1, "new track resets");
    }

    struct Known
    {
        const char* profile;
        std::uint32_t play, stop, ramp, fileName, choose, fade;
        // Return addresses: TfLoad.OnOpen, TfHangar take-off, TfPanelRuins take-off.
        std::uint32_t loading, hangar, station;
    };

    bool ProbeFile(const wchar_t* path)
    {
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE, 0, 0, nullptr);
        CloseHandle(file);
        if (!mapping) return false;
        auto* view = static_cast<unsigned char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        CloseHandle(mapping);
        if (!view) return false;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            view + reinterpret_cast<const IMAGE_DOS_HEADER*>(view)->e_lfanew);
        const std::uint32_t imageBase = nt->OptionalHeader.ImageBase;
        auto va = [&](const void* p) {
            return p ? imageBase + static_cast<std::uint32_t>(static_cast<const unsigned char*>(p) - view) : 0u;
        };
        xmf::MusicPoints points;
        std::wstring error;
        const bool found = xmf::FindMusicPoints(view, points, error);
        if (!found)
        {
            ++g_failures;
            std::fwprintf(stderr, L"FAIL probe %ls: %ls\n", path, error.c_str());
        }
        else
        {
            std::uint32_t loading = 0;
            std::vector<std::uint32_t> takeoffs;
            for (const auto& fade : points.placeFades)
            {
                if (fade.site == xmf::FadeSite::Loading) loading = va(fade.returnAddress);
                else takeoffs.push_back(va(fade.returnAddress));
            }
            std::wprintf(L"probe %ls profile=%ls play=%08X stop=%08X ramp=%08X file=%08X choose=%08X "
                L"fade=%08X loading=%08X takeoffs=%u\n",
                path, points.profile, va(points.playCategory), va(points.stopImmediately),
                va(points.volumeRamp), va(points.getFileName), va(points.chooseMusicFile),
                va(points.requestFadeOut), loading, static_cast<unsigned>(takeoffs.size()));
            Check(points.placeFades.size() == 3 && takeoffs.size() == 2, "one loading and two take-off fades");
            // Addresses read from the disassembly of two reference builds.
            const Known known[] = {
                { "optimized", 0x4D5434, 0x4D55B8, 0x4C9070, 0x4D4CD0, 0x4D5734, 0x4D5594,
                    0x5E9163, 0x60133F, 0x67DEB9 },
                { "legacy", 0x7FA7EC, 0x7FA97C, 0x7F808C, 0x7F9F70, 0x7FAB14, 0x7FA94C,
                    0x606506, 0x62028A, 0x6AAB3F },
            };
            for (const auto& entry : known)
            {
                if (va(points.playCategory) != entry.play) continue;
                Check(va(points.stopImmediately) == entry.stop, "known stop");
                Check(va(points.volumeRamp) == entry.ramp, "known ramp");
                Check(va(points.getFileName) == entry.fileName, "known GetFileName");
                Check(va(points.chooseMusicFile) == entry.choose, "known ChooseMusicFile");
                Check(va(points.requestFadeOut) == entry.fade, "known RequestFadeOut");
                Check(loading == entry.loading, "known loading fade");
                Check(takeoffs.size() == 2 && takeoffs[0] == entry.hangar && takeoffs[1] == entry.station,
                    "known take-off fades");
            }
        }
        UnmapViewOfFile(view);
        return found;
    }
}

int wmain(int argc, wchar_t** argv)
{
    TestCategories();
    TestDecisions();
    TestRamp();
    TestProgress();
    for (int i = 1; i < argc; ++i)
    {
        ProbeFile(argv[i]);
    }
    std::printf("native_tests=%s failures=%d probes=%d\n", g_failures ? "failed" : "passed", g_failures, argc - 1);
    return g_failures ? 1 : 0;
}
