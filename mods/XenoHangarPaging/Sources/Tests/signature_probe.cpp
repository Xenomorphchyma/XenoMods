// Offline signature probe: resolves every hangar routine in a Rangers.exe
// image without running the game. Usage: SignatureProbe.exe <Rangers.exe>...
#include "../Runtime/XenoHangarPaging.Runtime.cpp"

#include <vector>

namespace {

std::vector<BYTE> MapPeImage(const wchar_t* path) {
    std::vector<BYTE> image;
    HANDLE file = CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return image;
    }
    LARGE_INTEGER size = {};
    GetFileSizeEx(file, &size);
    std::vector<BYTE> raw(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read != raw.size() || raw.size() < sizeof(IMAGE_DOS_HEADER)) {
        return image;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(raw.data() + dos->e_lfanew);
    image.assign(nt->OptionalHeader.SizeOfImage, 0);
    memcpy(image.data(), raw.data(), nt->OptionalHeader.SizeOfHeaders);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
        const size_t length = section->SizeOfRawData < section->Misc.VirtualSize
            ? section->SizeOfRawData
            : section->Misc.VirtualSize;
        if (section->PointerToRawData + length <= raw.size() &&
            section->VirtualAddress + length <= image.size()) {
            memcpy(image.data() + section->VirtualAddress,
                raw.data() + section->PointerToRawData, length);
        }
    }
    return image;
}

unsigned Rva(uintptr_t address) {
    return static_cast<unsigned>(address - reinterpret_cast<uintptr_t>(gBase));
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    int failures = 0;
    for (int index = 1; index < argc; ++index) {
        std::vector<BYTE> image = MapPeImage(argv[index]);
        if (image.empty() || !ResolveRuntimeAddresses(image.data())) {
            wprintf(L"signature_probe=failed file=%s\n", argv[index]);
            ++failures;
            continue;
        }
        wprintf(
            L"signature_probe=ok profile=%s refresh=0x%X current=0x%X planet=0x%X "
            L"station=0x%X image=0x%X scale=0x%X bind=0x%X draw=0x%X list=0x%X "
            L"find=0x%X file=%s\n",
            gRuntimeProfile, Rva(gRefreshAddress), Rva(gGetCurrentAddress),
            Rva(gIsPlanetAddress), Rva(gIsStationAddress), Rva(gBuildShipImageAddress),
            Rva(gCalcShipScaleAddress), Rva(gBindShipImageAddress), Rva(gDrawShipAddress),
            Rva(gListItemAddress), Rva(gFindControlAddress), argv[index]);
    }
    return failures == 0 ? 0 : 1;
}
