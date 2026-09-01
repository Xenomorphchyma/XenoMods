#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <oleauto.h>
#include <stdint.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kSlotsPerPage = 8;
constexpr int kMaxCandidates = 4096;
constexpr int kPageNumberLabelCount = 16;
constexpr int kPrevX = 471;
constexpr int kNextX = 532;
constexpr int kArrowY = 519;
constexpr int kPrevWidth = 21;
constexpr int kNextWidth = 20;
constexpr int kArrowHeight = 36;

const BYTE kRefreshSignature[] = {
    0x55, 0x8b, 0xec, 0x83, 0xc4, 0xb8, 0x33, 0xd2,
    0x89, 0x55, 0xe4, 0x89, 0x45, 0xfc, 0x33, 0xc0
};

// TBaseForm.FindControl has the same instruction layout in the supported
// executables, while three embedded exception/string addresses move with the
// linker.  The first moving address is wildcarded; the remaining 36 bytes are
// stable and make the pattern unique in every tested Rangers executable.
const BYTE kFindControlSignature[] = {
    0x55, 0x8b, 0xec, 0x83, 0xc4, 0xec, 0x33, 0xc9,
    0x89, 0x4d, 0xf0, 0x89, 0x4d, 0xec, 0x89, 0x55,
    0xf8, 0x89, 0x45, 0xfc, 0x33, 0xc0, 0x55, 0x68,
    0x00, 0x00, 0x00, 0x00, 0x64, 0xff, 0x30, 0x64,
    0x89, 0x20, 0x8b, 0x55, 0xf8, 0x8b, 0x45, 0xfc,
    0x8b, 0x40, 0x1c, 0xe8, 0xec, 0xb8, 0xff, 0xff,
    0x89, 0x45, 0xf4, 0x83, 0x7d, 0xf4, 0x00, 0x75,
    0x2f, 0x8d, 0x45, 0xec, 0x8b, 0x4d, 0xf8, 0xba
};
const char kFindControlMask[] =
    "xxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
static_assert(sizeof(kFindControlMask) - 1 == sizeof(kFindControlSignature),
    "FindControl signature and mask length must match");

// The game uses Delphi WideString/BSTR names here.  The value is a pointer to
// UTF-16 data whose preceding DWORD is the byte length.  Passing a C++ literal
// directly makes GetByName read unrelated bytes; using a UnicodeString-style
// character count truncates the name to half its length.
template <size_t CharacterCount>
struct DelphiStaticWideString {
    ULONG byteLength;
    WCHAR data[CharacterCount + 1];
};

const DelphiStaticWideString<17> kPagingPanelName = {
    17 * sizeof(WCHAR), L"HangarPagingPanel"
};

struct Candidate {
    uint32_t id;
    void* ship;
};

struct GatherDiagnostics {
    void* current;
    void* space;
    void* primaryList;
    void* secondaryList;
    int primaryCount;
    int secondaryCount;
    bool planet;
    bool station;
};

BYTE* gBase = nullptr;
HINSTANCE gModule = nullptr;
uintptr_t gRefreshAddress = 0;
uintptr_t gGetCurrentAddress = 0;
uintptr_t gIsPlanetAddress = 0;
uintptr_t gIsStationAddress = 0;
uintptr_t gListItemAddress = 0;
uintptr_t gFindControlAddress = 0;
uintptr_t gBuildShipImageAddress = 0;
uintptr_t gClearUnicodeStringAddress = 0;
uintptr_t gCalcShipScaleAddress = 0;
uintptr_t gBindShipImageAddress = 0;
uintptr_t gDrawShipAddress = 0;
void* gRefreshTrampoline = nullptr;
void* volatile gHangar = nullptr;
volatile LONG gHangarActive = 0;
volatile LONG gPage = 0;
volatile LONG gPageCount = 1;
HWND gGameWindow = nullptr;
WNDPROC gOriginalWndProc = nullptr;
WCHAR gLogPath[MAX_PATH] = {};
volatile LONG gLogPathState = 0;
volatile LONG gLastCandidateCount = -2;
volatile LONG gLastLoggedPage = -2;
volatile LONG gLastPageInputTick = 0;
void* gPagingPanel = nullptr;
void* gPageNumberLabels[kPageNumberLabelCount] = {};
volatile LONG gLastUiPage = -2;
volatile LONG gLastUiPageCount = -2;
volatile LONG gUiBindLogged = 0;

void EnsureLogPath() {
    if (InterlockedCompareExchange(&gLogPathState, 0, 0) == 2) {
        return;
    }
    if (InterlockedCompareExchange(&gLogPathState, 1, 0) != 0) {
        return;
    }

    DWORD length = GetModuleFileNameW(gModule, gLogPath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        gLogPath[0] = L'\0';
        InterlockedExchange(&gLogPathState, 2);
        return;
    }
    WCHAR* separator = gLogPath + length;
    while (separator > gLogPath && separator[-1] != L'\\' && separator[-1] != L'/') {
        --separator;
    }
    const WCHAR fileName[] = L"XenoHangarPaging.Runtime.log";
    if (separator - gLogPath + sizeof(fileName) / sizeof(fileName[0]) >= MAX_PATH) {
        gLogPath[0] = L'\0';
    } else {
        lstrcpyW(separator, fileName);
    }
    InterlockedExchange(&gLogPathState, 2);
}

void LogLine(const WCHAR* format, ...) {
    EnsureLogPath();
    if (!gLogPath[0]) {
        return;
    }

    WCHAR message[1024] = {};
    va_list arguments;
    va_start(arguments, format);
    _vsnwprintf_s(
        message, sizeof(message) / sizeof(message[0]), _TRUNCATE, format, arguments);
    va_end(arguments);

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    WCHAR line[1280] = {};
    _snwprintf_s(
        line, sizeof(line) / sizeof(line[0]), _TRUNCATE,
        L"%04u-%02u-%02u %02u:%02u:%02u %s\r\n",
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, message);

    char bytes[4096] = {};
    int byteCount = WideCharToMultiByte(
        CP_UTF8, 0, line, -1, bytes,
        static_cast<int>(sizeof(bytes) / sizeof(bytes[0])), nullptr, nullptr);
    if (byteCount <= 1) {
        return;
    }
    HANDLE file = CreateFileW(
        gLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, bytes, static_cast<DWORD>(byteCount - 1), &written, nullptr);
    CloseHandle(file);
}

void* GetCurrent() {
    void* result = nullptr;
    uintptr_t fn = gGetCurrentAddress;
    __asm {
        call dword ptr [fn]
        mov result, eax
    }
    return result;
}

bool CallLocationPredicate(void* current, uintptr_t address) {
    BYTE result = 0;
    uintptr_t fn = address;
    __asm {
        mov eax, current
        call dword ptr [fn]
        mov result, al
    }
    return result != 0;
}

void* GetListItem(void* list, int index) {
    void* result = nullptr;
    uintptr_t fn = gListItemAddress;
    __asm {
        mov eax, list
        mov edx, index
        call dword ptr [fn]
        mov result, eax
    }
    return result;
}

void* FindControl(void* parent, const WCHAR* name) {
    void* result = nullptr;
    uintptr_t fn = gFindControlAddress;
    __asm {
        mov eax, parent
        mov edx, name
        call dword ptr [fn]
        mov result, eax
    }
    return result;
}

void RefreshVanilla(void* hangar) {
    void* fn = gRefreshTrampoline;
    __asm {
        mov eax, hangar
        call dword ptr [fn]
    }
}

void BuildShipImage(void* ship, void** text) {
    uintptr_t fn = gBuildShipImageAddress;
    __asm {
        mov eax, ship
        mov edx, text
        call dword ptr [fn]
    }
}

void ClearUnicodeString(void** text) {
    uintptr_t fn = gClearUnicodeStringAddress;
    __asm {
        mov eax, text
        call dword ptr [fn]
    }
}

float CalcShipScale(void* hangar, void* ship) {
    float result = 1.0f;
    uintptr_t fn = gCalcShipScaleAddress;
    __asm {
        mov eax, hangar
        mov edx, ship
        call dword ptr [fn]
        fstp dword ptr [result]
        fwait
    }
    return result;
}

void BindShipImage(void* hangar, int slot, void* text, float scale, BYTE flag) {
    uintptr_t fn = gBindShipImageAddress;
    __asm {
        movzx eax, flag
        push eax
        push dword ptr [scale]
        mov ecx, text
        mov edx, slot
        mov eax, hangar
        call dword ptr [fn]
    }
}

void DrawShip(void* hangar, int slot, BYTE flag) {
    uintptr_t fn = gDrawShipAddress;
    __asm {
        mov eax, hangar
        mov edx, slot
        mov cl, flag
        call dword ptr [fn]
    }
}

void SetControlActive(void* control, bool active) {
    if (!control) {
        return;
    }
    uintptr_t method = *reinterpret_cast<uintptr_t*>(
        *reinterpret_cast<uintptr_t*>(control) + 0x28);
    BYTE value = active ? 1 : 0;
    __asm {
        mov eax, control
        mov dl, value
        call dword ptr [method]
    }
}

bool IsReadableMemory(const void* address, size_t byteCount) {
    if (!address || byteCount == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION information = {};
    if (VirtualQuery(address, &information, sizeof(information)) != sizeof(information) ||
        information.State != MEM_COMMIT ||
        (information.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    uintptr_t start = reinterpret_cast<uintptr_t>(address);
    uintptr_t end = start + byteCount;
    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(information.BaseAddress) +
        information.RegionSize;
    return end >= start && end <= regionEnd;
}

void* FindControlByAllocatedName(void* parent, const WCHAR* name) {
    BSTR allocatedName = SysAllocString(name);
    if (!allocatedName) {
        return nullptr;
    }
    void* result = FindControl(parent, allocatedName);
    SysFreeString(allocatedName);
    return result;
}

void ResetPagingUiBindings() {
    gPagingPanel = nullptr;
    for (int index = 0; index < kPageNumberLabelCount; ++index) {
        gPageNumberLabels[index] = nullptr;
    }
    InterlockedExchange(&gLastUiPage, -2);
    InterlockedExchange(&gLastUiPageCount, -2);
    InterlockedExchange(&gUiBindLogged, 0);
}

void UpdatePagingUi(void* hangar, int page, int pages) {
    if (!gPagingPanel) {
        gPagingPanel = FindControl(hangar, kPagingPanelName.data);
    }
    // Keep the paging strip visible in every valid hangar.  A single-page or
    // empty landing area still shows page 1; ChangePage already ignores input
    // while gPageCount is 1.
    SetControlActive(gPagingPanel, true);
    LONG previousPage = InterlockedCompareExchange(&gLastUiPage, 0, 0);
    LONG previousPages = InterlockedCompareExchange(&gLastUiPageCount, 0, 0);
    if (previousPage == page && previousPages == pages) {
        return;
    }

    InterlockedExchange(&gLastUiPage, page);
    InterlockedExchange(&gLastUiPageCount, pages);
    if (!gPageNumberLabels[0]) {
        for (int index = 0; index < kPageNumberLabelCount; ++index) {
            WCHAR name[32] = {};
            _snwprintf_s(
                name,
                sizeof(name) / sizeof(name[0]),
                _TRUNCATE,
                L"HangarPageNumber%d",
                index + 1);
            gPageNumberLabels[index] = FindControlByAllocatedName(hangar, name);
        }
    }

    int visibleLabel = page;
    if (visibleLabel < 0) {
        visibleLabel = 0;
    } else if (visibleLabel >= kPageNumberLabelCount) {
        visibleLabel = kPageNumberLabelCount - 1;
    }
    for (int index = 0; index < kPageNumberLabelCount; ++index) {
        SetControlActive(gPageNumberLabels[index], index == visibleLabel);
    }

    if (!gPageNumberLabels[visibleLabel]) {
        if (InterlockedCompareExchange(&gUiBindLogged, -1, 0) == 0) {
            LogLine(L"paging ui number-label binding failed; page=%d", page + 1);
        }
    } else if (InterlockedCompareExchange(&gUiBindLogged, 1, 0) == 0) {
        LogLine(L"paging ui bound; panel=%p; static-number-labels=%d",
            gPagingPanel, kPageNumberLabelCount);
    }
}

bool AddCandidate(Candidate* candidates, int& count, void* current, void* ship, uintptr_t locationOffset) {
    if (!ship || ship == current || count >= kMaxCandidates) {
        return false;
    }
    if (*reinterpret_cast<void**>(reinterpret_cast<BYTE*>(ship) + locationOffset) !=
        *reinterpret_cast<void**>(reinterpret_cast<BYTE*>(current) + locationOffset)) {
        return false;
    }
    uint32_t id = *reinterpret_cast<uint32_t*>(reinterpret_cast<BYTE*>(ship) + 4);
    if (id == 0) {
        return false;
    }
    for (int index = 0; index < count; ++index) {
        if (candidates[index].id == id) {
            return false;
        }
    }
    candidates[count].id = id;
    candidates[count].ship = ship;
    ++count;
    return true;
}

void CollectList(Candidate* candidates, int& count, void* current, void* list, uintptr_t locationOffset) {
    if (!list || count >= kMaxCandidates) {
        return;
    }
    int listCount = *reinterpret_cast<int*>(reinterpret_cast<BYTE*>(list) + 8);
    if (listCount < 0 || listCount > kMaxCandidates) {
        return;
    }
    for (int index = 0; index < listCount && count < kMaxCandidates; ++index) {
        AddCandidate(candidates, count, current, GetListItem(list, index), locationOffset);
    }
}

int ReadListCount(void* list) {
    if (!list) {
        return 0;
    }
    int count = *reinterpret_cast<int*>(reinterpret_cast<BYTE*>(list) + 8);
    return count >= 0 && count <= kMaxCandidates ? count : -1;
}

int GatherCandidates(Candidate* candidates, GatherDiagnostics* diagnostics) {
    if (diagnostics) {
        std::memset(diagnostics, 0, sizeof(*diagnostics));
    }
    void* current = GetCurrent();
    if (!current) {
        return 0;
    }

    int count = 0;
    BYTE* currentBytes = reinterpret_cast<BYTE*>(current);
    void* space = *reinterpret_cast<void**>(currentBytes + 0x24);
    void* primaryList = space
        ? *reinterpret_cast<void**>(reinterpret_cast<BYTE*>(space) + 0x2c)
        : nullptr;

    bool isPlanet = CallLocationPredicate(current, gIsPlanetAddress);
    bool isStation = !isPlanet && CallLocationPredicate(current, gIsStationAddress);
    void* secondaryList = nullptr;

    if (isPlanet) {
        CollectList(candidates, count, current, primaryList, 0x1c);
        void* planet = *reinterpret_cast<void**>(currentBytes + 0x1c);
        secondaryList = planet
            ? *reinterpret_cast<void**>(reinterpret_cast<BYTE*>(planet) + 0x114)
            : nullptr;
        CollectList(candidates, count, current, secondaryList, 0x1c);
    } else if (isStation) {
        CollectList(candidates, count, current, primaryList, 0x20);
    }

    if (diagnostics) {
        diagnostics->current = current;
        diagnostics->space = space;
        diagnostics->primaryList = primaryList;
        diagnostics->secondaryList = secondaryList;
        diagnostics->primaryCount = ReadListCount(primaryList);
        diagnostics->secondaryCount = ReadListCount(secondaryList);
        diagnostics->planet = isPlanet;
        diagnostics->station = isStation;
    }

    for (int index = 1; index < count; ++index) {
        Candidate value = candidates[index];
        int position = index;
        while (position > 0 && candidates[position - 1].id > value.id) {
            candidates[position] = candidates[position - 1];
            --position;
        }
        candidates[position] = value;
    }
    return count;
}

void HideSlot(BYTE* hangar, int slot) {
    BYTE* record = hangar + 0xf8 + slot * 0x14;
    *reinterpret_cast<uint32_t*>(record) = 3;
    *reinterpret_cast<uint32_t*>(record + 4) = 0;
    *reinterpret_cast<uint32_t*>(record + 0x0c) = 0xff;
    SetControlActive(*reinterpret_cast<void**>(record + 0x10), false);
}

void PrepareSlot(BYTE* hangar, int slot, const Candidate& candidate) {
    BYTE* record = hangar + 0xf8 + slot * 0x14;
    // State 0 with a non-zero ID is the stock hangar's own "bind this ship"
    // request.  Let the original refresh routine build and attach the image;
    // calling its lower-level helpers directly leaves the graph buffer stale.
    *reinterpret_cast<uint32_t*>(record) = 0;
    *reinterpret_cast<uint32_t*>(record + 4) = candidate.id;
    *reinterpret_cast<uint32_t*>(record + 0x0c) = 0;
    SetControlActive(*reinterpret_cast<void**>(record + 0x10), false);
}

void ApplyPage(void* hangarValue, bool resetInfo) {
    if (!hangarValue || InterlockedCompareExchange(&gHangarActive, 0, 0) == 0) {
        return;
    }

    Candidate candidates[kMaxCandidates];
    GatherDiagnostics diagnostics = {};
    int count = GatherCandidates(candidates, &diagnostics);

    // The stock hangar already populated its eight records before this hook runs.
    // If paging is unnecessary or discovery failed, preserving those records is
    // safer than trying to recreate the vanilla layout.
    if (count <= kSlotsPerPage) {
        InterlockedExchange(&gPage, 0);
        InterlockedExchange(&gPageCount, 1);
        UpdatePagingUi(hangarValue, 0, 1);
        LONG previousCount = InterlockedExchange(&gLastCandidateCount, count);
        LONG previousPage = InterlockedExchange(&gLastLoggedPage, 0);
        if (previousCount != count || previousPage != 0) {
            LogLine(
                L"hangar preserved; location=%s; candidates=%d; primary=%d; secondary=%d; current=%p",
                diagnostics.planet ? L"planet" : (diagnostics.station ? L"station" : L"unknown"),
                count, diagnostics.primaryCount, diagnostics.secondaryCount, diagnostics.current);
        }
        return;
    }

    int pages = count > 0 ? (count + kSlotsPerPage - 1) / kSlotsPerPage : 1;
    LONG page = InterlockedCompareExchange(&gPage, 0, 0);
    if (page < 0) {
        page = pages - 1;
    }
    if (page >= pages) {
        page = 0;
    }
    InterlockedExchange(&gPage, page);
    InterlockedExchange(&gPageCount, pages);
    UpdatePagingUi(hangarValue, page, pages);

    LONG previousCount = InterlockedCompareExchange(&gLastCandidateCount, 0, 0);
    LONG previousPage = InterlockedCompareExchange(&gLastLoggedPage, 0, 0);
    BYTE* hangar = reinterpret_cast<BYTE*>(hangarValue);
    int first = page * kSlotsPerPage;
    bool recordsMatchPage = true;
    for (int slot = 1; slot <= kSlotsPerPage; ++slot) {
        BYTE* record = hangar + 0xf8 + slot * 0x14;
        int candidateIndex = first + slot - 1;
        uint32_t state = *reinterpret_cast<uint32_t*>(record);
        uint32_t shipId = *reinterpret_cast<uint32_t*>(record + 4);
        if (candidateIndex < count) {
            // State 1 is the stock refresh's visible, ready record. State 2 is
            // a matching record whose GraphBuf must be republished without
            // rebinding the image. Only state 1 can skip this correction pass.
            if (state != 1 || shipId != candidates[candidateIndex].id) {
                recordsMatchPage = false;
                break;
            }
        } else if (state != 3 || shipId != 0) {
            recordsMatchPage = false;
            break;
        }
    }
    // The stock refresh is called every frame.  Reapply only if that refresh
    // actually changed a slot away from the requested page.  Comparing only
    // the page/count missed partial-page pollution; redrawing unconditionally
    // made the GraphBuf controls flicker invisible.
    if (previousCount == count && previousPage == page && recordsMatchPage) {
        return;
    }
    bool selectionChanged = previousCount != count || previousPage != page;
    InterlockedExchange(&gLastCandidateCount, count);
    InterlockedExchange(&gLastLoggedPage, page);
    if (selectionChanged) {
        LogLine(
            L"page applied; location=%s; candidates=%d; primary=%d; secondary=%d; page=%d/%d",
            diagnostics.planet ? L"planet" : (diagnostics.station ? L"station" : L"unknown"),
            count, diagnostics.primaryCount, diagnostics.secondaryCount, page + 1, pages);
    }

    if (resetInfo && selectionChanged) {
        *reinterpret_cast<void**>(hangar + 0xe4) = nullptr;
        SetControlActive(*reinterpret_cast<void**>(hangar + 0xe0), false);
    }

    bool preparedSlots[kSlotsPerPage + 1] = {};
    bool refreshNeeded = false;
    int preparedCount = 0;
    int hiddenCount = 0;
    for (int slot = 1; slot <= kSlotsPerPage; ++slot) {
        BYTE* record = hangar + 0xf8 + slot * 0x14;
        int candidateIndex = first + slot - 1;
        if (candidateIndex < count) {
            uint32_t state = *reinterpret_cast<uint32_t*>(record);
            uint32_t shipId = *reinterpret_cast<uint32_t*>(record + 4);
            if (shipId == candidates[candidateIndex].id && state == 1) {
                // The stock pass already left the correct ship visible.
                continue;
            }
            if (shipId == candidates[candidateIndex].id && state == 2) {
                // State 2 is not a request for a different image. Reactivate
                // and republish the existing GraphBuf directly; the old path
                // first deactivated and rebound all eight records, which made
                // settled ships visibly blink on every stock refresh.
                SetControlActive(*reinterpret_cast<void**>(record + 0x10), true);
                DrawShip(hangar, slot, static_cast<BYTE>(
                    *reinterpret_cast<uint32_t*>(record + 0x0c)));
                continue;
            }
            PrepareSlot(hangar, slot, candidates[candidateIndex]);
            preparedSlots[slot] = true;
            refreshNeeded = true;
            ++preparedCount;
        } else {
            // Empty places need no full stock redraw.  Deactivating only their
            // GraphBuf controls removes vanilla duplicates without touching
            // the valid ships that are already visible on this page.
            HideSlot(hangar, slot);
            ++hiddenCount;
        }
    }

    if (refreshNeeded) {
        // Consume only the slots whose IDs actually changed.  Matching slots
        // stayed active throughout this pass and therefore cannot flicker.
        RefreshVanilla(hangar);
    }

    int rendered = 0;
    int mismatched = 0;
    for (int slot = 1; slot <= kSlotsPerPage; ++slot) {
        BYTE* record = hangar + 0xf8 + slot * 0x14;
        int candidateIndex = first + slot - 1;
        if (candidateIndex >= count) {
            if (refreshNeeded) {
                // A refresh needed by another slot may reactivate an empty
                // GraphBuf, so clear only that empty place once more.
                HideSlot(hangar, slot);
            }
        } else if (!preparedSlots[slot]) {
            continue;
        } else if (*reinterpret_cast<uint32_t*>(record) == 1 &&
            *reinterpret_cast<uint32_t*>(record + 4) == candidates[candidateIndex].id) {
            // The stock pass binds the new ship image but does not always
            // reactivate a GraphBuf that paging deliberately hid first.  This
            // affected pages after the first one: records and IDs were valid,
            // yet every landing place stayed visually empty.
            SetControlActive(*reinterpret_cast<void**>(record + 0x10), true);
            // The original refresh can bind/draw while the GraphBuf is still
            // inactive.  Repeating only its final draw step after activation
            // publishes the already prepared image immediately instead of on
            // the next unrelated hangar refresh.
            DrawShip(hangar, slot, static_cast<BYTE>(
                *reinterpret_cast<uint32_t*>(record + 0x0c)));
            ++rendered;
        } else {
            ++mismatched;
        }
    }
    if (selectionChanged || preparedCount > 0) {
        LogLine(
            L"slot correction completed; page=%d/%d; prepared=%d; rendered=%d; hidden=%d; mismatched=%d",
            page + 1, pages, preparedCount, rendered, hiddenCount, mismatched);
    }
}

void __cdecl AfterRefresh(void* hangar) {
    bool newVisit = InterlockedCompareExchange(&gHangarActive, 1, 0) == 0 || gHangar != hangar;
    gHangar = hangar;
    if (newVisit) {
        InterlockedExchange(&gPage, 0);
        InterlockedExchange(&gLastCandidateCount, -2);
        InterlockedExchange(&gLastLoggedPage, -2);
        ResetPagingUiBindings();
        LogLine(L"hangar opened; instance=%p", hangar);
    }
    ApplyPage(hangar, true);
}

extern "C" void __declspec(naked) RefreshHook() {
    __asm {
        push eax
        call dword ptr [gRefreshTrampoline]
        pop eax
        push eax
        call AfterRefresh
        add esp, 4
        ret
    }
}

bool PointInArrow(HWND window, LPARAM lParam, int logicalX, int width) {
    RECT client = {};
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) {
        return false;
    }
    int x = GET_X_LPARAM(lParam);
    int y = GET_Y_LPARAM(lParam);
    int left = logicalX + (client.right - 1024) / 2;
    int top = kArrowY + (client.bottom - 768) / 2;
    return x >= left && x < left + width &&
        y >= top && y < top + kArrowHeight;
}

void ChangePage(int delta) {
    if (InterlockedCompareExchange(&gHangarActive, 0, 0) == 0) {
        return;
    }
    LONG pages = InterlockedCompareExchange(&gPageCount, 0, 0);
    if (pages <= 1) {
        return;
    }
    LONG page = InterlockedCompareExchange(&gPage, 0, 0);
    page = (page + delta) % pages;
    if (page < 0) {
        page += pages;
    }
    InterlockedExchange(&gPage, page);
    ApplyPage(const_cast<void*>(gHangar), true);
}

bool AcceptPageInput() {
    LONG now = static_cast<LONG>(GetTickCount());
    LONG previous = InterlockedExchange(&gLastPageInputTick, now);
    return previous == 0 || static_cast<DWORD>(now - previous) >= 300;
}

LRESULT CALLBACK PagingWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    WNDPROC original = gOriginalWndProc;
    LRESULT result = original
        ? CallWindowProcW(original, window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
    if (InterlockedCompareExchange(&gHangarActive, 0, 0) == 0) {
        return result;
    }
    if (message == WM_LBUTTONUP) {
        if (PointInArrow(window, lParam, kPrevX, kPrevWidth) && AcceptPageInput()) {
            ChangePage(-1);
        } else if (PointInArrow(window, lParam, kNextX, kNextWidth) && AcceptPageInput()) {
            ChangePage(1);
        }
    } else if (message == WM_KEYUP) {
        if (wParam == VK_LEFT && AcceptPageInput()) {
            ChangePage(-1);
        } else if (wParam == VK_RIGHT && AcceptPageInput()) {
            ChangePage(1);
        }
    }
    return result;
}

BOOL CALLBACK FindGameWindow(HWND window, LPARAM data) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId() || GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    RECT client = {};
    if (!GetClientRect(window, &client) || client.right < 640 || client.bottom < 480) {
        return TRUE;
    }
    *reinterpret_cast<HWND*>(data) = window;
    return FALSE;
}

DWORD WINAPI WindowInstallerThread(void*) {
    LogLine(
        L"runtime 1.1.18 loaded; paging controls stay visible in empty, single-page, and multi-page hangars; arrows are half-gap aligned to the page number; vanilla ranger info positioning is untouched and its InfoPanel layer stays above paging; matching state 2 records remain stable; signatures resolved dynamically; refresh-rva=0x%08x; current-rva=0x%08x",
        static_cast<unsigned>(gRefreshAddress - reinterpret_cast<uintptr_t>(gBase)),
        static_cast<unsigned>(gGetCurrentAddress - reinterpret_cast<uintptr_t>(gBase)));
    for (;;) {
        HWND window = nullptr;
        EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&window));
        if (window) {
            WNDPROC current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
            if (!current) {
                Sleep(50);
                continue;
            }
            gOriginalWndProc = current;
            SetLastError(0);
            WNDPROC original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PagingWndProc)));
            if (original || GetLastError() == 0) {
                gGameWindow = window;
                LogLine(L"window hook installed; hwnd=%p", window);
                return 0;
            }
            gOriginalWndProc = nullptr;
        }
        Sleep(50);
    }
}

BYTE* FindUniqueExecutablePattern(
    IMAGE_NT_HEADERS* headers,
    const BYTE* pattern,
    const char* mask,
    size_t length) {
    BYTE* match = nullptr;
    int matchCount = 0;
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(headers);
    for (WORD sectionIndex = 0; sectionIndex < headers->FileHeader.NumberOfSections;
         ++sectionIndex, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            section->Misc.VirtualSize < length) {
            continue;
        }
        BYTE* start = gBase + section->VirtualAddress;
        size_t size = section->Misc.VirtualSize;
        for (size_t offset = 0; offset + length <= size; ++offset) {
            bool equal = true;
            for (size_t byteIndex = 0; byteIndex < length; ++byteIndex) {
                if ((!mask || mask[byteIndex] != '?') &&
                    start[offset + byteIndex] != pattern[byteIndex]) {
                    equal = false;
                    break;
                }
            }
            if (equal) {
                match = start + offset;
                ++matchCount;
            }
        }
    }
    return matchCount == 1 ? match : nullptr;
}

uintptr_t ResolveRelativeCall(BYTE* instruction) {
    if (!instruction || instruction[0] != 0xe8) {
        return 0;
    }
    int32_t displacement = *reinterpret_cast<int32_t*>(instruction + 1);
    return reinterpret_cast<uintptr_t>(instruction + 5 + displacement);
}

bool IsAddressInImage(uintptr_t address, size_t imageSize) {
    uintptr_t base = reinterpret_cast<uintptr_t>(gBase);
    return address >= base && address < base + imageSize;
}

void* MakeTrampoline(BYTE* target, size_t stolenBytes) {
    BYTE* trampoline = reinterpret_cast<BYTE*>(VirtualAlloc(
        nullptr, stolenBytes + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        return nullptr;
    }
    memcpy(trampoline, target, stolenBytes);
    trampoline[stolenBytes] = 0xe9;
    *reinterpret_cast<int32_t*>(trampoline + stolenBytes + 1) =
        static_cast<int32_t>((target + stolenBytes) - (trampoline + stolenBytes + 5));
    return trampoline;
}

bool PatchJump(BYTE* target, void* hook, size_t stolenBytes) {
    DWORD oldProtection = 0;
    if (!VirtualProtect(target, stolenBytes, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        return false;
    }
    target[0] = 0xe9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(
        reinterpret_cast<BYTE*>(hook) - (target + 5));
    for (size_t index = 5; index < stolenBytes; ++index) {
        target[index] = 0x90;
    }
    FlushInstructionCache(GetCurrentProcess(), target, stolenBytes);
    DWORD ignored = 0;
    VirtualProtect(target, stolenBytes, oldProtection, &ignored);
    return true;
}

bool InstallHooks() {
    gBase = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    if (!gBase) {
        return false;
    }
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(gBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    IMAGE_NT_HEADERS* headers = reinterpret_cast<IMAGE_NT_HEADERS*>(gBase + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE ||
        headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return false;
    }
    size_t imageSize = headers->OptionalHeader.SizeOfImage;

    BYTE* refresh = FindUniqueExecutablePattern(
        headers, kRefreshSignature, nullptr, sizeof(kRefreshSignature));
    BYTE* findControl = FindUniqueExecutablePattern(
        headers, kFindControlSignature, kFindControlMask, sizeof(kFindControlSignature));
    if (!refresh || !findControl) {
        return false;
    }
    gRefreshAddress = reinterpret_cast<uintptr_t>(refresh);

    // These calls are part of the verified stock hangar refresh body.  Their
    // relative positions are identical in the classic 2.1.2500 executable,
    // the Stable/BigGalaxy variants and the reordered Rangers1 build.
    gGetCurrentAddress = ResolveRelativeCall(refresh + 0x06d);
    gIsPlanetAddress = ResolveRelativeCall(refresh + 0x072);
    gIsStationAddress = ResolveRelativeCall(refresh + 0x090);
    gClearUnicodeStringAddress = ResolveRelativeCall(refresh + 0x1d4);
    gBuildShipImageAddress = ResolveRelativeCall(refresh + 0x1e3);
    gCalcShipScaleAddress = ResolveRelativeCall(refresh + 0x23a);
    gBindShipImageAddress = ResolveRelativeCall(refresh + 0x24f);
    gDrawShipAddress = ResolveRelativeCall(refresh + 0x26a);
    gListItemAddress = ResolveRelativeCall(refresh + 0x370);
    gFindControlAddress = reinterpret_cast<uintptr_t>(findControl);

    const uintptr_t resolved[] = {
        gGetCurrentAddress,
        gIsPlanetAddress,
        gIsStationAddress,
        gClearUnicodeStringAddress,
        gBuildShipImageAddress,
        gCalcShipScaleAddress,
        gBindShipImageAddress,
        gDrawShipAddress,
        gListItemAddress,
        gFindControlAddress
    };
    for (size_t index = 0; index < sizeof(resolved) / sizeof(resolved[0]); ++index) {
        if (!IsAddressInImage(resolved[index], imageSize)) {
            return false;
        }
    }

    gRefreshTrampoline = MakeTrampoline(refresh, 6);
    if (!gRefreshTrampoline) {
        return false;
    }
    if (!PatchJump(refresh, reinterpret_cast<void*>(RefreshHook), 6)) {
        return false;
    }
    return true;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        gModule = instance;
        DisableThreadLibraryCalls(instance);
        if (!InstallHooks()) {
            return FALSE;
        }
        HANDLE thread = CreateThread(nullptr, 0, WindowInstallerThread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
