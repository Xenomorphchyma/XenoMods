#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace xmf
{
    enum class FadeSite
    {
        Takeoff, // Take off from a planet or a station (after the autosave)
        Loading  // Loading screen between places
    };

    struct PlaceFade
    {
        unsigned char* returnAddress = nullptr;
        FadeSite site = FadeSite::Takeoff;
    };

    // Routines of TMusicControl (GR_Music) found by signature. Every field
    // offset the plugin uses is the same in all known builds.
    struct MusicPoints
    {
        unsigned char* playCategory = nullptr;   // TMusicControl.PlayCategory
        unsigned char* stopImmediately = nullptr; // TMusicControl.StopImmediately
        unsigned char* volumeRamp = nullptr;     // TSoundBuffer.StartVolumeRamp
        unsigned char* requestFadeOut = nullptr; // TMusicControl.RequestFadeOut
        // Calls of RequestFadeOut that only mark leaving a place; every other
        // caller (next track, battles, music switched off) stays as it is.
        std::vector<PlaceFade> placeFades;
        void* getFileName = nullptr;             // TMusicUnit.GetFileName
        void* chooseMusicFile = nullptr;         // ChooseMusicFile
        // Whole instructions at each routine entry moved to a trampoline.
        size_t playLength = 0;
        size_t stopLength = 0;
        size_t rampLength = 0;
        size_t fadeLength = 0;
        const wchar_t* profile = L"";
    };

    bool FindMusicPoints(unsigned char* imageBase, MusicPoints& points, std::wstring& error);

    // Maps an executable file without running it and scans it the same way.
    bool ProbeExecutableFile(const wchar_t* path, MusicPoints& points, std::wstring& error);
}
