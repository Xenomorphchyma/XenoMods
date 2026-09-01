#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "runtime_scan.h"

#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace xdr
{
    namespace
    {
        // TShip chameleon check used by TKling targeting. The wrapper in the
        // unmodified game only accepts Player as its target; the plugin extends
        // that one decision to explicitly registered NPC rangers.
        const int kChameleonConfusionPattern[] = {
            0x55, 0x8B, 0xEC, 0x83, 0xC4, 0xF4,
            0x89, 0x55, 0xF8, 0x89, 0x45, 0xFC, 0xC6, 0x45, 0xF7, 0x00,
            0xE8, -1, -1, -1, -1, 0x85, 0xC0, 0x0F, 0x84, -1, -1, -1, -1,
            0xE8, -1, -1, -1, -1, 0x3B, 0x45, 0xF8,
            0x0F, 0x85, -1, -1, -1, -1,
            0x8B, 0x45, 0xFC, 0xE8, -1, -1, -1, -1, 0x84, 0xC0,
            0x0F, 0x85, -1, -1, -1, -1,
            0x8B, 0x45, 0xF8, 0xE8, -1, -1, -1, -1, 0x84, 0xC0,
            0x0F, 0x85, -1, -1, -1, -1,
            0x8B, 0x45, 0xFC, 0x0F, 0xB6, 0x80, 0xD1, 0x04, 0x00, 0x00,
            0x8B, 0x55, 0xF8, 0x80, 0xBC, 0x02, 0x68, 0x04, 0x00, 0x00, 0x00
        };

        const unsigned char kExpectedPrologue[] = {
            0x55, 0x8B, 0xEC, 0x83, 0xC4, 0xF4
        };

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
                    for (size_t index = 0; index < patternLength; ++index)
                    {
                        if (pattern[index] >= 0 &&
                            start[offset + index] !=
                                static_cast<unsigned char>(pattern[index]))
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
    }

    bool DiscoverRuntimePoints(
        void* imageBase,
        RuntimePoints& points,
        std::wstring& error)
    {
        const auto matches = FindMatches(
            imageBase,
            kChameleonConfusionPattern,
            _countof(kChameleonConfusionPattern),
            error);
        if (matches.size() != 1)
        {
            std::wostringstream message;
            message << L"chameleon confusion signature count=" << matches.size();
            error = message.str();
            return false;
        }
        if (std::memcmp(
                matches[0], kExpectedPrologue, sizeof(kExpectedPrologue)) != 0)
        {
            std::wostringstream message;
            message << L"unexpected chameleon prologue at 0x"
                    << std::hex << std::uppercase
                    << reinterpret_cast<std::uintptr_t>(matches[0]);
            error = message.str();
            return false;
        }
        points.chameleonConfusion = matches[0];
        return true;
    }
}
