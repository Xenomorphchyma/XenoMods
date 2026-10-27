#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "runtime_scan.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace xnd
{
    namespace
    {
        struct ImageView
        {
            unsigned char* base = nullptr;
            size_t size = 0;
            unsigned char* code = nullptr;
            size_t codeSize = 0;
            std::uint32_t preferredBase = 0;
        };

        bool BuildImageView(void* imageBase, ImageView& view, std::wstring& error)
        {
            if (!imageBase)
            {
                error = L"game image is null";
                return false;
            }
            auto* base = static_cast<unsigned char*>(imageBase);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
            {
                error = L"game image has no valid DOS header";
                return false;
            }
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
                base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE ||
                nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            {
                error = L"game image is not a PE32 image";
                return false;
            }
            view.base = base;
            view.size = nt->OptionalHeader.SizeOfImage;
            view.preferredBase = nt->OptionalHeader.ImageBase;
            const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
            for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index)
            {
                const IMAGE_SECTION_HEADER& section = sections[index];
                if ((section.Characteristics & IMAGE_SCN_CNT_CODE) == 0)
                {
                    continue;
                }
                const size_t sectionSize = std::max<size_t>(
                    section.Misc.VirtualSize,
                    section.SizeOfRawData);
                if (section.VirtualAddress >= view.size ||
                    sectionSize > view.size - section.VirtualAddress)
                {
                    error = L"game code section exceeds the image";
                    return false;
                }
                view.code = base + section.VirtualAddress;
                view.codeSize = sectionSize;
                return true;
            }
            error = L"game image has no code section";
            return false;
        }

        unsigned char* FindBytes(
            unsigned char* start,
            size_t size,
            const unsigned char* needle,
            size_t needleSize)
        {
            if (!start || !needle || needleSize == 0 || size < needleSize)
            {
                return nullptr;
            }
            for (size_t offset = 0; offset <= size - needleSize; ++offset)
            {
                if (std::memcmp(start + offset, needle, needleSize) == 0)
                {
                    return start + offset;
                }
            }
            return nullptr;
        }

        bool InRange(const ImageView& view, const unsigned char* pointer, size_t length)
        {
            return pointer >= view.base &&
                length <= view.size &&
                pointer <= view.base + view.size - length;
        }

        unsigned char* FindDwordBefore(
            const ImageView& view,
            unsigned char* before,
            std::uint32_t value,
            size_t window)
        {
            if (!before || before < view.code || before >= view.code + view.codeSize)
            {
                return nullptr;
            }
            unsigned char needle[4] = {};
            std::memcpy(needle, &value, sizeof(value));
            unsigned char* start = before > view.code + window
                ? before - window
                : view.code;
            unsigned char* found = nullptr;
            for (unsigned char* cursor = start; cursor + 4 <= before; ++cursor)
            {
                if (std::memcmp(cursor, needle, sizeof(needle)) == 0)
                {
                    found = cursor;
                }
            }
            return found;
        }

        // push ebp; mov ebp, esp; add esp, -frame; push ebx; push esi; push edi.
        // The frame size differs between the $O- releases and the $O+ prerelease;
        // both hooks relocate the same nine bytes.
        bool IsLargeFrameProlog(const unsigned char* address)
        {
            static const unsigned char head[] = { 0x55, 0x8B, 0xEC, 0x81, 0xC4 };
            static const unsigned char tail[] = { 0x53, 0x56, 0x57 };
            return std::memcmp(address, head, sizeof(head)) == 0 &&
                std::memcmp(address + 9, tail, sizeof(tail)) == 0;
        }

        unsigned char* FindPrologBefore(
            const ImageView& view,
            unsigned char* before,
            size_t window)
        {
            if (!before || before < view.code || before >= view.code + view.codeSize)
            {
                return nullptr;
            }
            unsigned char* start = before > view.code + window
                ? before - window
                : view.code;
            unsigned char* found = nullptr;
            for (unsigned char* cursor = start; cursor + 12 <= before; ++cursor)
            {
                if (IsLargeFrameProlog(cursor))
                {
                    found = cursor;
                }
            }
            return found;
        }

        // Star.NextDay(False) for stars without the player:
        // xor edx, edx; mov eax, <star>; call TStar.NextDay.
        unsigned char* FindInactiveStarCall(
            const ImageView& view,
            unsigned char* begin,
            unsigned char* end)
        {
            unsigned char* found = nullptr;
            int count = 0;
            for (unsigned char* cursor = begin; cursor + 10 <= end; ++cursor)
            {
                if (cursor[0] != 0x33 || cursor[1] != 0xD2 || cursor[2] != 0x8B)
                {
                    continue;
                }
                // mov eax, reg (8B C0+r) or mov eax, [ebp+disp8] (8B 45 xx).
                unsigned char* call = (cursor[3] & 0xF8) == 0xC0 ? cursor + 4
                    : cursor[3] == 0x45 ? cursor + 5
                    : nullptr;
                if (!call || call[0] != 0xE8 || call + 5 > end)
                {
                    continue;
                }
                std::int32_t displacement = 0;
                std::memcpy(&displacement, call + 1, sizeof(displacement));
                unsigned char* target = call + 5 + displacement;
                if (!InRange(view, target, 12) || !IsLargeFrameProlog(target))
                {
                    continue;
                }
                found = call;
                ++count;
            }
            return count == 1 ? found : nullptr;
        }
    }

    bool DiscoverRuntimePoints(
        void* imageBase,
        RuntimePoints& points,
        std::wstring& error)
    {
        points = {};
        ImageView view;
        if (!BuildImageView(imageBase, view, error))
        {
            return false;
        }

        static const char diagnostic[] =
            "Error in procedure TGalaxy.NextDay label = ";
        unsigned char* stringAddress = FindBytes(
            view.code,
            view.codeSize,
            reinterpret_cast<const unsigned char*>(diagnostic),
            sizeof(diagnostic) - 1);
        if (!stringAddress)
        {
            error = L"TGalaxy.NextDay diagnostic string was not found";
            return false;
        }

        const std::uint32_t stringRva = static_cast<std::uint32_t>(
            stringAddress - view.base);
        const std::uint32_t preferredAddress = view.preferredBase + stringRva;
        const uintptr_t liveAddressValue = reinterpret_cast<uintptr_t>(stringAddress);
        unsigned char* stringReference = nullptr;
        if (liveAddressValue <= std::numeric_limits<std::uint32_t>::max())
        {
            stringReference = FindDwordBefore(
                view,
                stringAddress,
                static_cast<std::uint32_t>(liveAddressValue),
                0x2000);
        }
        if (!stringReference)
        {
            stringReference = FindDwordBefore(
                view, stringAddress, preferredAddress, 0x2000);
        }
        if (!stringReference)
        {
            error = L"TGalaxy.NextDay diagnostic reference was not found";
            return false;
        }

        unsigned char* nextDay = FindPrologBefore(view, stringReference, 0x1000);
        if (!nextDay)
        {
            error = L"TGalaxy.NextDay prolog was not found";
            return false;
        }

        // The inactive-star TStar.NextDay invocation in stage 2 is the only
        // Star.NextDay(False) call before the diagnostic handler.
        unsigned char* starCall = FindInactiveStarCall(view, nextDay, stringReference);
        if (!starCall)
        {
            error = L"TStar.NextDay call site validation failed";
            return false;
        }
        std::int32_t displacement = 0;
        std::memcpy(&displacement, starCall + 1, sizeof(displacement));
        unsigned char* starNextDay = starCall + 5 + displacement;

        points.nextDay = nextDay;
        points.starNextDay = starNextDay;
        points.diagnosticString = stringAddress;
        return true;
    }
}
