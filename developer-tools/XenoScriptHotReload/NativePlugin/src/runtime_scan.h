#pragma once

#include <windows.h>
#include <string>

namespace xsr
{
    struct RuntimePoints
    {
        void* restartScript = nullptr;
        void** galaxySlot = nullptr;
    };

    bool DiscoverRuntimePoints(
        void* imageBase,
        RuntimePoints& points,
        std::wstring& error);

    bool VerifyExecutableFile(
        const wchar_t* path,
        std::wstring& error);
}
