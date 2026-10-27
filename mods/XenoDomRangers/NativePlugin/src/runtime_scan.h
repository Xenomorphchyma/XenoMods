#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

namespace xdr
{
    struct RuntimePoints
    {
        unsigned char* chameleonConfusion = nullptr;
        // Whole prologue instructions relocated into the trampoline.
        size_t hookLength = 0;
        const wchar_t* profile = L"";
    };

    bool DiscoverRuntimePoints(
        void* imageBase,
        RuntimePoints& points,
        std::wstring& error);
}
