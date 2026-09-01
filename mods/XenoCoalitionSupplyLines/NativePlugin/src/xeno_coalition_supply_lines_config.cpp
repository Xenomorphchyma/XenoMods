#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../include/xeno_plugin_api.h"

#include <array>
#include <sstream>
#include <string>

namespace
{
#ifdef CSL_PLUGIN_TEST
    const wchar_t* kPluginId = L"XenoCoalitionSupplyLinesEarthTest";
    const wchar_t* kPluginDescription = L"Editable composition config for CSL EarthTest";
#else
    const wchar_t* kPluginId = L"XenoCoalitionSupplyLines";
    const wchar_t* kPluginDescription = L"Editable composition config for Coalition Supply Lines";
#endif
    const wchar_t* kPluginVersion = L"1.0.83";

    struct BandConfig
    {
        int transportMin;
        int transportMax;
        int escortMin;
        int escortMax;
        int cargoMin;
        int cargoMax;
        int productionBatch;
    };

    constexpr std::array<BandConfig, 7> kDefaults = {{
        {4, 6, 5, 6, 4, 5, 4},
        {3, 5, 4, 6, 3, 5, 3},
        {2, 4, 4, 5, 3, 4, 2},
        {2, 3, 4, 4, 2, 4, 2},
        {2, 3, 3, 4, 2, 3, 1},
        {2, 3, 2, 2, 2, 3, 1},
        {2, 2, 0, 0, 2, 3, 1},
    }};

    constexpr std::array<const wchar_t*, 7> kBandSections = {{
        L"Band80To100",
        L"Band60To79",
        L"Band40To59",
        L"Band20To39",
        L"Band11To19",
        L"BandBelow11",
        L"CrisisBelow3Systems",
    }};

    std::array<BandConfig, 7> g_bands = kDefaults;
    int g_cargoPerEnhancedShip = 2;
    int g_remainderCreatesSimpleShip = 1;
    int g_shipsPerDeliveredEscort = 1;
    int g_maximumStrikeShips = 21;
    int g_maxConcurrentConvoys = 3;
    volatile LONG g_ready = 0;
    XenoHostLogFn g_log = nullptr;

    int ReadInt(
        const XenoPluginHostV1* host,
        const wchar_t* section,
        const wchar_t* key,
        int fallback,
        int minimum,
        int maximum)
    {
        if (!host || host->size < sizeof(XenoPluginHostV1) || !host->configGetInt)
        {
            return fallback;
        }
        return host->configGetInt(
            host->configPath, section, key, fallback, minimum, maximum);
    }

    void NormalizeBand(BandConfig& band)
    {
        if (band.transportMax < band.transportMin) band.transportMax = band.transportMin;
        if (band.escortMax < band.escortMin) band.escortMax = band.escortMin;
        if (band.cargoMax < band.cargoMin) band.cargoMax = band.cargoMin;
    }

    int BandValue(const BandConfig& band, int field)
    {
        switch (field)
        {
        case 1: return band.transportMin;
        case 2: return band.transportMax;
        case 3: return band.escortMin;
        case 4: return band.escortMax;
        case 5: return band.cargoMin;
        case 6: return band.cargoMax;
        case 7: return band.productionBatch;
        default: return -1;
        }
    }
}

extern "C" BOOL WINAPI XenoPlugin_Query(XenoPluginInfoV1* info)
{
    if (!info || info->size < XENO_PLUGIN_INFO_V1_BASE_SIZE) return FALSE;
    info->requiredHostApi = XENO_NATIVE_HOST_API_V1;
    lstrcpynW(info->id, kPluginId, static_cast<int>(_countof(info->id)));
    lstrcpynW(info->version, kPluginVersion, static_cast<int>(_countof(info->version)));
    lstrcpynW(info->description, kPluginDescription, static_cast<int>(_countof(info->description)));
    if (info->size >= sizeof(XenoPluginInfoV1)) info->exclusiveCapabilities = 0;
    return TRUE;
}

extern "C" DWORD WINAPI XenoPlugin_Initialize(const XenoPluginHostV1* host)
{
    if (!host || host->apiVersion < XENO_NATIVE_HOST_API_V1 ||
        host->size < XENO_PLUGIN_HOST_V1_BASE_SIZE)
    {
        return 1;
    }

    g_log = host->log;
    g_bands = kDefaults;
    for (std::size_t index = 0; index < g_bands.size(); ++index)
    {
        BandConfig& band = g_bands[index];
        const wchar_t* section = kBandSections[index];
        band.transportMin = ReadInt(host, section, L"TransportMin", band.transportMin, 1, 6);
        band.transportMax = ReadInt(host, section, L"TransportMax", band.transportMax, 1, 6);
        band.escortMin = ReadInt(host, section, L"EscortMin", band.escortMin, 0, 6);
        band.escortMax = ReadInt(host, section, L"EscortMax", band.escortMax, 0, 6);
        band.cargoMin = ReadInt(host, section, L"CargoPerTransportMin", band.cargoMin, 1, 5);
        band.cargoMax = ReadInt(host, section, L"CargoPerTransportMax", band.cargoMax, 1, 5);
        band.productionBatch = ReadInt(host, section, L"ProductionBatch", band.productionBatch, 1, 4);
        NormalizeBand(band);
    }

    g_cargoPerEnhancedShip = ReadInt(
        host, L"StrikeGroup", L"CargoPerEnhancedShip", 2, 1, 10);
    g_remainderCreatesSimpleShip = ReadInt(
        host, L"StrikeGroup", L"RemainderCreatesSimpleShip", 1, 0, 1);
    g_shipsPerDeliveredEscort = ReadInt(
        host, L"StrikeGroup", L"ShipsPerDeliveredEscort", 1, 0, 3);
    g_maximumStrikeShips = ReadInt(
        host, L"StrikeGroup", L"MaximumShips", 21, 1, 21);
    g_maxConcurrentConvoys = ReadInt(
        host, L"General", L"MaxConcurrentConvoys", 3, 1, 3);

    InterlockedExchange(&g_ready, 1);
    if (g_log)
    {
        std::wostringstream message;
        message << L"config=loaded max_strike=" << g_maximumStrikeShips
                << L" cargo_per_ship=" << g_cargoPerEnhancedShip
                << L" top_batch=" << g_bands[0].productionBatch
                << L" crisis_batch=" << g_bands[6].productionBatch
                << L" max_convoys=" << g_maxConcurrentConvoys;
        g_log(kPluginId, message.str().c_str());
    }
    return 0;
}

extern "C" int __cdecl XenoCoalitionSupplyLines_GetConfigInt(int key)
{
    if (!g_ready) return -1;
    if (key >= 11 && key <= 77)
    {
        const int band = key / 10;
        const int field = key - band * 10;
        if (band >= 1 && band <= 7) return BandValue(g_bands[band - 1], field);
    }
    switch (key)
    {
    case 101: return g_cargoPerEnhancedShip;
    case 102: return g_remainderCreatesSimpleShip;
    case 103: return g_shipsPerDeliveredEscort;
    case 104: return g_maximumStrikeShips;
    case 105: return g_maxConcurrentConvoys;
    default: return -1;
    }
}
