#include "music_scan.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace xmf
{
    namespace
    {
        // -1 is a wildcard: absolute addresses, exception frames and call targets.
        struct Signature
        {
            const int* bytes;
            size_t length;
        };

        // $O+ prerelease builds (2026-10): Category stays in EBX.
        const int kPlayOptimized[] = {
            0x55, 0x8B, 0xEC, 0x33, 0xC9, 0x51, 0x51, 0x51, 0x51, 0x51, 0x53, 0x8B,
            0xDA, 0x89, 0x45, 0xF8, 0x33, 0xC0, 0x55, 0x68, -1, -1, -1, -1,
            0x64, 0xFF, 0x30, 0x64, 0x89, 0x20, 0xA1, -1, -1, -1, -1, 0x80,
            0x38, 0x00, 0x0F, 0x84, -1, -1, -1, -1, 0x8B, 0x45, 0xF8, 0x8B,
            0x40, 0x30, 0xE8, -1, -1, -1, -1, 0x33, 0xC0, 0x55, 0x68, -1,
            -1, -1, -1, 0x64, 0xFF, 0x30, 0x64, 0x89, 0x20, 0x8B, 0x45, 0xF8,
            0x8B, 0x40, 0x44, 0x33, 0xD2, 0xE8, -1, -1, -1, -1, 0x75, -1,
            0x8D, 0x55, 0xF4, 0x8B, 0x45, 0xF8, 0x8B, 0x40, 0x34, 0xE8, -1, -1,
            -1, -1, 0x8B, 0x55, 0xF4, 0x8D, 0x4D, 0xFC, 0x8B, 0xC3, 0xE8
        };
        // $O- release builds: Self and Category in [ebp-4] and [ebp-8].
        const int kPlayLegacy[] = {
            0x55, 0x8B, 0xEC, 0x33, 0xC9, 0x51, 0x51, 0x51, 0x51, 0x51, 0x51, 0x51,
            0x89, 0x55, 0xF8, 0x89, 0x45, 0xFC, 0x33, 0xC0, 0x55, 0x68, -1, -1,
            -1, -1, 0x64, 0xFF, 0x30, 0x64, 0x89, 0x20, 0xA1, -1, -1, -1,
            -1, 0x80, 0x38, 0x00, 0x0F, 0x84, -1, -1, -1, -1, 0x8B, 0x45,
            0xFC, 0x8B, 0x40, 0x30, 0xE8, -1, -1, -1, -1, 0x33, 0xC0, 0x55,
            0x68, -1, -1, -1, -1, 0x64, 0xFF, 0x30, 0x64, 0x89, 0x20, 0x8B,
            0x45, 0xFC, 0x8B, 0x40, 0x44, 0x33, 0xD2, 0xE8, -1, -1, -1, -1,
            0x75, -1, 0x8D, 0x55, 0xEC, 0x8B, 0x45, 0xFC, 0x8B, 0x40, 0x34, 0xE8,
            -1, -1, -1, -1, 0x8B, 0x55, 0xEC, 0x8D, 0x4D, 0xF0, 0x8B, 0x45,
            0xF8, 0xE8
        };
        const int kStopOptimized[] = {
            0x53, 0x8B, 0xD8, 0xA1, -1, -1, -1, -1, 0x80, 0x38, 0x00, 0x74,
            -1, 0x8B, 0x43, 0x34, 0xE8, -1, -1, -1, -1, 0x84, 0xC0, 0x74,
            -1, 0x8B, 0x43, 0x34, 0xE8, -1, -1, -1, -1, 0x8B, 0x43, 0x34,
            0xC6, 0x40, 0x38, 0x01, 0x5B, 0xC3
        };
        const int kStopLegacy[] = {
            0x55, 0x8B, 0xEC, 0x51, 0x89, 0x45, 0xFC, 0xA1, -1, -1, -1, -1,
            0x80, 0x38, 0x00, 0x74, -1, 0x8B, 0x45, 0xFC, 0x8B, 0x40, 0x34, 0xE8,
            -1, -1, -1, -1, 0x84, 0xC0, 0x74, -1, 0x8B, 0x45, 0xFC, 0x8B,
            0x40, 0x34, 0xE8, -1, -1, -1, -1, 0x8B, 0x45, 0xFC, 0x8B, 0x40,
            0x34, 0xC6, 0x40, 0x38, 0x01, 0x59, 0x5D, 0xC3
        };
        // RequestFadeOut is StopImmediately without the ImmediateStop store.
        const int kFadeOptimized[] = {
            0x53, 0x8B, 0xD8, 0xA1, -1, -1, -1, -1, 0x80, 0x38, 0x00, 0x74,
            -1, 0x8B, 0x43, 0x34, 0xE8, -1, -1, -1, -1, 0x84, 0xC0, 0x74,
            -1, 0x8B, 0x43, 0x34, 0xE8, -1, -1, -1, -1, 0x5B, 0xC3
        };
        const int kFadeLegacy[] = {
            0x55, 0x8B, 0xEC, 0x51, 0x89, 0x45, 0xFC, 0xA1, -1, -1, -1, -1,
            0x80, 0x38, 0x00, 0x74, -1, 0x8B, 0x45, 0xFC, 0x8B, 0x40, 0x34, 0xE8,
            -1, -1, -1, -1, 0x84, 0xC0, 0x74, -1, 0x8B, 0x45, 0xFC, 0x8B,
            0x40, 0x34, 0xE8, -1, -1, -1, -1, 0x59, 0x5D, 0xC3
        };
        const int kRampOptimized[] = {
            0x55, 0x8B, 0xEC, 0x53, 0x56, 0x8B, 0xF2, 0x8B, 0xD8, 0x8B, 0x43, 0x28,
            0x50, 0xE8, -1, -1, -1, -1, 0x8B, 0x45, 0x08, 0x89, 0x43, 0x50,
            0x8B, 0x43, 0x44, 0x85, 0xC0, 0x74, -1, 0x50, 0xE8, -1, -1, -1,
            -1, 0x33, 0xC0, 0x89, 0x43, 0x44, 0x6A, 0x11, 0x6A, 0x00
        };
        const int kRampLegacy[] = {
            0x55, 0x8B, 0xEC, 0x83, 0xC4, 0xF8, 0x89, 0x55, 0xF8, 0x89, 0x45, 0xFC,
            0x8B, 0x45, 0xFC, 0x8B, 0x40, 0x28, 0x50, 0xE8, -1, -1, -1, -1,
            0x8B, 0x45, 0x08, 0x8B, 0x55, 0xFC, 0x89, 0x42, 0x50
        };

        struct Profile
        {
            const wchar_t* name;
            Signature play;
            Signature stop;
            Signature ramp;
            Signature fade;
            // Offsets of the GetFileName and ChooseMusicFile CALL opcodes
            // inside PlayCategory.
            size_t getFileNameCall;
            size_t chooseCall;
            size_t playLength;
            size_t stopLength;
            size_t rampLength;
            size_t fadeLength;
        };

        // Entry lengths cover whole instructions: push ebp / mov ebp,esp /
        // xor ecx,ecx; push ebx / mov ebx,eax / mov eax,[abs]; and so on.
        const Profile kProfiles[] = {
            { L"optimized",
                { kPlayOptimized, _countof(kPlayOptimized) },
                { kStopOptimized, _countof(kStopOptimized) },
                { kRampOptimized, _countof(kRampOptimized) },
                { kFadeOptimized, _countof(kFadeOptimized) },
                93, 106, 5, 8, 5, 8 },
            { L"legacy",
                { kPlayLegacy, _countof(kPlayLegacy) },
                { kStopLegacy, _countof(kStopLegacy) },
                { kRampLegacy, _countof(kRampLegacy) },
                { kFadeLegacy, _countof(kFadeLegacy) },
                95, 109, 5, 7, 6, 7 },
        };

        bool Matches(const unsigned char* data, const Signature& signature)
        {
            for (size_t i = 0; i < signature.length; ++i)
            {
                if (signature.bytes[i] >= 0 && data[i] != static_cast<unsigned char>(signature.bytes[i]))
                {
                    return false;
                }
            }
            return true;
        }

        struct CodeRange
        {
            unsigned char* begin;
            size_t size;
        };

        std::vector<CodeRange> ExecutableRanges(unsigned char* base, std::wstring& error)
        {
            std::vector<CodeRange> ranges;
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x10000)
            {
                error = L"invalid DOS header";
                return ranges;
            }
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386)
            {
                error = L"not an x86 PE image";
                return ranges;
            }
            const auto* sections = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            {
                const auto& section = sections[i];
                if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
                if (section.VirtualAddress > nt->OptionalHeader.SizeOfImage) continue;
                const size_t size = (std::min)(static_cast<size_t>(section.Misc.VirtualSize),
                    static_cast<size_t>(nt->OptionalHeader.SizeOfImage - section.VirtualAddress));
                ranges.push_back({ base + section.VirtualAddress, size });
            }
            return ranges;
        }

        std::vector<unsigned char*> FindAll(const std::vector<CodeRange>& ranges, const Signature& signature)
        {
            std::vector<unsigned char*> found;
            for (const auto& range : ranges)
            {
                if (range.size < signature.length) continue;
                for (size_t offset = 0; offset + signature.length <= range.size; ++offset)
                {
                    if (range.begin[offset] == signature.bytes[0] && Matches(range.begin + offset, signature))
                    {
                        found.push_back(range.begin + offset);
                    }
                }
            }
            return found;
        }

        void* CallTarget(unsigned char* call)
        {
            std::int32_t relative = 0;
            std::memcpy(&relative, call + 1, sizeof(relative));
            return call + 5 + relative;
        }

        struct Image
        {
            unsigned char* base;
            std::uintptr_t preferredBase;
            size_t size;
        };

        Image DescribeImage(unsigned char* base)
        {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
                base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
            return { base, nt->OptionalHeader.ImageBase, nt->OptionalHeader.SizeOfImage };
        }

        // An absolute operand as an address in this mapping (the probe maps
        // the file away from its preferred base).
        const unsigned char* Absolute(const Image& image, std::uint32_t value, size_t length)
        {
            if (value < image.preferredBase || value - image.preferredBase + length > image.size)
            {
                return nullptr;
            }
            return image.base + (value - image.preferredBase);
        }

        bool Contains(const unsigned char* begin, const unsigned char* end, const unsigned char* needle,
            size_t length, const unsigned char*& found)
        {
            for (const unsigned char* at = end - length; at >= begin; --at)
            {
                if (std::memcmp(at, needle, length) == 0)
                {
                    found = at;
                    return true;
                }
            }
            return false;
        }

        // The game fades the music when the player leaves a place in three
        // spots, each guarded by "if CategoryOverride = '' then":
        //  - TfHangar / TfPanelRuins take-off, right after the autosave
        //    SaveGameToFile(..., 'as') that gets 'as' in EDX;
        //  - TfLoad.OnOpen, right after "if SkipIntro then IntroFinished :=
        //    True; if IntroFinished" (mov byte [X],1 / cmp byte [X],0).
        bool ClassifyFadeCall(const Image& image, const unsigned char* call, FadeSite& site)
        {
            const unsigned char guard[] = { 0x8B, 0x40, 0x44, 0x33, 0xD2, 0xE8 };
            const unsigned char* imageStart = image.base;
            const unsigned char* windowStart = (std::max)(imageStart, call - 0x100);
            const unsigned char* guardAt = nullptr;
            if (!Contains((std::max)(windowStart, call - 0x30), call, guard, sizeof(guard), guardAt))
            {
                return false;
            }
            const wchar_t literal[] = L"as";
            for (const unsigned char* at = call - 5; at > (std::max)(windowStart, call - 0xC0); --at)
            {
                if (*at != 0xBA) continue;
                std::uint32_t value = 0;
                std::memcpy(&value, at + 1, sizeof(value));
                const unsigned char* text = Absolute(image, value, sizeof(literal));
                if (text && std::memcmp(text, literal, sizeof(literal)) == 0)
                {
                    site = FadeSite::Takeoff;
                    return true;
                }
            }
            for (const unsigned char* at = (std::max)(windowStart, guardAt - 0x30); at + 14 <= guardAt; ++at)
            {
                if (at[0] == 0xC6 && at[1] == 0x05 && at[6] == 1 && at[7] == 0x80 && at[8] == 0x3D &&
                    std::memcmp(at + 2, at + 9, 4) == 0 && at[13] == 0)
                {
                    site = FadeSite::Loading;
                    return true;
                }
            }
            return false;
        }

        std::vector<PlaceFade> FindPlaceFades(const std::vector<CodeRange>& ranges, unsigned char* fade,
            const Image& image)
        {
            std::vector<PlaceFade> found;
            for (const auto& range : ranges)
            {
                for (size_t offset = 0; offset + 5 <= range.size; ++offset)
                {
                    unsigned char* call = range.begin + offset;
                    if (*call != 0xE8 || CallTarget(call) != fade) continue;
                    FadeSite site = FadeSite::Takeoff;
                    if (ClassifyFadeCall(image, call, site))
                    {
                        found.push_back({ call + 5, site });
                    }
                }
            }
            return found;
        }
    }

    bool FindMusicPoints(unsigned char* imageBase, MusicPoints& points, std::wstring& error)
    {
        points = {};
        const auto ranges = ExecutableRanges(imageBase, error);
        if (ranges.empty())
        {
            if (error.empty()) error = L"no executable sections";
            return false;
        }
        for (const auto& profile : kProfiles)
        {
            const auto plays = FindAll(ranges, profile.play);
            if (plays.empty()) continue;
            const auto stops = FindAll(ranges, profile.stop);
            const auto ramps = FindAll(ranges, profile.ramp);
            const auto fades = FindAll(ranges, profile.fade);
            if (plays.size() != 1 || stops.size() != 1 || ramps.size() != 1 || fades.size() != 1)
            {
                error = std::wstring(L"profile=") + profile.name +
                    L" play=" + std::to_wstring(plays.size()) +
                    L" stop=" + std::to_wstring(stops.size()) +
                    L" ramp=" + std::to_wstring(ramps.size()) +
                    L" fade=" + std::to_wstring(fades.size());
                return false;
            }
            points.playCategory = plays[0];
            points.stopImmediately = stops[0];
            points.volumeRamp = ramps[0];
            points.requestFadeOut = fades[0];
            points.placeFades = FindPlaceFades(ranges, fades[0], DescribeImage(imageBase));
            points.fadeLength = profile.fadeLength;
            points.getFileName = CallTarget(plays[0] + profile.getFileNameCall);
            points.chooseMusicFile = CallTarget(plays[0] + profile.chooseCall);
            points.playLength = profile.playLength;
            points.stopLength = profile.stopLength;
            points.rampLength = profile.rampLength;
            points.profile = profile.name;
            return true;
        }
        error = L"TMusicControl.PlayCategory signature not found";
        return false;
    }

    bool ProbeExecutableFile(const wchar_t* path, MusicPoints& points, std::wstring& error)
    {
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = L"cannot open file";
            return false;
        }
        // SEC_IMAGE lays the sections out like the loader, so RVAs work.
        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE, 0, 0, nullptr);
        CloseHandle(file);
        if (!mapping)
        {
            error = L"cannot map image";
            return false;
        }
        void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(mapping);
        if (!view)
        {
            error = L"cannot view image";
            return false;
        }
        const bool found = FindMusicPoints(static_cast<unsigned char*>(view), points, error);
        UnmapViewOfFile(view);
        return found;
    }
}
