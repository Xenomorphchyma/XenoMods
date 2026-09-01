#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../src/mask_policy.h"
#include "../src/runtime_scan.h"
#include "../src/xdr_config.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::vector<unsigned char> MapPeImage(const wchar_t* path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            throw std::runtime_error("could not open test executable");
        }
        std::vector<unsigned char> file(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        Require(file.size() >= sizeof(IMAGE_DOS_HEADER), "test PE is too small");
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
        Require(dos->e_magic == IMAGE_DOS_SIGNATURE, "test PE has no MZ header");
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            file.data() + dos->e_lfanew);
        Require(nt->Signature == IMAGE_NT_SIGNATURE, "test PE has no PE header");
        std::vector<unsigned char> image(nt->OptionalHeader.SizeOfImage, 0);
        const size_t headers = std::min<size_t>(
            nt->OptionalHeader.SizeOfHeaders, file.size());
        std::copy_n(file.data(), headers, image.data());
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
                section.VirtualAddress + section.SizeOfRawData <= image.size(),
                "test PE section exceeds mapped image");
            std::copy_n(
                file.data() + section.PointerToRawData,
                section.SizeOfRawData,
                image.data() + section.VirtualAddress);
        }
        return image;
    }

    void TestPolicy()
    {
        Require(
            xdr::IsConfusedByChameleon(0, true, 0, false, 2),
            "matching reliable camouflage was rejected");
        Require(
            !xdr::IsConfusedByChameleon(1, true, 0, false, 0),
            "wrong-series normal camouflage was accepted");
        Require(
            !xdr::IsConfusedByChameleon(0, true, 0, true, 1),
            "detected logic-1 camouflage was accepted");
        Require(
            xdr::IsConfusedByChameleon(0, true, 0, true, 2),
            "logic-2 camouflage did not override detection");
        Require(
            !xdr::IsConfusedByChameleon(3, true, 0, false, 2),
            "invalid Dominator series was accepted");
        Require(
            xdr::IsPersistedManagedMask(true, 1, true, 1, false, 2),
            "saved managed camouflage was not recognized");
        Require(
            !xdr::IsPersistedManagedMask(false, 1, true, 1, false, 2),
            "a first application was mistaken for a loaded session");
        Require(
            !xdr::IsPersistedManagedMask(true, 1, true, 2, false, 2),
            "wrong-series saved camouflage was accepted");
        Require(
            !xdr::IsPersistedManagedMask(true, 1, true, 1, true, 2),
            "detected camouflage was mistaken for the managed pattern");
    }

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
        if (sectionName == L"Recruitment" && keyName == L"ChancePercent") value = 35;
        else if (sectionName == L"Recruitment" && keyName == L"MaximumSpecialists") value = 9;
        else if (sectionName == L"Mission" && keyName == L"MaximumSalvageTurns") value = 45;
        else if (sectionName == L"Safety" && keyName == L"SafeLootStarRadius") value = 700;
        else if (sectionName == L"Safety" && keyName == L"SolarCarefulRadius") value = 500;
        else if (sectionName == L"Safety" && keyName == L"SolarRetreatRadius") value = 800;
        if (value < minimumValue) value = minimumValue;
        if (value > maximumValue) value = maximumValue;
        return value;
    }

    void TestConfig()
    {
        XenoPluginHostV1 host = {};
        host.size = sizeof(host);
        host.apiVersion = XENO_NATIVE_HOST_API_V1;
        host.configPath = L"test.ini";
        host.configGetInt = &TestConfigGetInt;
        xdr::UserConfig config;
        xdr::LoadUserConfig(&host, config);
        Require(xdr::GetUserConfigValue(config, 2) == 35, "recruitment chance config was not read");
        Require(xdr::GetUserConfigValue(config, 3) == 3, "specialist limit was not clamped");
        Require(xdr::GetUserConfigValue(config, 13) == 45, "salvage duration config was not read");
        Require(xdr::GetUserConfigValue(config, 20) == 700, "safe loot radius config was not read");
        Require(xdr::GetUserConfigValue(config, 23) == 800, "solar retreat radius config was not read");
        Require(xdr::GetUserConfigValue(config, 21) == 900, "solar careful radius was not normalized");
        Require(xdr::GetUserConfigValue(config, 999) == -1, "unknown config key was accepted");
    }

    void TestExecutable(const wchar_t* path)
    {
        std::vector<unsigned char> image = MapPeImage(path);
        xdr::RuntimePoints points;
        std::wstring error;
        if (!xdr::DiscoverRuntimePoints(image.data(), points, error))
        {
            std::wcerr << L"signature_error=" << error << L" file=" << path << L"\n";
            throw std::runtime_error("runtime signature discovery failed");
        }
        const auto rva = static_cast<std::uintptr_t>(
            points.chameleonConfusion - image.data());
        Require(
            rva == 0x1CECA8 || rva == 0x1EF330,
            "unexpected chameleon check RVA");
        std::wcout << L"signature_test=ok file=" << path
                   << L" chameleon_rva=0x" << std::hex << std::uppercase
                   << rva << std::dec << L"\n";
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        TestPolicy();
        std::cout << "mask_policy_tests=ok\n";
        TestConfig();
        std::cout << "config_tests=ok\n";
        for (int index = 1; index < argc; ++index)
        {
            TestExecutable(argv[index]);
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "native_tests=failed " << error.what() << "\n";
        return 1;
    }
}
