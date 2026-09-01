#pragma once

#include <windows.h>
#include <string>

namespace xnd
{
    struct RuntimePoints
    {
        unsigned char* nextDay = nullptr;
        unsigned char* starNextDay = nullptr;
        unsigned char* diagnosticString = nullptr;
    };

    bool DiscoverRuntimePoints(
        void* imageBase,
        RuntimePoints& points,
        std::wstring& error);
}
