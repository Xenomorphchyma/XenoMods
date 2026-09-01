#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstddef>
#include <cstdint>

static const std::uint32_t XENO_NATIVE_HOST_API_V1 = 1;

typedef void (WINAPI* XenoHostLogFn)(const wchar_t* pluginId, const wchar_t* message);
typedef BOOL (WINAPI* XenoConfigGetBoolFn)(
    const wchar_t*, const wchar_t*, const wchar_t*, BOOL);
typedef int (WINAPI* XenoConfigGetIntFn)(
    const wchar_t*, const wchar_t*, const wchar_t*, int, int, int);
typedef DWORD (WINAPI* XenoConfigGetStringFn)(
    const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, wchar_t*, DWORD);

struct XenoPluginHostV1
{
    std::uint32_t size;
    std::uint32_t apiVersion;
    HMODULE gameModule;
    const wchar_t* gameRoot;
    const wchar_t* executablePath;
    const wchar_t* pluginRoot;
    const wchar_t* configPath;
    XenoHostLogFn log;
    XenoConfigGetBoolFn configGetBool;
    XenoConfigGetIntFn configGetInt;
    XenoConfigGetStringFn configGetString;
};

static const std::uint32_t XENO_PLUGIN_HOST_V1_BASE_SIZE =
    static_cast<std::uint32_t>(offsetof(XenoPluginHostV1, configGetBool));

struct XenoPluginInfoV1
{
    std::uint32_t size;
    std::uint32_t requiredHostApi;
    wchar_t id[64];
    wchar_t version[32];
    wchar_t description[160];
    std::uint64_t exclusiveCapabilities;
};

static const std::uint32_t XENO_PLUGIN_INFO_V1_BASE_SIZE =
    static_cast<std::uint32_t>(offsetof(XenoPluginInfoV1, exclusiveCapabilities));

