#include "../mods/room_titles/native_ui.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>

namespace native_ui = sdgo::room_titles::native_ui;
using native_ui::Surface;

namespace {

struct NativeString {
    union {
        wchar_t small[8];
        const wchar_t* pointer;
        std::uint8_t storage[16];
    } value;
    std::uint32_t length;
    std::uint32_t capacity;
};

static_assert(sizeof(NativeString) == 24);

struct FakeEdit {
    std::array<std::uint8_t, 0x2A4> bytes = {};
};

struct FakeDialog {
    FakeEdit* edit = nullptr;
    void* button = nullptr;
    void* staticText = nullptr;
};

struct FakeService {
    std::array<std::uint8_t, 0x80> bytes = {};
};

struct FakeButton {
    std::array<std::uint8_t, 0x198> bytes = {};
    std::array<NativeString*, 2> itemPointers = {};
};

struct FakeTitleBar {
    std::array<std::uint8_t, 0x160> bytes = {};
};

struct FakeStaticText {
    std::array<std::uint8_t, 0x260> bytes = {};
};

struct FakeRoom {
    std::array<std::uint8_t, 0xA0> bytes = {};
};

void* g_editVtable[8] = {};
void* g_buttonVtable[32] = {};
void* g_staticTextVtable[40] = {};
void* g_titlebarVtable[8] = {};
NativeString g_native = {};
std::wstring g_heapText;
std::wstring g_setText;
std::map<NativeString*, std::wstring> g_nativeStorage;
std::array<wchar_t, 64> g_wideBuffer = {};
std::array<char, 0x400> g_legacyBuffer = {};
bool g_originalRan = false;
std::uint32_t g_originalArgument = 0;
std::unordered_map<std::string, void*> g_detours;
int g_callbacks = 0;
FakeEdit* g_callbackEdit = nullptr;
Surface g_expectedSurface = Surface::PvpCreate;
FakeButton* g_renderButton = nullptr;
FakeButton* g_pvpListButton = nullptr;
FakeStaticText* g_missionStatic = nullptr;
bool g_renderOriginalRan = false;
bool g_lastBattleDomain = false;
std::uint32_t g_lastRoomId = 0;

void SetPointer(void* owner, std::size_t offset, void* value)
{
    std::memcpy(static_cast<std::uint8_t*>(owner) + offset, &value, sizeof(value));
}

std::uint32_t Limit(const FakeEdit& edit)
{
    std::uint32_t value = 0;
    std::memcpy(&value, edit.bytes.data() + 0x2A0, sizeof(value));
    return value;
}

void InitEdit(FakeEdit* edit)
{
    void* table = g_editVtable;
    std::memcpy(edit->bytes.data(), &table, sizeof(table));
}

const wchar_t* Data(const NativeString& text)
{
    return text.capacity < 8 ? text.value.small : text.value.pointer;
}

void SetNative(NativeString* target, const std::wstring& text)
{
    *target = {};
    target->length = static_cast<std::uint32_t>(text.size());
    if (text.size() < 8) {
        std::memcpy(target->value.small, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        target->capacity = 7;
        return;
    }
    auto& storage = g_nativeStorage[target];
    storage = text;
    target->value.pointer = storage.c_str();
    target->capacity = static_cast<std::uint32_t>(storage.size());
}

std::wstring GetNative(const NativeString& text)
{
    return std::wstring(Data(text), text.length);
}

NativeString* NativeAt(void* owner, std::size_t offset)
{
    return reinterpret_cast<NativeString*>(static_cast<std::uint8_t*>(owner) + offset);
}

void* __fastcall FakeFindControl(void* dialog, void*, const NativeString* name, int type)
{
    if (!dialog || !name)
        return nullptr;
    const std::wstring control(Data(*name), name->length);
    if (type == 8 && control == L"GameRoomNameEdit")
        return static_cast<FakeDialog*>(dialog)->edit;
    if (type == 4 && control == L"TitleChangeBtn")
        return static_cast<FakeDialog*>(dialog)->button;
    if (type == 3 && control == L"RoomNameStatic")
        return static_cast<FakeDialog*>(dialog)->staticText;
    return nullptr;
}

const NativeString* __fastcall FakeGetText(void*, void*)
{
    return &g_native;
}

void __fastcall FakeSetText(void*, void*, const NativeString* text, int)
{
    g_setText.assign(Data(*text), text->length);
}

const char* __cdecl FakeWideToNarrow(UINT codePage, LPCWSTR text)
{
    if (codePage != 874 || !text)
        return nullptr;
    g_legacyBuffer.fill(0);
    if (std::wstring(text) == L"A\u3042B") {
        g_legacyBuffer[0] = 'A';
        g_legacyBuffer[1] = static_cast<char>(0x82);
        g_legacyBuffer[2] = static_cast<char>(0xA0);
        g_legacyBuffer[3] = 'B';
        return g_legacyBuffer.data();
    }
    std::size_t index = 0;
    while (text[index] && index + 1 < g_legacyBuffer.size()) {
        if (text[index] > 0x7F)
            return nullptr;
        g_legacyBuffer[index] = static_cast<char>(text[index]);
        ++index;
    }
    return g_legacyBuffer.data();
}

const wchar_t* __cdecl FakeNarrowToWide(UINT codePage, const char* text)
{
    if (codePage != 874 || !text)
        return nullptr;
    g_wideBuffer.fill(L'\0');
    std::size_t index = 0;
    while (text[index] && index + 1 < g_wideBuffer.size()) {
        g_wideBuffer[index] = static_cast<unsigned char>(text[index]);
        ++index;
    }
    return g_wideBuffer.data();
}

NativeString* __fastcall FakeNativeAssign(NativeString* destination,
                                          void*,
                                          const NativeString* source,
                                          std::size_t position,
                                          std::size_t count)
{
    const std::wstring value(Data(*source), source->length);
    if (position > value.size())
        std::abort();
    const std::size_t available = value.size() - position;
    SetNative(destination, value.substr(position, std::min(available, count)));
    return destination;
}

NativeString* __fastcall FakeButtonTextItem(void* control, void*, std::uint32_t index)
{
    auto* button = static_cast<FakeButton*>(control);
    return index < button->itemPointers.size() ? button->itemPointers[index] : nullptr;
}

void __fastcall FakeStaticTextSet(void* control, void*, const NativeString* text)
{
    SetNative(NativeAt(control, 0x21C), GetNative(*text));
}

void __fastcall FakeOriginal(void*, void*, std::uint32_t argument)
{
    g_originalRan = true;
    g_originalArgument = argument;
}

void __fastcall FakePvpRender(void*, void*, const void*)
{
    g_renderOriginalRan = true;
    SetNative(g_renderButton->itemPointers[0], L"Room 42: Short [PW]");
    SetNative(g_renderButton->itemPointers[1], L"Short / hover");
}

void __fastcall FakeMissionRender(void*, void*, const void*)
{
    g_renderOriginalRan = true;
    SetNative(g_renderButton->itemPointers[0], L"Mission 17: Task");
    SetNative(g_renderButton->itemPointers[1], L"Task / hover");
}

void __fastcall FakePvpRoomList(void*, void*, const void*)
{
    SetNative(g_pvpListButton->itemPointers[0], L"42");
    SetNative(g_pvpListButton->itemPointers[1], L"Short");
}

void __fastcall FakeMissionRoomRow(void*, void*, int)
{
    SetNative(NativeAt(g_missionStatic->bytes.data(), 0x21C), L"Task");
}

void* FakeSymbol(SdgoMod*, const char* name)
{
    if (!std::strcmp(name, "room_titles.ui_find_control"))
        return reinterpret_cast<void*>(&FakeFindControl);
    if (!std::strcmp(name, "room_titles.ui_edit_get_text"))
        return reinterpret_cast<void*>(&FakeGetText);
    if (!std::strcmp(name, "room_titles.ui_edit_set_text"))
        return reinterpret_cast<void*>(&FakeSetText);
    if (!std::strcmp(name, "room_titles.ui_wide_to_narrow"))
        return reinterpret_cast<void*>(&FakeWideToNarrow);
    if (!std::strcmp(name, "room_titles.ui_legacy_to_wide"))
        return reinterpret_cast<void*>(&FakeNarrowToWide);
    if (!std::strcmp(name, "room_titles.ui_native_string_assign"))
        return reinterpret_cast<void*>(&FakeNativeAssign);
    if (!std::strcmp(name, "room_titles.ui_button_text_item"))
        return reinterpret_cast<void*>(&FakeButtonTextItem);
    if (!std::strcmp(name, "room_titles.ui_static_text_set"))
        return reinterpret_cast<void*>(&FakeStaticTextSet);
    if (!std::strcmp(name, "room_titles.ui_edit_vtable"))
        return g_editVtable;
    if (!std::strcmp(name, "room_titles.ui_button_vtable"))
        return g_buttonVtable;
    if (!std::strcmp(name, "room_titles.ui_static_text_vtable"))
        return g_staticTextVtable;
    if (!std::strcmp(name, "room_titles.ui_titlebar_vtable"))
        return g_titlebarVtable;
    return nullptr;
}

int FakeHookSlot(SdgoMod*, const char* name, void* detour, void** original)
{
    g_detours[name] = detour;
    *original = reinterpret_cast<void*>(&FakeOriginal);
    return 1;
}

int FakeHookCode(SdgoMod*, const char* name, void* detour, void** original)
{
    g_detours[name] = detour;
    if (!std::strcmp(name, "room_titles.ui_pvp_titlebar_render"))
        *original = reinterpret_cast<void*>(&FakePvpRender);
    else if (!std::strcmp(name, "room_titles.ui_mission_titlebar_render"))
        *original = reinterpret_cast<void*>(&FakeMissionRender);
    else if (!std::strcmp(name, "room_titles.ui_pvp_room_list"))
        *original = reinterpret_cast<void*>(&FakePvpRoomList);
    else if (!std::strcmp(name, "room_titles.ui_mission_room_row"))
        *original = reinterpret_cast<void*>(&FakeMissionRoomRow);
    else
        return 0;
    return 1;
}

void FakeLog(SdgoMod*, int, const char*, ...)
{
}

void InitCallback(Surface surface, void*, void* edit)
{
    if (!g_originalRan || surface != g_expectedSurface || edit != g_callbackEdit ||
        Limit(*g_callbackEdit) != 512)
        std::abort();
    ++g_callbacks;
}

bool LookupTitle(bool battleDomain, std::uint32_t roomId, std::wstring* title)
{
    g_lastBattleDomain = battleDomain;
    g_lastRoomId = roomId;
    if (battleDomain && roomId == 42) {
        *title = L"Full PvP title longer than twenty bytes";
        return true;
    }
    if (!battleDomain && roomId == 17) {
        *title = L"Full mission title for task rules";
        return true;
    }
    return false;
}

bool Check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool Invoke(const char* symbol, Surface surface, FakeService* service, std::uint32_t argument)
{
    using Detour = void (__fastcall*)(void*, void*, std::uint32_t);
    g_expectedSurface = surface;
    g_originalRan = false;
    reinterpret_cast<Detour>(g_detours.at(symbol))(service->bytes.data(), nullptr, argument);
    return g_originalRan && g_originalArgument == argument;
}

}  // namespace

int main()
{
    g_buttonVtable[0x70 / sizeof(void*)] =
        reinterpret_cast<void*>(&FakeButtonTextItem);
    SdgoApi api = {};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Log = &FakeLog;
    api.Symbol = &FakeSymbol;
    api.HookCode = &FakeHookCode;
    api.HookSlot = &FakeHookSlot;
    auto* mod = reinterpret_cast<SdgoMod*>(1);
    if (!Check(native_ui::Bind(&api, mod), "Bind"))
        return 1;

    FakeEdit edit;
    InitEdit(&edit);
    FakeDialog dialog = {&edit, nullptr, nullptr};
    FakeService service;
    SetPointer(service.bytes.data(), 0x4C, &dialog);
    if (!Check(native_ui::FindRoomNameEdit(service.bytes.data(), Surface::PvpCreate) == &edit,
               "find create edit") ||
        !Check(native_ui::SetRoomNameLimit(&edit) && Limit(edit) == 512, "set UTF-16 byte limit"))
        return 1;

    g_native = {};
    std::memcpy(g_native.value.small, L"short", 6 * sizeof(wchar_t));
    g_native.length = 5;
    g_native.capacity = 7;
    std::wstring copied;
    if (!Check(native_ui::ReadEditText(&edit, &copied) && copied == L"short", "read small string") ||
        !Check(native_ui::ReadNativeString(&g_native, &copied) && copied == L"short",
               "read event string"))
        return 1;

    const auto RejectNativeString = [&copied](const NativeString* value, const char* message) {
        copied = L"preserve on failure";
        return Check(!native_ui::ReadNativeString(value, &copied) &&
                         copied == L"preserve on failure",
                     message);
    };
    if (!Check(!native_ui::ReadNativeString(&g_native, nullptr), "reject null output") ||
        !RejectNativeString(nullptr, "reject null native string"))
        return 1;

    NativeString malformed = {};
    malformed.length = 257;
    malformed.capacity = 257;
    if (!RejectNativeString(&malformed, "reject overlong native string"))
        return 1;
    malformed = {};
    malformed.length = 6;
    malformed.capacity = 5;
    if (!RejectNativeString(&malformed, "reject capacity shorter than length"))
        return 1;
    malformed = {};
    malformed.length = 8;
    malformed.capacity = 7;
    if (!RejectNativeString(&malformed, "reject oversized inline string"))
        return 1;
    malformed = {};
    std::fill(std::begin(malformed.value.small), std::end(malformed.value.small), L'x');
    malformed.length = 7;
    malformed.capacity = 7;
    if (!RejectNativeString(&malformed, "reject unterminated inline string"))
        return 1;
    malformed = {};
    malformed.value.pointer = nullptr;
    malformed.length = 8;
    malformed.capacity = 8;
    if (!RejectNativeString(&malformed, "reject null heap string"))
        return 1;

    g_heapText = L"mission and pvp long room title";
    g_native = {};
    g_native.value.pointer = g_heapText.c_str();
    g_native.length = static_cast<std::uint32_t>(g_heapText.size());
    g_native.capacity = g_native.length;
    if (!Check(native_ui::ReadEditText(&edit, &copied) && copied == g_heapText, "read heap string") ||
        !Check(native_ui::SetEditText(&edit, g_heapText) && g_setText == g_heapText,
               "set const-reference text"))
        return 1;

    std::string legacy;
    const std::string expectedLegacy = {'A', static_cast<char>(0x82), static_cast<char>(0xA0), 'B'};
    if (!Check(native_ui::LegacyTitleBytes(L"A\u3042B", &legacy) && legacy == expectedLegacy,
               "copy host code-page conversion"))
        return 1;
    if (!Check(!native_ui::LegacyTitleBytes(L"A", nullptr), "reject null legacy output") ||
        !Check(!native_ui::SetEditText(&edit, std::wstring(257, L'x')),
               "reject overlong edit text"))
        return 1;

    if (!Check(native_ui::InstallInitHooks(&InitCallback) && g_detours.size() == 4,
               "install four init slots"))
        return 1;
    g_callbackEdit = &edit;

    FakeService pvpCreate;
    SetPointer(pvpCreate.bytes.data(), 0x4C, &dialog);
    FakeService missionCreate;
    SetPointer(missionCreate.bytes.data(), 0x4C, &dialog);
    FakeService missionSettings;
    SetPointer(missionSettings.bytes.data(), 0x5C, &dialog);
    FakeService pvpTitleEdit;
    SetPointer(pvpTitleEdit.bytes.data(), 0x50, &edit);

    if (!Check(Invoke("room_titles.ui_pvp_create_init", Surface::PvpCreate, &pvpCreate, 1),
               "PvP create ABI") ||
        !Check(Invoke("room_titles.ui_mission_create_init", Surface::MissionCreate, &missionCreate, 2),
               "mission create ABI") ||
        !Check(Invoke("room_titles.ui_mission_settings_init", Surface::MissionSettings,
                      &missionSettings, 3),
               "mission settings ABI") ||
        !Check(Invoke("room_titles.ui_pvp_title_edit_init", Surface::PvpTitleEdit,
                      &pvpTitleEdit, 4),
               "PvP title-edit ABI") ||
        !Check(g_callbacks == 4, "callback after every original init"))
        return 1;

    NativeString normalText = {};
    NativeString hoverText = {};
    SetNative(&normalText, L"before");
    SetNative(&hoverText, L"before");
    FakeButton button;
    void* buttonTable = g_buttonVtable;
    std::memcpy(button.bytes.data(), &buttonTable, sizeof(buttonTable));
    button.itemPointers = {&normalText, &hoverText};
    NativeString** itemBegin = button.itemPointers.data();
    NativeString** itemEnd = itemBegin + button.itemPointers.size();
    SetPointer(button.bytes.data(), 0x190, itemBegin);
    SetPointer(button.bytes.data(), 0x194, itemEnd);
    dialog.button = &button;
    g_renderButton = &button;

    FakeTitleBar titleBar;
    void* titlebarTable = g_titlebarVtable;
    std::memcpy(titleBar.bytes.data(), &titlebarTable, sizeof(titlebarTable));
    SetPointer(titleBar.bytes.data(), 0x48, &dialog);
    SetNative(NativeAt(titleBar.bytes.data(), 0x110), L"Short");

    if (!Check(native_ui::SetTitleBarCachedTitle(
                   titleBar.bytes.data(), L"Direct full cached title"),
               "deep-copy titlebar cache") ||
        !Check(GetNative(*NativeAt(titleBar.bytes.data(), 0x110)) ==
                   L"Direct full cached title",
               "read back titlebar cache") ||
        !Check(native_ui::InstallTitleDisplayHooks(&LookupTitle),
               "install two title display hooks"))
        return 1;

    FakeRoom pvpRoom;
    const std::uint32_t pvpId = 42;
    std::memcpy(pvpRoom.bytes.data(), &pvpId, sizeof(pvpId));
    std::memcpy(pvpRoom.bytes.data() + 0x7A, "Short", 6);
    using RenderDetour = void (__fastcall*)(void*, void*, const void*);
    g_renderOriginalRan = false;
    reinterpret_cast<RenderDetour>(g_detours.at("room_titles.ui_pvp_titlebar_render"))(
        titleBar.bytes.data(), nullptr, pvpRoom.bytes.data());
    if (!Check(g_renderOriginalRan, "PvP native render runs first") ||
        !Check(g_lastBattleDomain && g_lastRoomId == 42, "PvP lookup identity") ||
        !Check(GetNative(*NativeAt(titleBar.bytes.data(), 0x110)) ==
                   L"Full PvP title longer than twenty bytes",
               "PvP full cache") ||
        !Check(GetNative(normalText) ==
                   L"Room 42: Full PvP title longer than twenty bytes [PW]",
               "PvP normal label preserves wrapper") ||
        !Check(GetNative(hoverText) ==
                   L"Full PvP title longer than twenty bytes / hover",
               "PvP hover label"))
        return 1;

    FakeRoom missionRoom;
    const std::uint32_t missionId = 17;
    std::memcpy(missionRoom.bytes.data(), &missionId, sizeof(missionId));
    std::memcpy(missionRoom.bytes.data() + 0x54, "Task", 5);
    g_renderOriginalRan = false;
    reinterpret_cast<RenderDetour>(g_detours.at("room_titles.ui_mission_titlebar_render"))(
        titleBar.bytes.data(), nullptr, missionRoom.bytes.data());
    if (!Check(g_renderOriginalRan, "mission native render runs first") ||
        !Check(!g_lastBattleDomain && g_lastRoomId == 17, "mission lookup identity") ||
        !Check(GetNative(*NativeAt(titleBar.bytes.data(), 0x110)) ==
                   L"Full mission title for task rules",
               "mission full cache") ||
        !Check(GetNative(normalText) ==
                   L"Mission 17: Full mission title for task rules",
               "mission normal label preserves wrapper") ||
        !Check(GetNative(hoverText) ==
                   L"Full mission title for task rules / hover",
               "mission hover label"))
        return 1;

    NativeString pvpNumber = {};
    NativeString pvpListTitle = {};
    SetNative(&pvpNumber, L"before");
    SetNative(&pvpListTitle, L"before");
    FakeButton pvpListButton;
    std::memcpy(pvpListButton.bytes.data(), &buttonTable, sizeof(buttonTable));
    pvpListButton.itemPointers = {&pvpNumber, &pvpListTitle};
    itemBegin = pvpListButton.itemPointers.data();
    itemEnd = itemBegin + pvpListButton.itemPointers.size();
    SetPointer(pvpListButton.bytes.data(), 0x190, itemBegin);
    SetPointer(pvpListButton.bytes.data(), 0x194, itemEnd);
    g_pvpListButton = &pvpListButton;

    std::array<std::uint8_t, 0x90> pvpEntry = {};
    std::memcpy(pvpEntry.data(), &pvpId, sizeof(pvpId));
    std::memcpy(pvpEntry.data() + 0x7A, "Short", 6);
    std::array<std::uint8_t, 0x200> pvpListService = {};
    void* pvpEntryBegin = pvpEntry.data();
    void* pvpEntryEnd = pvpEntry.data() + pvpEntry.size();
    SetPointer(pvpListService.data(), 0x124, pvpEntryBegin);
    SetPointer(pvpListService.data(), 0x128, pvpEntryEnd);
    SetPointer(pvpListService.data(), 0x94, &pvpListButton);
    reinterpret_cast<RenderDetour>(g_detours.at("room_titles.ui_pvp_room_list"))(
        pvpListService.data(), nullptr, nullptr);
    if (!Check(GetNative(pvpListTitle) ==
                   L"Full PvP title longer than twenty bytes",
               "PvP lobby list full title") ||
        !Check(GetNative(pvpNumber) == L"42",
               "PvP lobby list leaves room number item"))
        return 1;

    FakeStaticText missionStatic;
    void* staticTable = g_staticTextVtable;
    std::memcpy(missionStatic.bytes.data(), &staticTable, sizeof(staticTable));
    SetNative(NativeAt(missionStatic.bytes.data(), 0x21C), L"before");
    FakeDialog missionRow = {nullptr, nullptr, &missionStatic};
    g_missionStatic = &missionStatic;
    std::array<std::uint8_t, 0x200> missionListService = {};
    SetPointer(missionListService.data(), 0x9C, &missionRow);
    std::memcpy(missionListService.data() + 0xC8, &missionId, sizeof(missionId));
    std::memcpy(missionListService.data() + 0x11C, "Task", 5);
    using MissionRowDetour = void (__fastcall*)(void*, void*, int);
    reinterpret_cast<MissionRowDetour>(g_detours.at("room_titles.ui_mission_room_row"))(
        missionListService.data(), nullptr, 0);
    if (!Check(GetNative(*NativeAt(missionStatic.bytes.data(), 0x21C)) ==
                   L"Full mission title for task rules",
               "mission lobby list full title"))
        return 1;

    std::cout << "room_titles_native_ui_test passed\n";
    return 0;
}
