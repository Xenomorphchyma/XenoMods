#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../include/xeno_plugin_api.h"
#include "../src/runtime_scan.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

extern "C" DWORD WINAPI XenoPlugin_Initialize(const XenoPluginHostV1* host);

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void WINAPI TestLog(const wchar_t* pluginId, const wchar_t* message)
    {
        std::wcout << L"log[" << (pluginId ? pluginId : L"") << L"] "
                   << (message ? message : L"") << L"\n";
    }

    unsigned char* MapPeImage(const wchar_t* path)
    {
        std::ifstream stream(path, std::ios::binary);
        Require(static_cast<bool>(stream), "could not open test executable");
        std::vector<unsigned char> file(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        Require(file.size() >= sizeof(IMAGE_DOS_HEADER), "test PE is too small");
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
        Require(dos->e_magic == IMAGE_DOS_SIGNATURE, "test PE has no MZ header");
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            file.data() + dos->e_lfanew);
        Require(nt->Signature == IMAGE_NT_SIGNATURE, "test PE has no PE header");
        auto* image = static_cast<unsigned char*>(VirtualAlloc(
            nullptr,
            nt->OptionalHeader.SizeOfImage,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE));
        Require(image != nullptr, "VirtualAlloc failed");
        std::memset(image, 0, nt->OptionalHeader.SizeOfImage);
        const size_t headers = std::min<size_t>(
            nt->OptionalHeader.SizeOfHeaders, file.size());
        std::copy_n(file.data(), headers, image);
        const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
        for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index)
        {
            const IMAGE_SECTION_HEADER& section = sections[index];
            if (!section.SizeOfRawData)
            {
                continue;
            }
            Require(
                section.PointerToRawData + section.SizeOfRawData <= file.size(),
                "test PE section exceeds file");
            Require(
                section.VirtualAddress + section.SizeOfRawData <=
                    nt->OptionalHeader.SizeOfImage,
                "test PE section exceeds image");
            std::copy_n(
                file.data() + section.PointerToRawData,
                section.SizeOfRawData,
                image + section.VirtualAddress);
        }
        return image;
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        Require(argc == 2, "expected one Rangers executable path");
        unsigned char* image = MapPeImage(argv[1]);
        xgg::RuntimePoints points;
        std::wstring error;
        Require(xgg::DiscoverRuntimePoints(image, points, error),
            "runtime points were not discovered");
        xgg::RuntimeBaseSchedulerPoint baseScheduler;
        Require(
            xgg::DiscoverRuntimeBaseSchedulerPoint(image, baseScheduler, error),
            "runtime-base scheduler point was not discovered");
        const std::vector<unsigned char> baseSchedulerOriginal(
            baseScheduler.call, baseScheduler.call + 5);

        XenoPluginHostV1 host = {};
        host.size = sizeof(host);
        host.apiVersion = XENO_NATIVE_HOST_API_V1;
        host.gameModule = reinterpret_cast<HMODULE>(image);
        host.log = &TestLog;
        const DWORD result = XenoPlugin_Initialize(&host);
        Require(result == 0, "plugin initialization failed");
        Require(points.sector[0] == 0xE9, "sector hook was not installed");
        Require(points.geometryCall[0] == 0xE8, "geometry hook was not installed");
        Require(!std::equal(
            baseSchedulerOriginal.begin(), baseSchedulerOriginal.end(),
            baseScheduler.call),
            "runtime-base scheduler throttle hook was not installed");
        Require(
            xgg::DiscoverRuntimePoints(image, points, error) == false,
            "patched image unexpectedly matched pristine signatures");
        std::wcout << L"hook_smoke=ok file=" << argv[1] << L"\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "hook_smoke=failed " << error.what() << "\n";
        return 1;
    }
}
