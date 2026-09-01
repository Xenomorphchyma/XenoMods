#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "live_game_adapter.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace xnd
{
    namespace
    {
        constexpr size_t kDelphiSelfSlotBeforeClassName = 0x20;
        constexpr size_t kDelphiVmtAfterClassName = 0x2C;
        constexpr size_t kDelphiInstanceSizeAfterClassName = 0x04;
        constexpr size_t kDelphiParentAfterClassName = 0x08;

        constexpr size_t kGalaxyDayOffset = 0x4C;
        constexpr size_t kStarStableIdOffset = 0x08;
        constexpr size_t kStarShipListOffset = 0x2C;
        constexpr size_t kStarInactiveDaysOffset = 0x70;
        constexpr size_t kListItemsOffset = 0x04;
        constexpr size_t kListCountOffset = 0x08;
        constexpr int kMaximumShipsPerStar = 8192;

        struct ImageView
        {
            unsigned char* base = nullptr;
            size_t size = 0;
            std::uint32_t preferredBase = 0;
            unsigned char* scanStart = nullptr;
            size_t scanSize = 0;
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
                error = L"game image is not PE32";
                return false;
            }
            view.base = base;
            view.size = nt->OptionalHeader.SizeOfImage;
            view.preferredBase = nt->OptionalHeader.ImageBase;
            if (view.size < nt->OptionalHeader.SizeOfHeaders)
            {
                error = L"game image size is invalid";
                return false;
            }
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
                view.scanStart = base + section.VirtualAddress;
                view.scanSize = sectionSize;
                return true;
            }
            error = L"game image has no code section";
            return false;
        }

        std::uint32_t ReadDword(const unsigned char* pointer)
        {
            std::uint32_t value = 0;
            std::memcpy(&value, pointer, sizeof(value));
            return value;
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

        bool MatchesRelocatedPointer(
            std::uint32_t encoded,
            const ImageView& view,
            const unsigned char* livePointer)
        {
            const size_t rva = static_cast<size_t>(livePointer - view.base);
            if (rva > std::numeric_limits<std::uint32_t>::max())
            {
                return false;
            }
            const std::uint32_t preferred = view.preferredBase +
                static_cast<std::uint32_t>(rva);
            const std::uintptr_t live = reinterpret_cast<std::uintptr_t>(livePointer);
            return encoded == preferred ||
                (live <= std::numeric_limits<std::uint32_t>::max() &&
                 encoded == static_cast<std::uint32_t>(live));
        }

        bool ValidateClassSize(
            const DelphiClassMetadata& metadata,
            std::uint32_t expected,
            const wchar_t* name,
            std::wstring& error)
        {
            if (metadata.instanceSize == expected)
            {
                return true;
            }
            error = std::wstring(name) + L" instance size mismatch: expected " +
                std::to_wstring(expected) + L", got " +
                std::to_wstring(metadata.instanceSize);
            return false;
        }
    }

    bool DiscoverDelphiClass(
        void* imageBase,
        const char* className,
        DelphiClassMetadata& metadata,
        std::wstring& error)
    {
        metadata = {};
        ImageView view;
        if (!BuildImageView(imageBase, view, error) || !className)
        {
            return false;
        }
        const size_t nameLength = std::strlen(className);
        if (nameLength < 2 || nameLength > 63)
        {
            error = L"invalid Delphi class name";
            return false;
        }

        std::vector<unsigned char> pascalName(nameLength + 1);
        pascalName[0] = static_cast<unsigned char>(nameLength);
        std::memcpy(pascalName.data() + 1, className, nameLength);

        unsigned char* nameAddress = FindBytes(
            view.scanStart,
            view.scanSize,
            pascalName.data(),
            pascalName.size());
        while (nameAddress)
        {
            for (size_t offset = kDelphiSelfSlotBeforeClassName;
                 offset + kDelphiVmtAfterClassName < view.scanSize;
                 offset += sizeof(std::uint32_t))
            {
                unsigned char* classNameSlot = view.scanStart + offset;
                if (!MatchesRelocatedPointer(
                        ReadDword(classNameSlot), view, nameAddress))
                {
                    continue;
                }
                unsigned char* selfSlot =
                    classNameSlot - kDelphiSelfSlotBeforeClassName;
                unsigned char* vmt = classNameSlot + kDelphiVmtAfterClassName;
                if (!MatchesRelocatedPointer(ReadDword(selfSlot), view, vmt))
                {
                    continue;
                }

                const std::uint32_t instanceSize = ReadDword(
                    classNameSlot + kDelphiInstanceSizeAfterClassName);
                if (instanceSize < sizeof(void*) || instanceSize > 0x100000)
                {
                    continue;
                }
                const std::uint32_t parentValue = ReadDword(
                    classNameSlot + kDelphiParentAfterClassName);
                metadata.vmt = vmt;
                metadata.vmtRva = static_cast<std::uint32_t>(vmt - view.base);
                metadata.selfSlotRva = static_cast<std::uint32_t>(selfSlot - view.base);
                metadata.instanceSize = instanceSize;
                if (parentValue >= view.preferredBase &&
                    parentValue < view.preferredBase + view.size)
                {
                    metadata.parentSelfSlotRva = parentValue - view.preferredBase;
                }
                else if (parentValue >= reinterpret_cast<std::uintptr_t>(view.base) &&
                         parentValue < reinterpret_cast<std::uintptr_t>(view.base) + view.size)
                {
                    metadata.parentSelfSlotRva = static_cast<std::uint32_t>(
                        parentValue - reinterpret_cast<std::uintptr_t>(view.base));
                }
                return true;
            }

            const size_t consumed = static_cast<size_t>(
                nameAddress - view.scanStart) + 1;
            if (consumed >= view.scanSize)
            {
                break;
            }
            nameAddress = FindBytes(
                view.scanStart + consumed,
                view.scanSize - consumed,
                pascalName.data(),
                pascalName.size());
        }

        std::wstring wideName(className, className + nameLength);
        error = L"Delphi class was not found: " + wideName;
        return false;
    }

    bool DiscoverLiveGameLayout(
        void* imageBase,
        LiveGameLayout& layout,
        std::wstring& error)
    {
        layout = {};
        ImageView view;
        if (!BuildImageView(imageBase, view, error))
        {
            return false;
        }
        layout.imageBase = view.base;
        layout.imageSize = view.size;
        if (!DiscoverDelphiClass(imageBase, "TGalaxy", layout.galaxyClass, error) ||
            !DiscoverDelphiClass(imageBase, "TStar", layout.starClass, error) ||
            !DiscoverDelphiClass(imageBase, "TShip", layout.shipClass, error) ||
            !DiscoverDelphiClass(imageBase, "TKling", layout.dominatorClass, error))
        {
            return false;
        }
        if (!ValidateClassSize(layout.galaxyClass, 0x1DC, L"TGalaxy", error) ||
            !ValidateClassSize(layout.starClass, 0x114, L"TStar", error) ||
            !ValidateClassSize(layout.shipClass, 0x4D0, L"TShip", error) ||
            !ValidateClassSize(layout.dominatorClass, 0x4DC, L"TKling", error))
        {
            return false;
        }
        if (layout.dominatorClass.parentSelfSlotRva !=
            layout.shipClass.selfSlotRva)
        {
            error = L"TKling is not a direct TShip descendant";
            return false;
        }
        layout.verified = true;
        return true;
    }

    bool IsExpectedGalaxy(const LiveGameLayout& layout, void* galaxy)
    {
        __try
        {
            return layout.verified && galaxy &&
                *reinterpret_cast<const void* const*>(galaxy) ==
                    layout.galaxyClass.vmt;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool InspectDominatorOnlyStar(
        const LiveGameLayout& layout,
        void* star,
        int farAfterInactiveDays,
        LiveStarInspection& inspection)
    {
        inspection = {};
        inspection.star = star;
        __try
        {
            if (!star)
            {
                inspection.reason = LiveStarReason::NullStar;
                return false;
            }
            const auto* bytes = static_cast<const unsigned char*>(star);
            if (!layout.verified ||
                *reinterpret_cast<const void* const*>(bytes) !=
                    layout.starClass.vmt)
            {
                inspection.reason = LiveStarReason::StarClassMismatch;
                return false;
            }
            inspection.stableId = *reinterpret_cast<const std::int32_t*>(
                bytes + kStarStableIdOffset);
            inspection.inactiveDays = *reinterpret_cast<const std::int32_t*>(
                bytes + kStarInactiveDaysOffset);
            if (inspection.inactiveDays < 0 || inspection.inactiveDays > 10000000)
            {
                inspection.reason = LiveStarReason::InvalidInactiveDays;
                return false;
            }
            if (inspection.inactiveDays < std::max(farAfterInactiveDays, 1))
            {
                inspection.reason = LiveStarReason::TooNear;
                return false;
            }

            const void* list = *reinterpret_cast<void* const*>(
                bytes + kStarShipListOffset);
            if (!list)
            {
                inspection.reason = LiveStarReason::InvalidShipList;
                return false;
            }
            const auto* listBytes = static_cast<const unsigned char*>(list);
            void* const* items = *reinterpret_cast<void* const* const*>(
                listBytes + kListItemsOffset);
            inspection.shipCount = *reinterpret_cast<const std::int32_t*>(
                listBytes + kListCountOffset);
            if (inspection.shipCount < 0 ||
                inspection.shipCount > kMaximumShipsPerStar ||
                (inspection.shipCount > 0 && !items))
            {
                inspection.reason = LiveStarReason::InvalidShipList;
                return false;
            }
            if (inspection.shipCount == 0)
            {
                inspection.reason = LiveStarReason::EmptyShipList;
                return false;
            }
            for (int index = 0; index < inspection.shipCount; ++index)
            {
                const void* ship = items[index];
                if (!ship || *reinterpret_cast<const void* const*>(ship) !=
                        layout.dominatorClass.vmt)
                {
                    inspection.reason = LiveStarReason::NonDominatorShip;
                    return false;
                }
            }
            inspection.reason = LiveStarReason::Eligible;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            inspection.reason = LiveStarReason::MemoryFault;
            return false;
        }
    }

    bool IncrementInactiveDays(void* star)
    {
        __try
        {
            if (!star)
            {
                return false;
            }
            auto* value = reinterpret_cast<std::int32_t*>(
                static_cast<unsigned char*>(star) + kStarInactiveDaysOffset);
            if (*value < 0 || *value >= 10000000)
            {
                return false;
            }
            ++*value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadGalaxyDay(void* galaxy, int& day)
    {
        __try
        {
            if (!galaxy)
            {
                return false;
            }
            day = *reinterpret_cast<const int*>(
                static_cast<const unsigned char*>(galaxy) + kGalaxyDayOffset);
            return day >= 0 && day < 100000000;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool WriteGalaxyDay(void* galaxy, int day)
    {
        __try
        {
            if (!galaxy || day < 0 || day >= 100000000)
            {
                return false;
            }
            *reinterpret_cast<int*>(
                static_cast<unsigned char*>(galaxy) + kGalaxyDayOffset) = day;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    const wchar_t* LiveStarReasonName(LiveStarReason reason)
    {
        switch (reason)
        {
        case LiveStarReason::Eligible: return L"eligible";
        case LiveStarReason::NullStar: return L"null_star";
        case LiveStarReason::StarClassMismatch: return L"star_class_mismatch";
        case LiveStarReason::InvalidInactiveDays: return L"invalid_inactive_days";
        case LiveStarReason::TooNear: return L"too_near";
        case LiveStarReason::InvalidShipList: return L"invalid_ship_list";
        case LiveStarReason::EmptyShipList: return L"empty_ship_list";
        case LiveStarReason::NonDominatorShip: return L"non_dominator_ship";
        case LiveStarReason::MemoryFault: return L"memory_fault";
        }
        return L"unknown";
    }
}
