#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "runtime_scan.h"
#include "xeno_plugin_api.h"

#include <iostream>
#include <stdexcept>
#include <string>

extern "C" BOOL WINAPI XenoPlugin_Query(XenoPluginInfoV1* info);

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        XenoPluginInfoV1 info = {};
        info.size = sizeof(info);
        Require(XenoPlugin_Query(&info) != FALSE, "plugin query failed");
        Require(wcscmp(info.id, L"xeno.script_hot_reload") == 0,
            "unexpected plugin id");
        Require(wcscmp(info.version, L"0.1.0") == 0,
            "unexpected plugin version");

        int verified = 0;
        for (int index = 1; index < argc; ++index)
        {
            std::wstring error;
            if (!xsr::VerifyExecutableFile(argv[index], error))
            {
                std::wcerr << L"signature_error=" << error
                           << L" file=" << argv[index] << L"\n";
                return 2;
            }
            HMODULE image = LoadLibraryExW(
                argv[index], nullptr, DONT_RESOLVE_DLL_REFERENCES);
            if (!image)
            {
                std::wcerr << L"image_map_error=" << GetLastError()
                           << L" file=" << argv[index] << L"\n";
                return 3;
            }
            xsr::RuntimePoints points;
            error.clear();
            const bool runtimeOk = xsr::DiscoverRuntimePoints(image, points, error);
            FreeLibrary(image);
            if (!runtimeOk || !points.restartScript || !points.galaxySlot)
            {
                std::wcerr << L"runtime_discovery_error=" << error
                           << L" file=" << argv[index] << L"\n";
                return 4;
            }
            std::wcout << L"signature_test=ok file=" << argv[index] << L"\n";
            ++verified;
        }
        std::wcout << L"plugin_query=ok executables_verified=" << verified << L"\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "test failure: " << exception.what() << "\n";
        return 1;
    }
}
