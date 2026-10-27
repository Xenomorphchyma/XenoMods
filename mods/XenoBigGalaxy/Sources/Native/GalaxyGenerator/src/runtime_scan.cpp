#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "runtime_scan.h"

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace xgg
{
    namespace
    {
        // Legacy builds use $O-; the 2026-10 prerelease uses $O+. Patterns
        // without an "Optimized" twin match both, with frame slots wildcarded
        // and read back from the matched instructions.
        const int kSectorPattern[] = {
            0x8B, 0x45, 0xF8, 0xC7, 0x40, 0x6C, 0x00, 0x00, 0x80, 0x3F,
            0x8B, 0x45, 0xF8, 0xC7, 0x80, 0x60, 0x01, 0x00, 0x00,
            -1, -1, -1, -1, 0xB2, 0x01, 0xA1, -1, -1, -1, -1,
            0xE8, -1, -1, -1, -1, 0x8B, 0x55, 0xF8,
            0x89, 0x82, 0x64, 0x01, 0x00, 0x00
        };
        const int kSectorOptimizedPattern[] = {
            0xC7, 0x43, 0x6C, 0x00, 0x00, 0x80, 0x3F,
            0xC7, 0x83, 0x60, 0x01, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
            0xB2, 0x01, 0xA1, -1, -1, -1, -1,
            0xE8, -1, -1, -1, -1,
            0x89, 0x83, 0x64, 0x01, 0x00, 0x00
        };
        const int kContourPattern[] = {
            0x80, 0xBD, 0xC7, 0xFE, 0xFF, 0xFF, 0x00,
            0x0F, 0x85, -1, -1, -1, -1,
            0x8B, 0x85, 0x48, 0xFF, 0xFF, 0xFF,
            0x83, 0x78, 0x08, 0x02,
            0x0F, 0x8F, 0x2D, 0xFD, 0xFF, 0xFF,
            0xE9, 0xDC, 0x02, 0x00, 0x00,
            0x8B, 0x85, 0x48, 0xFF, 0xFF, 0xFF
        };
        // Starts at the forced-removal block of a flagged ear-trimming loop.
        // Both such loops match; the second is the legacy contour hook.
        const int kContourOptimizedPattern[] = {
            0x8B, 0xD5, 0x8B, 0xC6, 0xE8, -1, -1, -1, -1,
            0x8B, 0xD5, 0x8B, 0x84, 0x24, -1, -1, -1, -1,
            0xE8, -1, -1, -1, -1,
            0xC6, 0x84, 0x24, -1, -1, -1, -1, 0x01,
            0xEB, -1,
            0xFF, 0x44, 0x24, -1,
            0xFF, 0x8C, 0x24, -1, -1, -1, -1,
            0x0F, 0x85, -1, -1, -1, -1,
            0x80, 0xBC, 0x24, -1, -1, -1, -1, 0x00,
            0x0F, 0x85, -1, -1, -1, -1,
            0x83, 0x7E, 0x08, 0x02,
            0x0F, 0x8F, -1, -1, -1, -1,
            0xE9
        };
        const size_t kContourOptimizedHookOffset = 68;
        const int kDistributionPattern[] = {
            0x83, 0x7D, -1, 0x46, 0x74, 0x06,
            0x83, 0x7D, -1, 0x47, 0x75, 0x15,
            0x8B, 0x45, -1, 0x8B, 0x80, 0x64, 0x01, 0x00, 0x00,
            0x8B, 0x40, 0x08, 0x48, 0x89, 0x45, -1,
            0xE9, -1, -1, -1, -1,
            0x83, 0x7D, -1, 0x41, 0x7D, 0x10,
            0x8B, 0x45, -1, 0xB9, 0x12, 0x00, 0x00, 0x00
        };
        const int kRetryPattern[] = {
            0xFF, 0x85, -1, -1, -1, -1,
            0x81, 0xBD, -1, -1, -1, -1, 0xE8, 0x03, 0x00, 0x00,
            0x7F, 0x0A, 0x80, 0x7D, -1, 0x00,
            0x0F, 0x85, -1, -1, -1, -1
        };
        const int kConfigPattern[] = {
            0xA1, -1, -1, -1, -1, 0x8B, 0x00,
            0xBA, -1, -1, -1, -1,
            0xE8, -1, -1, -1, -1,
            0x85, 0xC0, 0x7E, -1,
            0x8D, 0x4D, -1,
            0xA1, -1, -1, -1, -1, 0x8B, 0x00,
            0xBA, -1, -1, -1, -1,
            0xE8, -1, -1, -1, -1,
            0x8B, 0x45, -1,
            0xE8, -1, -1, -1, -1,
            0xA3, -1, -1, -1, -1
        };
        const int kGeometryPattern[] = {
            0x8B, 0x95, 0x7C, 0xFF, 0xFF, 0xFF,
            0x8B, 0x45, 0xFC,
            0xE8, -1, -1, -1, -1,
            0x8B, 0x85, 0x7C, 0xFF, 0xFF, 0xFF,
            0xE8, -1, -1, -1, -1,
            0xC6, 0x45, 0xCB, 0x00
        };
        const int kGeometryOptimizedPattern[] = {
            0x8B, 0x55, -1,
            0x8B, 0x45, -1,
            0xE8, -1, -1, -1, -1,
            0x8B, 0x45, -1,
            0xE8, -1, -1, -1, -1,
            0xC6, 0x45, -1, 0x00
        };
        const int kMapDimensionsPattern[] = {
            0xA1, -1, -1, -1, -1,
            0xDB, 0x00,
            0xDB, 0x2D, -1, -1, -1, -1,
            0xDE, 0xC9,
            0xA1, -1, -1, -1, -1,
            0xDB, 0x00,
            0xDE, 0xC9,
            0x8B, 0x45, -1,
            0x8B, 0x80, 0x60, 0x01, 0x00, 0x00
        };
        const int kInitialBaseSchedulerPattern[] = {
            0x8A, 0x55, 0xEF,
            0x8B, 0x45, 0xFC,
            0xE8, -1, -1, -1, -1,
            0xEB, 0x29,
            0xFE, 0x45, 0xEF,
            0x80, 0x7D, 0xEF, 0x0D,
            0x0F, 0x85, 0x6B, 0xFF, 0xFF, 0xFF,
            0x8B, 0x45, 0xFC,
            0x8B, 0x40, 0x4C,
            0x83, 0xC0, 0x64,
            0xB9, 0x07, 0x00, 0x00, 0x00,
            0x99, 0xF7, 0xF9,
            0x80, 0xC2, 0x06,
            0x8B, 0x45, 0xFC,
            0xE8, -1, -1, -1, -1
        };
        // TGalaxy.ProcessStationSpawning with Self kept at [ESP].
        const int kInitialBaseSchedulerOptimizedPattern[] = {
            0x43, 0x80, 0xFB, 0x0D, 0x75, -1,
            0x8B, 0x04, 0x24,
            0x8B, 0x40, 0x4C,
            0x83, 0xC0, 0x64,
            0xB9, 0x07, 0x00, 0x00, 0x00,
            0x99, 0xF7, 0xF9,
            0x80, 0xC2, 0x06,
            0x8B, 0x04, 0x24,
            0xE8
        };
        // TGalaxy.NextDay stage 8 call; Self and Stage slots vary per build.
        const int kInitialBaseSchedulerCallPattern[] = {
            0x8B, 0x45, -1,
            0xFF, 0x40, 0x4C,
            0xC7, 0x45, -1, 0x08, 0x00, 0x00, 0x00,
            0x8B, 0x45, -1,
            0xE8, -1, -1, -1, -1,
            0xC7, 0x45, -1, 0x09, 0x00, 0x00, 0x00
        };
        // TThreadCreateNewGame.Execute; the zeroed frame size varies per build.
        const int kFullGeneratorPattern[] = {
            0x55, 0x8B, 0xEC, 0xB9, -1, 0x00, 0x00, 0x00,
            0x6A, 0x00, 0x6A, 0x00, 0x49, 0x75, 0xF9,
            0x53, 0x56, 0x57, 0x89, 0x45, 0xF8, 0x33, 0xC0,
            0x55, 0x68, -1, -1, -1, -1, 0x64, 0xFF, 0x30, 0x64, 0x89, 0x20,
            0x33, 0xC0, 0x89, 0x45, -1, 0x33, -1,
            0x55, 0x68, -1, -1, -1, -1, 0x64, 0xFF, -1, 0x64, 0x89, -1,
            0x66, 0xC7, 0x45, 0xFE, 0x3F, 0x13, 0x9B, 0xDB, 0xE2,
            0x66, 0x81, 0x65, 0xFE, 0xFF, 0xFC, 0xD9, 0x6D, 0xFE,
            0xA1, -1, -1, -1, -1, 0x33, 0xD2, 0x89, 0x10
        };

        const unsigned char kSectorOriginal[] = {
            0xC7, 0x80, 0x60, 0x01, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00
        };
        const unsigned char kSectorOptimizedOriginal[] = {
            0xC7, 0x83, 0x60, 0x01, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00
        };
        const unsigned char kContourOriginal[] = {
            0x0F, 0x8F, 0x2D, 0xFD, 0xFF, 0xFF
        };
        // push ebp; mov ebp, esp; mov ecx, imm32: relocatable 8-byte prologue.
        const int kFullGeneratorOriginal[] = {
            0x55, 0x8B, 0xEC, 0xB9, -1, 0x00, 0x00, 0x00
        };
        const unsigned char kGeometryCallOriginal[] = { 0xE8 };
        const unsigned char kNearCallOriginal[] = { 0xE8 };

        struct SolHookApiBridgeV1
        {
            std::uint32_t size;
            std::uint32_t version;
            void* install;
            void* owner;
            void* log;
            void* report;
            void* resolve;
            void* buildExact;
        };
        using SolHookEnsureInitFn = std::int32_t (__cdecl*)(const XenoPluginHostV1*);
        using SolHookGetApiFn = const SolHookApiBridgeV1* (__cdecl*)();
        using SolHookResolveFn = std::uintptr_t (__cdecl*)(const char*);

        unsigned char* ResolveSolEngineGenerator(
            void* imageBase,
            const XenoPluginHostV1* host)
        {
            const HMODULE solHook = GetModuleHandleW(L"SolHook.XenoPlugin.dll");
            if (!solHook)
            {
                return nullptr;
            }
            const auto ensure = reinterpret_cast<SolHookEnsureInitFn>(
                GetProcAddress(solHook, "SolHook_EnsureInit"));
            const auto getApi = reinterpret_cast<SolHookGetApiFn>(
                GetProcAddress(solHook, "SolHook_GetApi"));
            if (!ensure || !getApi || !host || ensure(host) != 0)
            {
                return nullptr;
            }
            const SolHookApiBridgeV1* api = getApi();
            if (!api || api->version < 1 || api->size < sizeof(SolHookApiBridgeV1) ||
                !api->resolve)
            {
                return nullptr;
            }
            const auto resolve = reinterpret_cast<SolHookResolveFn>(api->resolve);
            const std::uintptr_t address = resolve("ADDR_GenerateConstellationsAndStars");
            const auto image = reinterpret_cast<std::uintptr_t>(imageBase);
            const auto* imageBytes = reinterpret_cast<const unsigned char*>(imageBase);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBytes);
            if (!address || address < image || dos->e_magic != IMAGE_DOS_SIGNATURE)
            {
                return nullptr;
            }
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
                imageBytes + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE ||
                address >= image + nt->OptionalHeader.SizeOfImage)
            {
                return nullptr;
            }
            return reinterpret_cast<unsigned char*>(address);
        }

        bool IsSolEngineDetour(const unsigned char* address)
        {
            if (!address || address[0] != 0xE9)
            {
                return false;
            }
            const std::int32_t relative = *reinterpret_cast<const std::int32_t*>(address + 1);
            const auto target = reinterpret_cast<std::uintptr_t>(address + 5) + relative;
            const HMODULE solEngine = GetModuleHandleW(L"SolEngine.XenoPlugin.dll");
            if (!solEngine)
            {
                return false;
            }
            const auto image = reinterpret_cast<const unsigned char*>(solEngine);
            const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            {
                return false;
            }
            const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
                image + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
            {
                return false;
            }
            const auto base = reinterpret_cast<std::uintptr_t>(image);
            return target >= base && target < base + nt->OptionalHeader.SizeOfImage;
        }

        bool ParseImage(
            const void* imageBase,
            const IMAGE_NT_HEADERS32*& nt,
            const IMAGE_SECTION_HEADER*& sections,
            std::wstring& error)
        {
            if (!imageBase)
            {
                error = L"image base is null";
                return false;
            }
            const auto* base = static_cast<const unsigned char*>(imageBase);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
                dos->e_lfanew > 16 * 1024 * 1024)
            {
                error = L"invalid DOS header";
                return false;
            }
            nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE ||
                nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
                nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            {
                error = L"image is not a supported x86 PE";
                return false;
            }
            sections = IMAGE_FIRST_SECTION(nt);
            return true;
        }

        bool HasSection(void* imageBase, const char* wanted)
        {
            const IMAGE_NT_HEADERS32* nt = nullptr;
            const IMAGE_SECTION_HEADER* sections = nullptr;
            std::wstring error;
            if (!ParseImage(imageBase, nt, sections, error))
            {
                return false;
            }
            for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index)
            {
                char name[9] = {};
                std::memcpy(name, sections[index].Name, 8);
                if (_stricmp(name, wanted) == 0)
                {
                    return true;
                }
            }
            return false;
        }

        std::vector<unsigned char*> FindMatches(
            void* imageBase,
            const int* pattern,
            size_t patternLength,
            std::wstring& error)
        {
            std::vector<unsigned char*> matches;
            const IMAGE_NT_HEADERS32* nt = nullptr;
            const IMAGE_SECTION_HEADER* sections = nullptr;
            if (!ParseImage(imageBase, nt, sections, error))
            {
                return matches;
            }
            auto* base = static_cast<unsigned char*>(imageBase);
            for (WORD sectionIndex = 0;
                sectionIndex < nt->FileHeader.NumberOfSections;
                ++sectionIndex)
            {
                const IMAGE_SECTION_HEADER& section = sections[sectionIndex];
                if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
                {
                    continue;
                }
                const size_t size = section.Misc.VirtualSize;
                if (size < patternLength ||
                    section.VirtualAddress > nt->OptionalHeader.SizeOfImage ||
                    size > nt->OptionalHeader.SizeOfImage - section.VirtualAddress)
                {
                    continue;
                }
                unsigned char* start = base + section.VirtualAddress;
                for (size_t offset = 0; offset <= size - patternLength; ++offset)
                {
                    bool equal = true;
                    for (size_t byteIndex = 0; byteIndex < patternLength; ++byteIndex)
                    {
                        if (pattern[byteIndex] >= 0 &&
                            start[offset + byteIndex] !=
                                static_cast<unsigned char>(pattern[byteIndex]))
                        {
                            equal = false;
                            break;
                        }
                    }
                    if (equal)
                    {
                        matches.push_back(start + offset);
                    }
                }
            }
            return matches;
        }

        unsigned char* RelativeTarget(unsigned char* instruction, size_t length)
        {
            const std::int32_t relative = *reinterpret_cast<std::int32_t*>(
                instruction + length - sizeof(std::int32_t));
            return instruction + length + relative;
        }

        unsigned char* AbsoluteImageTarget(
            void* imageBase,
            std::uint32_t encoded,
            const IMAGE_NT_HEADERS32* nt)
        {
            if (encoded < nt->OptionalHeader.ImageBase)
            {
                return nullptr;
            }
            const std::uint32_t rva = encoded - nt->OptionalHeader.ImageBase;
            return rva < nt->OptionalHeader.SizeOfImage
                ? static_cast<unsigned char*>(imageBase) + rva
                : nullptr;
        }

        bool RequireOriginal(
            const unsigned char* address,
            const unsigned char* expected,
            size_t length,
            const wchar_t* name,
            std::wstring& error)
        {
            if (address && std::memcmp(address, expected, length) == 0)
            {
                return true;
            }
            std::wostringstream message;
            message << L"unexpected machine code at " << name << L" 0x"
                    << std::hex << std::uppercase
                    << reinterpret_cast<std::uintptr_t>(address);
            error = message.str();
            return false;
        }

        bool MatchesAt(const unsigned char* address, const int* pattern, size_t length)
        {
            for (size_t index = 0; index < length; ++index)
            {
                if (pattern[index] >= 0 &&
                    address[index] != static_cast<unsigned char>(pattern[index]))
                {
                    return false;
                }
            }
            return true;
        }

        std::int32_t ReadInt32(const unsigned char* address)
        {
            std::int32_t value = 0;
            std::memcpy(&value, address, sizeof(value));
            return value;
        }

        bool DiscoverSectorAndContour(
            void* imageBase,
            RuntimePoints& points,
            std::wstring& error)
        {
            const auto legacySector = FindMatches(
                imageBase, kSectorPattern, _countof(kSectorPattern), error);
            const auto legacyContour = FindMatches(
                imageBase, kContourPattern, _countof(kContourPattern), error);
            if (legacySector.size() == 1 && legacyContour.size() == 1)
            {
                points.profile = L"legacy";
                points.sector = legacySector[0] + 13;
                points.sectorInEbx = false;
                points.contour = legacyContour[0] + 23;
                points.contourRemove = points.contour - 0x4B;
                const unsigned char contourRemoveExpected[] = {
                    0x8B, 0x55, 0xDC, 0x8B, 0x85, 0x48, 0xFF, 0xFF, 0xFF
                };
                return RequireOriginal(points.sector, kSectorOriginal,
                        sizeof(kSectorOriginal), L"sector hook", error) &&
                    RequireOriginal(points.contour, kContourOriginal,
                        sizeof(kContourOriginal), L"contour hook", error) &&
                    RequireOriginal(points.contourRemove, contourRemoveExpected,
                        sizeof(contourRemoveExpected), L"contour fallback destination", error);
            }
            const auto optimizedSector = FindMatches(
                imageBase, kSectorOptimizedPattern, _countof(kSectorOptimizedPattern), error);
            const auto optimizedContour = FindMatches(
                imageBase, kContourOptimizedPattern, _countof(kContourOptimizedPattern), error);
            if (optimizedSector.size() == 1 && optimizedContour.size() == 2 &&
                optimizedContour[1] > optimizedContour[0] &&
                optimizedContour[1] - optimizedContour[0] < 0x1000)
            {
                points.profile = L"optimized";
                points.sector = optimizedSector[0] + 7;
                points.sectorInEbx = true;
                points.contourRemove = optimizedContour[1];
                points.contour = optimizedContour[1] + kContourOptimizedHookOffset;
                return RequireOriginal(points.sector, kSectorOptimizedOriginal,
                    sizeof(kSectorOptimizedOriginal), L"sector hook", error);
            }
            std::wostringstream message;
            message << L"signature counts: sector=" << legacySector.size()
                    << L"/" << optimizedSector.size()
                    << L", contour=" << legacyContour.size()
                    << L"/" << optimizedContour.size();
            error = message.str();
            return false;
        }

        std::vector<unsigned char*> FindGeometry(
            void* imageBase,
            size_t& callOffset,
            std::wstring& error)
        {
            auto geometry = FindMatches(
                imageBase, kGeometryPattern, _countof(kGeometryPattern), error);
            if (geometry.size() == 1)
            {
                callOffset = 9;
                return geometry;
            }
            geometry = FindMatches(
                imageBase, kGeometryOptimizedPattern, _countof(kGeometryOptimizedPattern), error);
            // WorkingPolygon is both the argument and the object freed next.
            std::vector<unsigned char*> consistent;
            for (unsigned char* candidate : geometry)
            {
                if (candidate[2] == candidate[13])
                {
                    consistent.push_back(candidate);
                }
            }
            callOffset = 6;
            return consistent;
        }
    }

    bool DiscoverRuntimePoints(
        void* imageBase,
        RuntimePoints& points,
        std::wstring& error,
        const XenoPluginHostV1* host)
    {
        points = {};
        if (HasSection(imageBase, ".xbg"))
        {
            error = L"EXE already contains the legacy .xbg embedded patch";
            return false;
        }
        const auto distribution = FindMatches(
            imageBase, kDistributionPattern, _countof(kDistributionPattern), error);
        const auto retry = FindMatches(
            imageBase, kRetryPattern, _countof(kRetryPattern), error);
        const auto configCandidates = FindMatches(
            imageBase, kConfigPattern, _countof(kConfigPattern), error);
        const auto full = FindMatches(
            imageBase, kFullGeneratorPattern, _countof(kFullGeneratorPattern), error);
        unsigned char* foreignFull = nullptr;
        if (full.empty())
        {
            foreignFull = ResolveSolEngineGenerator(imageBase, host);
        }
        size_t geometryCallOffset = 0;
        const auto geometry = FindGeometry(imageBase, geometryCallOffset, error);
        const auto dimensions = FindMatches(
            imageBase, kMapDimensionsPattern, _countof(kMapDimensionsPattern), error);
        const IMAGE_NT_HEADERS32* nt = nullptr;
        const IMAGE_SECTION_HEADER* sections = nullptr;
        if (!ParseImage(imageBase, nt, sections, error))
        {
            return false;
        }
        const wchar_t wantedConfigPath[] = L"Constellations.GalaxyCountStars";
        std::vector<unsigned char*> config;
        for (unsigned char* candidate : configCandidates)
        {
            const std::uint32_t encodedPath =
                *reinterpret_cast<std::uint32_t*>(candidate + 8);
            unsigned char* decodedPath = AbsoluteImageTarget(imageBase, encodedPath, nt);
            const std::uint32_t pathRva = encodedPath >= nt->OptionalHeader.ImageBase
                ? encodedPath - nt->OptionalHeader.ImageBase
                : nt->OptionalHeader.SizeOfImage;
            if (decodedPath &&
                pathRva <= nt->OptionalHeader.SizeOfImage - sizeof(wantedConfigPath) &&
                std::memcmp(decodedPath, wantedConfigPath, sizeof(wantedConfigPath)) == 0)
            {
                config.push_back(candidate);
            }
        }
        if (distribution.size() != 1 ||
            retry.size() != 1 || config.size() != 1 ||
            (full.size() != 1 && foreignFull == nullptr) ||
            geometry.size() != 1 || dimensions.size() != 1)
        {
            std::wostringstream message;
            message << L"signature counts: distribution=" << distribution.size()
                    << L", retry=" << retry.size()
                    << L", config=" << config.size()
                    << L", full=" << full.size()
                    << L", geometry=" << geometry.size()
                    << L", dimensions=" << dimensions.size();
            error = message.str();
            return false;
        }
        if (!DiscoverSectorAndContour(imageBase, points, error))
        {
            return false;
        }

        const unsigned char starSlot = distribution[0][2];
        if (distribution[0][8] != starSlot || distribution[0][35] != starSlot ||
            distribution[0][41] != starSlot ||
            dimensions[0][26] != distribution[0][14])
        {
            error = L"inconsistent generator frame slots";
            return false;
        }
        points.frameStarIndex = static_cast<signed char>(starSlot);
        points.frameSelf = static_cast<signed char>(distribution[0][14]);
        points.frameSector = static_cast<signed char>(distribution[0][27]);
        points.frameRetryCount = ReadInt32(retry[0] + 2);
        if (ReadInt32(retry[0] + 8) != points.frameRetryCount ||
            points.frameRetryCount >= 0 || points.frameRetryCount < -0x1000)
        {
            error = L"inconsistent retry counter slot";
            return false;
        }

        points.contourOriginalTarget = RelativeTarget(points.contour, 6);
        points.distribution = distribution[0];
        unsigned char* completeJump = distribution[0] + 37 + 16;
        points.distributionComplete = completeJump[0] == 0xEB
            ? completeJump + 2 + static_cast<signed char>(completeJump[1])
            : nullptr;
        points.retry = retry[0] + 6;
        points.geometryCall = geometry[0] + geometryCallOffset;
        points.geometryOriginalTarget = RelativeTarget(points.geometryCall, 5);
        points.fullGenerator = full.size() == 1 ? full[0] : foreignFull;
        points.fullGeneratorForeign = full.empty();

        const std::uint32_t rootA = *reinterpret_cast<std::uint32_t*>(config[0] + 1);
        const std::uint32_t rootB = *reinterpret_cast<std::uint32_t*>(config[0] + 25);
        const std::uint32_t pathA = *reinterpret_cast<std::uint32_t*>(config[0] + 8);
        const std::uint32_t pathB = *reinterpret_cast<std::uint32_t*>(config[0] + 32);
        points.configRootSlot = reinterpret_cast<void***>(
            AbsoluteImageTarget(imageBase, rootA, nt));
        const std::uint32_t widthSlot =
            *reinterpret_cast<std::uint32_t*>(dimensions[0] + 1);
        const std::uint32_t heightSlot =
            *reinterpret_cast<std::uint32_t*>(dimensions[0] + 16);
        points.mapWidthSlot = reinterpret_cast<int**>(
            AbsoluteImageTarget(imageBase, widthSlot, nt));
        points.mapHeightSlot = reinterpret_cast<int**>(
            AbsoluteImageTarget(imageBase, heightSlot, nt));
        points.parCount = RelativeTarget(config[0] + 12, 5);
        points.parGet = RelativeTarget(config[0] + 36, 5);

        const unsigned char completeExpected[] = { 0xDB, 0x45, starSlot };
        return rootA == rootB && pathA == pathB && points.configRootSlot &&
            points.mapWidthSlot && points.mapHeightSlot &&
            RequireOriginal(points.distributionComplete, completeExpected,
                sizeof(completeExpected), L"distribution completion", error) &&
            RequireOriginal(points.geometryCall, kGeometryCallOriginal,
                sizeof(kGeometryCallOriginal), L"geometry call", error) &&
            (points.fullGeneratorForeign
                ? (IsSolEngineDetour(points.fullGenerator)
                    ? true
                    : (error = L"unexpected foreign hook at full generator entry", false))
                : (MatchesAt(points.fullGenerator, kFullGeneratorOriginal,
                        _countof(kFullGeneratorOriginal))
                    ? true
                    : (error = L"unexpected machine code at full generator entry", false)));
    }

    bool DiscoverRuntimeBaseSchedulerPoint(
        void* imageBase,
        RuntimeBaseSchedulerPoint& point,
        std::wstring& error)
    {
        const auto legacyScheduler = FindMatches(
            imageBase,
            kInitialBaseSchedulerPattern,
            _countof(kInitialBaseSchedulerPattern),
            error);
        const auto optimizedScheduler = FindMatches(
            imageBase,
            kInitialBaseSchedulerOptimizedPattern,
            _countof(kInitialBaseSchedulerOptimizedPattern),
            error);
        const auto caller = FindMatches(
            imageBase,
            kInitialBaseSchedulerCallPattern,
            _countof(kInitialBaseSchedulerCallPattern),
            error);
        const bool legacy = legacyScheduler.size() == 1;
        if ((legacyScheduler.size() + optimizedScheduler.size()) != 1 ||
            caller.size() != 1 ||
            caller[0][2] != caller[0][15] || caller[0][8] != caller[0][23])
        {
            std::wostringstream message;
            message << L"runtime-base signature counts: scheduler="
                    << legacyScheduler.size() << L"/" << optimizedScheduler.size()
                    << L", caller=" << caller.size();
            error = message.str();
            return false;
        }

        point.call = caller[0] + 16;
        point.originalTarget = RelativeTarget(point.call, 5);
        const unsigned char* scheduler = legacy ? legacyScheduler[0] : optimizedScheduler[0];
        const bool targetMatches = legacy
            ? point.originalTarget == scheduler - 0xA5
            : point.originalTarget < scheduler && scheduler - point.originalTarget < 0x200;
        if (!targetMatches)
        {
            error = L"runtime-base scheduler call does not target the scheduler";
            return false;
        }
        return RequireOriginal(
            point.call,
            kNearCallOriginal,
            sizeof(kNearCallOriginal),
            L"runtime-base scheduler call",
            error);
    }
}
