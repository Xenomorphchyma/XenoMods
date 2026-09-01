#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

namespace xgg
{
    struct RuntimePoints
    {
        unsigned char* sector = nullptr;
        unsigned char* contour = nullptr;
        unsigned char* contourRemove = nullptr;
        unsigned char* contourOriginalTarget = nullptr;
        unsigned char* distribution = nullptr;
        unsigned char* distributionComplete = nullptr;
        unsigned char* retry = nullptr;
        unsigned char* geometryCall = nullptr;
        unsigned char* geometryOriginalTarget = nullptr;
        unsigned char* fullGenerator = nullptr;
        void*** configRootSlot = nullptr;
        int** mapWidthSlot = nullptr;
        int** mapHeightSlot = nullptr;
        void* parCount = nullptr;
        void* parGet = nullptr;
    };

    struct RuntimeBaseSchedulerPoint
    {
        unsigned char* call = nullptr;
        unsigned char* originalTarget = nullptr;
    };

    bool DiscoverRuntimePoints(
        void* imageBase,
        RuntimePoints& points,
        std::wstring& error);

    bool DiscoverRuntimeBaseSchedulerPoint(
        void* imageBase,
        RuntimeBaseSchedulerPoint& point,
        std::wstring& error);
}
