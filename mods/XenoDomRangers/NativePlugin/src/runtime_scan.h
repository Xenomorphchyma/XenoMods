#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

namespace xdr
{
    struct RuntimePoints
    {
        unsigned char* chameleonConfusion = nullptr;
    };

    bool DiscoverRuntimePoints(
        void* imageBase,
        RuntimePoints& points,
        std::wstring& error);
}
