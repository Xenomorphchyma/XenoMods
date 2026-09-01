#include <cstdlib>
#include <iostream>
#include <string>

#include "../include/xeno_plugin_api.h"

extern "C" BOOL WINAPI XenoPlugin_Query(XenoPluginInfoV1* info);
extern "C" DWORD WINAPI XenoPlugin_Initialize(const XenoPluginHostV1* host);
extern "C" int __cdecl XenoCoalitionSupplyLines_GetConfigInt(int key);

namespace
{
    int WINAPI TestConfigGetInt(
        const wchar_t*,
        const wchar_t* section,
        const wchar_t* key,
        int defaultValue,
        int minimumValue,
        int maximumValue)
    {
        int value = defaultValue;
        const std::wstring sectionName = section ? section : L"";
        const std::wstring keyName = key ? key : L"";
        if (sectionName == L"Band80To100" && keyName == L"TransportMin") value = 6;
        else if (sectionName == L"Band80To100" && keyName == L"TransportMax") value = 2;
        else if (sectionName == L"Band60To79" && keyName == L"ProductionBatch") value = 3;
        else if (sectionName == L"StrikeGroup" && keyName == L"MaximumShips") value = 13;
        else if (sectionName == L"General" && keyName == L"MaxConcurrentConvoys") value = 2;
        if (value < minimumValue) value = minimumValue;
        if (value > maximumValue) value = maximumValue;
        return value;
    }

    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            std::exit(1);
        }
    }
}

int main()
{
    XenoPluginInfoV1 info = {};
    info.size = sizeof(info);
    Require(XenoPlugin_Query(&info) == TRUE, "plugin query failed");
    Require(info.requiredHostApi == XENO_NATIVE_HOST_API_V1, "unexpected host ABI");
    Require(std::wstring(info.id) == L"XenoCoalitionSupplyLines", "unexpected plugin id");
    Require(std::wstring(info.version) == L"1.0.83", "unexpected plugin version");

    XenoPluginHostV1 host = {};
    host.size = sizeof(host);
    host.apiVersion = XENO_NATIVE_HOST_API_V1;
    host.configPath = L"test.ini";
    host.configGetInt = &TestConfigGetInt;
    Require(XenoPlugin_Initialize(&host) == 0, "plugin initialization failed");

    Require(XenoCoalitionSupplyLines_GetConfigInt(11) == 6, "transport minimum was not read");
    Require(XenoCoalitionSupplyLines_GetConfigInt(12) == 6, "inverted range was not normalized");
    Require(XenoCoalitionSupplyLines_GetConfigInt(17) == 4, "top production batch default changed");
    Require(XenoCoalitionSupplyLines_GetConfigInt(27) == 3, "configured production batch was not read");
    Require(XenoCoalitionSupplyLines_GetConfigInt(67) == 1, "low-band production batch default changed");
    Require(XenoCoalitionSupplyLines_GetConfigInt(77) == 1, "crisis production batch default changed");
    Require(XenoCoalitionSupplyLines_GetConfigInt(104) == 13, "strike maximum was not read");
    Require(XenoCoalitionSupplyLines_GetConfigInt(105) == 2, "concurrent convoy limit was not read");
    Require(XenoCoalitionSupplyLines_GetConfigInt(999) == -1, "unknown key must be rejected");
    std::cout << "CSL native config tests passed\n";
    return 0;
}
