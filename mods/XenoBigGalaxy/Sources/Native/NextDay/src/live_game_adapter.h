#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace xnd
{
    struct DelphiClassMetadata
    {
        const unsigned char* vmt = nullptr;
        std::uint32_t vmtRva = 0;
        std::uint32_t selfSlotRva = 0;
        std::uint32_t parentSelfSlotRva = 0;
        std::uint32_t instanceSize = 0;
    };

    struct LiveGameLayout
    {
        unsigned char* imageBase = nullptr;
        size_t imageSize = 0;
        DelphiClassMetadata galaxyClass;
        DelphiClassMetadata starClass;
        DelphiClassMetadata shipClass;
        DelphiClassMetadata dominatorClass;
        bool verified = false;
    };

    enum class LiveStarReason
    {
        Eligible,
        NullStar,
        StarClassMismatch,
        InvalidInactiveDays,
        TooNear,
        InvalidShipList,
        EmptyShipList,
        NonDominatorShip,
        MemoryFault
    };

    struct LiveStarInspection
    {
        void* star = nullptr;
        std::int32_t stableId = 0;
        std::int32_t inactiveDays = 0;
        std::int32_t shipCount = 0;
        LiveStarReason reason = LiveStarReason::MemoryFault;

        bool Eligible() const
        {
            return reason == LiveStarReason::Eligible;
        }
    };

    bool DiscoverDelphiClass(
        void* imageBase,
        const char* className,
        DelphiClassMetadata& metadata,
        std::wstring& error);

    bool DiscoverLiveGameLayout(
        void* imageBase,
        LiveGameLayout& layout,
        std::wstring& error);

    bool IsExpectedGalaxy(const LiveGameLayout& layout, void* galaxy);

    bool InspectDominatorOnlyStar(
        const LiveGameLayout& layout,
        void* star,
        int farAfterInactiveDays,
        LiveStarInspection& inspection);

    bool IncrementInactiveDays(void* star);
    bool ReadGalaxyDay(void* galaxy, int& day);
    bool WriteGalaxyDay(void* galaxy, int day);
    const wchar_t* LiveStarReasonName(LiveStarReason reason);
}
