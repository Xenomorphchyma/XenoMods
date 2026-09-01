#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "runtime_scan.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace xsr
{
    namespace
    {
        // TScript restart routine. Absolute addresses, relative calls and the
        // Delphi exception target are wildcards. The stable instructions cover
        // the idle check and the first lookup by ScriptName.
        const int kRestartPattern[] = {
            0x55, 0x8B, 0xEC, 0x83, 0xC4, 0xD4, 0x53, 0x33,
            0xDB, 0x89, 0x5D, 0xD4, 0x89, 0x4D, 0xF4, 0x89,
            0x55, 0xF8, 0x89, 0x45, 0xFC, 0x33, 0xC0, 0x55,
            -1, -1, -1, -1, -1, 0x64, 0xFF, 0x30,
            0x64, 0x89, 0x20, 0xC6, 0x45, 0xF3, 0x00, 0x83,
            0x7D, 0xFC, 0x00, 0x0F, 0x84, -1, -1, -1,
            -1, 0x8B, 0x45, 0xFC, 0x8B, 0x40, 0x08, 0xE8,
            -1, -1, -1, -1, 0x89, 0x45, 0xE4, 0x83,
            0x7D, 0xE4, 0x00, 0x0F, 0x8C
        };

        bool ParseImage(
            const unsigned char* base,
            const IMAGE_NT_HEADERS32*& nt,
            const IMAGE_SECTION_HEADER*& sections,
            std::wstring& error)
        {
            if (!base)
            {
                error = L"image base is null";
                return false;
            }
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
                error = L"not a supported x86 PE image";
                return false;
            }
            sections = IMAGE_FIRST_SECTION(nt);
            return true;
        }

        bool PatternMatches(const unsigned char* data)
        {
            for (size_t i = 0; i < _countof(kRestartPattern); ++i)
            {
                if (kRestartPattern[i] >= 0 &&
                    data[i] != static_cast<unsigned char>(kRestartPattern[i]))
                {
                    return false;
                }
            }
            return true;
        }

        std::vector<unsigned char*> FindRuntimeMatches(
            unsigned char* base,
            const IMAGE_NT_HEADERS32* nt,
            const IMAGE_SECTION_HEADER* sections)
        {
            std::vector<unsigned char*> matches;
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            {
                const auto& section = sections[i];
                if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
                {
                    continue;
                }
                const size_t size = section.Misc.VirtualSize;
                if (size < _countof(kRestartPattern) ||
                    section.VirtualAddress > nt->OptionalHeader.SizeOfImage ||
                    size > nt->OptionalHeader.SizeOfImage - section.VirtualAddress)
                {
                    continue;
                }
                auto* start = base + section.VirtualAddress;
                for (size_t offset = 0;
                    offset <= size - _countof(kRestartPattern);
                    ++offset)
                {
                    if (PatternMatches(start + offset))
                    {
                        matches.push_back(start + offset);
                    }
                }
            }
            return matches;
        }

        bool ResolveGalaxySlot(
            unsigned char* imageBase,
            const IMAGE_NT_HEADERS32* nt,
            unsigned char* restart,
            void**& galaxySlot,
            std::wstring& error)
        {
            const unsigned char marker[] = {
                0xA1, 0, 0, 0, 0, 0x8B, 0x00,
                0x8B, 0x80, 0x50, 0x01, 0x00, 0x00
            };
            std::uintptr_t found = 0;
            int count = 0;
            for (size_t i = 0; i + sizeof(marker) <= 0x170; ++i)
            {
                if (restart[i] != marker[0] || restart[i + 5] != marker[5] ||
                    restart[i + 6] != marker[6] ||
                    std::memcmp(restart + i + 7, marker + 7, 6) != 0)
                {
                    continue;
                }
                std::uintptr_t candidate = 0;
                std::memcpy(&candidate, restart + i + 1, sizeof(std::uint32_t));
                const auto begin = reinterpret_cast<std::uintptr_t>(imageBase);
                const auto end = begin + nt->OptionalHeader.SizeOfImage;
                if (candidate < begin || candidate + sizeof(void*) > end)
                {
                    continue;
                }
                if (found == 0)
                {
                    found = candidate;
                }
                if (candidate == found)
                {
                    ++count;
                }
            }
            if (found == 0 || count < 2)
            {
                error = L"galaxy slot could not be derived from restart routine";
                return false;
            }
            galaxySlot = reinterpret_cast<void**>(found);
            return true;
        }
    }

    bool DiscoverRuntimePoints(
        void* imageBase,
        RuntimePoints& points,
        std::wstring& error)
    {
        points = {};
        auto* base = static_cast<unsigned char*>(imageBase);
        const IMAGE_NT_HEADERS32* nt = nullptr;
        const IMAGE_SECTION_HEADER* sections = nullptr;
        if (!ParseImage(base, nt, sections, error))
        {
            return false;
        }
        const auto matches = FindRuntimeMatches(base, nt, sections);
        if (matches.size() != 1)
        {
            std::wostringstream text;
            text << L"TScript restart signature count=" << matches.size();
            error = text.str();
            return false;
        }
        void** galaxySlot = nullptr;
        if (!ResolveGalaxySlot(base, nt, matches[0], galaxySlot, error))
        {
            return false;
        }
        points.restartScript = matches[0];
        points.galaxySlot = galaxySlot;
        return true;
    }

    bool VerifyExecutableFile(const wchar_t* path, std::wstring& error)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            error = L"cannot open executable";
            return false;
        }
        std::vector<unsigned char> bytes(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        const IMAGE_NT_HEADERS32* nt = nullptr;
        const IMAGE_SECTION_HEADER* sections = nullptr;
        if (!ParseImage(bytes.data(), nt, sections, error))
        {
            return false;
        }
        size_t matches = 0;
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        {
            const auto& section = sections[i];
            if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
                section.PointerToRawData >= bytes.size())
            {
                continue;
            }
            const size_t available = bytes.size() - section.PointerToRawData;
            const size_t size = (std::min)(
                static_cast<size_t>(section.SizeOfRawData), available);
            if (size < _countof(kRestartPattern))
            {
                continue;
            }
            const auto* start = bytes.data() + section.PointerToRawData;
            for (size_t offset = 0;
                offset <= size - _countof(kRestartPattern);
                ++offset)
            {
                if (PatternMatches(start + offset))
                {
                    ++matches;
                }
            }
        }
        if (matches != 1)
        {
            std::wostringstream text;
            text << L"file TScript restart signature count=" << matches;
            error = text.str();
            return false;
        }
        return true;
    }
}
