#include "native_ui.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>

namespace sdgo {
namespace room_titles {
namespace native_ui {
namespace {

constexpr std::size_t kHostSmallCapacity = 7;
constexpr std::size_t kHostHeapThreshold = 8;
constexpr std::size_t kMaximumTitleCodeUnits = 256;
constexpr std::uint32_t kTitleLimitBytes = 512;
constexpr std::size_t kEditLimitOffset = 0x2A0;
constexpr std::size_t kPvpCreateDialogOffset = 0x4C;
constexpr std::size_t kMissionCreateDialogOffset = 0x4C;
constexpr std::size_t kMissionSettingsDialogOffset = 0x5C;
constexpr std::size_t kPvpTitleEditOffset = 0x50;
constexpr std::size_t kTitleBarDialogOffset = 0x48;
constexpr std::size_t kTitleBarCachedTitleOffset = 0x110;
constexpr std::size_t kPvpRoomTitleOffset = 0x7A;
constexpr std::size_t kMissionRoomTitleOffset = 0x54;
constexpr std::size_t kLegacyRoomTitleBytes = 20;
constexpr std::size_t kControlItemsBeginOffset = 0x190;
constexpr std::size_t kControlItemsEndOffset = 0x194;
constexpr std::size_t kMaximumControlTextItems = 64;
constexpr std::size_t kMaximumRenderedCodeUnits = 1024;
constexpr std::size_t kPvpListEntriesBeginOffset = 0x124;
constexpr std::size_t kPvpListEntriesEndOffset = 0x128;
constexpr std::size_t kPvpListControlOffset = 0x94;
constexpr std::size_t kPvpListControlStride = 0x0C;
constexpr std::size_t kPvpListEntryStride = 0x90;
constexpr std::size_t kPvpListTitleOffset = 0x7A;
constexpr std::size_t kPvpListTitleItem = 1;
constexpr std::size_t kMissionListControlOffset = 0x9C;
constexpr std::size_t kMissionListEntryStride = 0x123;
constexpr std::size_t kMissionListRoomIdOffset = 0xC8;
constexpr std::size_t kMissionListTitleOffset = 0x11C;
constexpr std::size_t kMaximumVisibleRoomRows = 64;
constexpr int kEditControlType = 8;
constexpr int kButtonControlType = 4;
constexpr int kStaticTextControlType = 3;

enum class RoomKind : std::uint8_t {
    Pvp,
    Mission
};

struct HostWString24 {
    union {
        wchar_t small[8];
        const wchar_t* pointer;
        std::uint8_t storage[16];
    } value;
    std::uint32_t length;
    std::uint32_t capacity;
};

static_assert(sizeof(wchar_t) == 2, "GOnline native strings use 16-bit wchar_t");
static_assert(sizeof(HostWString24) == 24, "GOnline native string reference layout changed");
static_assert(offsetof(HostWString24, length) == 0x10, "unexpected native string length offset");
static_assert(offsetof(HostWString24, capacity) == 0x14, "unexpected native string capacity offset");

using FindControlFn = void* (__thiscall*)(void*, const HostWString24*, int);
using GetTextFn = const HostWString24* (__thiscall*)(void*);
using SetTextFn = void (__thiscall*)(void*, const HostWString24*, int);
using WideToNarrowFn = const char* (__cdecl*)(UINT, LPCWSTR);
using NarrowToWideFn = const wchar_t* (__cdecl*)(UINT, const char*);
using NativeStringAssignFn =
    HostWString24* (__thiscall*)(HostWString24*, const HostWString24*, std::size_t, std::size_t);
using ButtonTextItemFn = HostWString24* (__thiscall*)(void*, std::uint32_t);
using StaticTextSetFn = void (__thiscall*)(void*, const HostWString24*);
using InitFn = void (__thiscall*)(void*, std::uint32_t);
using TitleRenderFn = void (__thiscall*)(void*, const void*);
using PvpRoomListFn = void (__thiscall*)(void*, const void*);
using MissionRoomRowFn = void (__thiscall*)(void*, int);

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
FindControlFn g_findControl = nullptr;
GetTextFn g_getText = nullptr;
SetTextFn g_setText = nullptr;
WideToNarrowFn g_wideToNarrow = nullptr;
NarrowToWideFn g_narrowToWide = nullptr;
NativeStringAssignFn g_nativeStringAssign = nullptr;
ButtonTextItemFn g_buttonTextItem = nullptr;
StaticTextSetFn g_staticTextSet = nullptr;
const void* g_editVtable = nullptr;
const void* g_buttonVtable = nullptr;
const void* g_staticTextVtable = nullptr;
const void* g_titleBarVtable = nullptr;
InitCallback g_callback = nullptr;
TitleLookupCallback g_titleLookup = nullptr;
InitFn g_pvpCreateInit = nullptr;
InitFn g_missionCreateInit = nullptr;
InitFn g_missionSettingsInit = nullptr;
InitFn g_pvpTitleEditInit = nullptr;
TitleRenderFn g_pvpTitleRender = nullptr;
TitleRenderFn g_missionTitleRender = nullptr;
PvpRoomListFn g_pvpRoomList = nullptr;
MissionRoomRowFn g_missionRoomRow = nullptr;
bool g_bound = false;
bool g_hooksInstalled = false;
bool g_titleHooksInstalled = false;

bool IsReadableProtection(DWORD protection)
{
    if (protection & (PAGE_GUARD | PAGE_NOACCESS))
        return false;
    switch (protection & 0xFFu) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool IsWritableProtection(DWORD protection)
{
    if (protection & (PAGE_GUARD | PAGE_NOACCESS))
        return false;
    switch (protection & 0xFFu) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool IsAccessibleRange(const void* address, std::size_t size, bool writable)
{
    if (!address)
        return false;
    if (!size)
        return true;

    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (size > std::numeric_limits<std::uintptr_t>::max() - begin)
        return false;
    const auto end = begin + size;
    auto cursor = begin;

    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory = {};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT ||
            !(writable ? IsWritableProtection(memory.Protect) : IsReadableProtection(memory.Protect)))
            return false;

        const auto regionBegin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize > std::numeric_limits<std::uintptr_t>::max() - regionBegin)
            return false;
        const auto regionEnd = regionBegin + memory.RegionSize;
        if (regionEnd <= cursor)
            return false;
        cursor = end < regionEnd ? end : regionEnd;
    }
    return true;
}

bool ReadPointer(const void* owner, std::size_t offset, void** value)
{
    if (!owner || !value)
        return false;
    const auto address = reinterpret_cast<const std::uint8_t*>(owner) + offset;
    if (!IsAccessibleRange(address, sizeof(void*), false))
        return false;
    std::memcpy(value, address, sizeof(void*));
    return *value != nullptr;
}

bool IsEdit(const void* edit)
{
    if (!edit || !g_editVtable || !IsAccessibleRange(edit, sizeof(void*), false))
        return false;
    const void* vtable = nullptr;
    std::memcpy(&vtable, edit, sizeof(vtable));
    return vtable == g_editVtable;
}

bool IsObjectWithVtable(const void* object, const void* expected)
{
    if (!object || !expected || !IsAccessibleRange(object, sizeof(void*), false))
        return false;
    const void* vtable = nullptr;
    std::memcpy(&vtable, object, sizeof(vtable));
    return vtable == expected;
}

HostWString24 MakeStringView(const wchar_t* text, std::size_t length)
{
    HostWString24 view = {};
    view.length = static_cast<std::uint32_t>(length);
    if (length < kHostHeapThreshold) {
        if (length)
            std::memcpy(view.value.small, text, length * sizeof(wchar_t));
        view.value.small[length] = L'\0';
        view.capacity = static_cast<std::uint32_t>(kHostSmallCapacity);
    } else {
        view.value.pointer = text;
        view.capacity = static_cast<std::uint32_t>(length);
    }
    return view;
}

const wchar_t* StringData(const HostWString24& value)
{
    return value.capacity < kHostHeapThreshold ? value.value.small : value.value.pointer;
}

bool CopyNativeString(const HostWString24* native,
                      std::size_t maximumCodeUnits,
                      std::wstring* copy)
{
    if (!native || !copy || !IsAccessibleRange(native, sizeof(*native), false))
        return false;

    HostWString24 value = {};
    std::memcpy(&value, native, sizeof(value));
    if (value.length > maximumCodeUnits || value.capacity < value.length)
        return false;
    if (value.capacity < kHostHeapThreshold && value.length > kHostSmallCapacity)
        return false;

    const wchar_t* data = StringData(value);
    const std::size_t bytes = (static_cast<std::size_t>(value.length) + 1) * sizeof(wchar_t);
    if (!data || !IsAccessibleRange(data, bytes, false) || data[value.length] != L'\0')
        return false;

    try {
        std::wstring temporary(data, value.length);
        copy->swap(temporary);
    } catch (...) {
        return false;
    }
    return true;
}

bool AssignNativeString(HostWString24* destination, const std::wstring& text)
{
    if (!g_nativeStringAssign || !destination ||
        text.size() > kMaximumRenderedCodeUnits ||
        text.find(L'\0') != std::wstring::npos ||
        !IsAccessibleRange(destination, sizeof(*destination), true))
        return false;

    std::wstring current;
    if (!CopyNativeString(destination, kMaximumRenderedCodeUnits, &current))
        return false;
    if (current == text)
        return true;

    const HostWString24 view = MakeStringView(text.c_str(), text.size());
    g_nativeStringAssign(destination, &view, 0, static_cast<std::size_t>(-1));
    return true;
}

bool ReadLegacyRoomTitle(const void* room, RoomKind kind, std::wstring* title)
{
    if (!room || !title || !g_narrowToWide)
        return false;
    const std::size_t offset =
        kind == RoomKind::Pvp ? kPvpRoomTitleOffset : kMissionRoomTitleOffset;
    const auto* source = static_cast<const std::uint8_t*>(room) + offset;
    if (!IsAccessibleRange(source, kLegacyRoomTitleBytes, false))
        return false;

    char terminated[kLegacyRoomTitleBytes + 1] = {};
    std::memcpy(terminated, source, kLegacyRoomTitleBytes);
    const wchar_t* converted = g_narrowToWide(874, terminated);
    if (!converted ||
        !IsAccessibleRange(converted, (kLegacyRoomTitleBytes + 1) * sizeof(wchar_t), false))
        return false;

    std::size_t length = 0;
    while (length <= kLegacyRoomTitleBytes && converted[length] != L'\0')
        ++length;
    if (length > kLegacyRoomTitleBytes)
        return false;

    try {
        std::wstring temporary(converted, length);
        title->swap(temporary);
    } catch (...) {
        return false;
    }
    return !title->empty();
}

bool TextItemCount(void* control, std::size_t* count)
{
    if (!count || !control || !IsAccessibleRange(control, sizeof(void*), false))
        return false;
    const void* vtable = nullptr;
    std::memcpy(&vtable, control, sizeof(vtable));
    if (!vtable ||
        !IsAccessibleRange(static_cast<const std::uint8_t*>(vtable) + 0x70,
                           sizeof(void*), false))
        return false;
    ButtonTextItemFn getter = nullptr;
    std::memcpy(&getter, static_cast<const std::uint8_t*>(vtable) + 0x70, sizeof(getter));
    if (getter != g_buttonTextItem)
        return false;
    const auto* bytes = static_cast<const std::uint8_t*>(control);
    const auto* beginAddress = bytes + kControlItemsBeginOffset;
    const auto* endAddress = bytes + kControlItemsEndOffset;
    if (!IsAccessibleRange(beginAddress, sizeof(void*), false) ||
        !IsAccessibleRange(endAddress, sizeof(void*), false))
        return false;

    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::memcpy(&begin, beginAddress, sizeof(begin));
    std::memcpy(&end, endAddress, sizeof(end));
    if (!begin || !end || end < begin ||
        (end - begin) % sizeof(HostWString24*) != 0)
        return false;
    const std::size_t items =
        static_cast<std::size_t>((end - begin) / sizeof(HostWString24*));
    if (items > kMaximumControlTextItems ||
        (items && !IsAccessibleRange(reinterpret_cast<const void*>(begin),
                                     items * sizeof(HostWString24*), false)))
        return false;
    *count = items;
    return true;
}

bool LookupFullTitle(RoomKind kind, const void* roomIdAddress, std::wstring* fullTitle)
{
    if (!g_titleLookup || !roomIdAddress || !fullTitle ||
        !IsAccessibleRange(roomIdAddress, sizeof(std::uint32_t), false))
        return false;
    std::uint32_t roomId = 0;
    std::memcpy(&roomId, roomIdAddress, sizeof(roomId));
    if (!roomId || !g_titleLookup(kind == RoomKind::Pvp, roomId, fullTitle) ||
        fullTitle->empty() || fullTitle->size() > kMaximumTitleCodeUnits ||
        fullTitle->find(L'\0') != std::wstring::npos)
        return false;
    return true;
}

void ProjectTitleBar(void* titleBar, const void* room, RoomKind kind)
{
    if (!room || !IsObjectWithVtable(titleBar, g_titleBarVtable))
        return;

    std::wstring fullTitle;
    if (!LookupFullTitle(kind, room, &fullTitle))
        return;

    std::wstring legacyTitle;
    if (!ReadLegacyRoomTitle(room, kind, &legacyTitle))
        return;

    SetTitleBarCachedTitle(titleBar, fullTitle);
    if (fullTitle == legacyTitle)
        return;

    void* dialog = nullptr;
    if (!ReadPointer(titleBar, kTitleBarDialogOffset, &dialog))
        return;
    constexpr wchar_t kTitleButtonControl[] = L"TitleChangeBtn";
    const HostWString24 name =
        MakeStringView(kTitleButtonControl, std::size(kTitleButtonControl) - 1);
    void* control = g_findControl(dialog, &name, kButtonControlType);

    std::size_t count = 0;
    if (!TextItemCount(control, &count))
        return;
    for (std::size_t index = 0; index < count; ++index) {
        HostWString24* item = g_buttonTextItem(control, static_cast<std::uint32_t>(index));
        std::wstring rendered;
        if (!CopyNativeString(item, kMaximumRenderedCodeUnits, &rendered))
            continue;
        const std::size_t position = rendered.rfind(legacyTitle);
        if (position == std::wstring::npos)
            continue;
        rendered.replace(position, legacyTitle.size(), fullTitle);
        AssignNativeString(item, rendered);
    }
}

void ProjectPvpRoomList(void* service)
{
    if (!service)
        return;
    const auto* bytes = static_cast<const std::uint8_t*>(service);
    const auto* beginAddress = bytes + kPvpListEntriesBeginOffset;
    const auto* endAddress = bytes + kPvpListEntriesEndOffset;
    if (!IsAccessibleRange(beginAddress, sizeof(void*), false) ||
        !IsAccessibleRange(endAddress, sizeof(void*), false))
        return;

    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::memcpy(&begin, beginAddress, sizeof(begin));
    std::memcpy(&end, endAddress, sizeof(end));
    if (!begin || end < begin || (end - begin) % kPvpListEntryStride != 0)
        return;
    const std::size_t count =
        static_cast<std::size_t>((end - begin) / kPvpListEntryStride);
    if (count > kMaximumVisibleRoomRows ||
        (count && !IsAccessibleRange(reinterpret_cast<const void*>(begin),
                                     count * kPvpListEntryStride, false)))
        return;

    for (std::size_t index = 0; index < count; ++index) {
        const auto* entry = reinterpret_cast<const std::uint8_t*>(
            begin + index * kPvpListEntryStride);
        std::wstring fullTitle;
        if (!LookupFullTitle(RoomKind::Pvp, entry, &fullTitle))
            continue;

        void* control = nullptr;
        const auto* controlAddress =
            bytes + kPvpListControlOffset + index * kPvpListControlStride;
        if (!IsAccessibleRange(controlAddress, sizeof(control), false))
            continue;
        std::memcpy(&control, controlAddress, sizeof(control));

        std::size_t itemCount = 0;
        if (!TextItemCount(control, &itemCount) ||
            kPvpListTitleItem >= itemCount)
            continue;
        HostWString24* item =
            g_buttonTextItem(control, static_cast<std::uint32_t>(kPvpListTitleItem));
        AssignNativeString(item, fullTitle);
    }
}

void ProjectMissionRoomRow(void* service, int index)
{
    if (!service || index < 0 ||
        static_cast<std::size_t>(index) >= kMaximumVisibleRoomRows)
        return;
    auto* entry = static_cast<std::uint8_t*>(service) +
                  static_cast<std::size_t>(index) * kMissionListEntryStride;
    std::wstring fullTitle;
    if (!LookupFullTitle(RoomKind::Mission,
                         entry + kMissionListRoomIdOffset, &fullTitle))
        return;

    void* row = nullptr;
    const auto* rowAddress = static_cast<const std::uint8_t*>(service) +
                             kMissionListControlOffset +
                             static_cast<std::size_t>(index) * sizeof(void*);
    if (!IsAccessibleRange(rowAddress, sizeof(row), false))
        return;
    std::memcpy(&row, rowAddress, sizeof(row));
    if (!row)
        return;

    constexpr wchar_t kRoomNameStatic[] = L"RoomNameStatic";
    const HostWString24 name =
        MakeStringView(kRoomNameStatic, std::size(kRoomNameStatic) - 1);
    void* control = g_findControl(row, &name, kStaticTextControlType);
    if (!IsObjectWithVtable(control, g_staticTextVtable))
        return;
    const HostWString24 view = MakeStringView(fullTitle.c_str(), fullTitle.size());
    g_staticTextSet(control, &view);
}

const char* SurfaceName(Surface surface)
{
    switch (surface) {
    case Surface::PvpCreate: return "PvP create";
    case Surface::MissionCreate: return "mission create";
    case Surface::MissionSettings: return "mission settings";
    case Surface::PvpTitleEdit: return "PvP title edit";
    default: return "unknown";
    }
}

void CompleteInit(Surface surface, void* service)
{
    void* edit = FindRoomNameEdit(service, surface);
    if (edit && !SetRoomNameLimit(edit)) {
        if (g_api && g_api->Log)
            g_api->Log(g_mod, SDGO_LOG_WARN, "%s room-title edit did not match the verified native layout",
                       SurfaceName(surface));
        edit = nullptr;
    }
    if (g_callback)
        g_callback(surface, service, edit);
}

void __fastcall PvpCreateInit(void* self, void*, std::uint32_t argument)
{
    g_pvpCreateInit(self, argument);
    CompleteInit(Surface::PvpCreate, self);
}

void __fastcall MissionCreateInit(void* self, void*, std::uint32_t argument)
{
    g_missionCreateInit(self, argument);
    CompleteInit(Surface::MissionCreate, self);
}

void __fastcall MissionSettingsInit(void* self, void*, std::uint32_t argument)
{
    g_missionSettingsInit(self, argument);
    CompleteInit(Surface::MissionSettings, self);
}

void __fastcall PvpTitleEditInit(void* self, void*, std::uint32_t argument)
{
    g_pvpTitleEditInit(self, argument);
    CompleteInit(Surface::PvpTitleEdit, self);
}

void __fastcall PvpTitleRender(void* self, void*, const void* room)
{
    g_pvpTitleRender(self, room);
    try {
        ProjectTitleBar(self, room, RoomKind::Pvp);
    } catch (...) {
    }
}

void __fastcall MissionTitleRender(void* self, void*, const void* room)
{
    g_missionTitleRender(self, room);
    try {
        ProjectTitleBar(self, room, RoomKind::Mission);
    } catch (...) {
    }
}

void __fastcall PvpRoomList(void* self, void*, const void* packet)
{
    g_pvpRoomList(self, packet);
    try {
        ProjectPvpRoomList(self);
    } catch (...) {
    }
}

void __fastcall MissionRoomRow(void* self, void*, int index)
{
    g_missionRoomRow(self, index);
    try {
        ProjectMissionRoomRow(self, index);
    } catch (...) {
    }
}

}  // namespace

bool Bind(const SdgoApi* api, SdgoMod* mod)
{
    if (!api || !mod || !api->Symbol || !api->HookSlot)
        return false;
    if (g_bound)
        return api == g_api && mod == g_mod;

    const auto findControl = reinterpret_cast<FindControlFn>(api->Symbol(mod, "room_titles.ui_find_control"));
    const auto getText = reinterpret_cast<GetTextFn>(api->Symbol(mod, "room_titles.ui_edit_get_text"));
    const auto setText = reinterpret_cast<SetTextFn>(api->Symbol(mod, "room_titles.ui_edit_set_text"));
    const auto wideToNarrow =
        reinterpret_cast<WideToNarrowFn>(api->Symbol(mod, "room_titles.ui_wide_to_narrow"));
    const auto narrowToWide =
        reinterpret_cast<NarrowToWideFn>(api->Symbol(mod, "room_titles.ui_legacy_to_wide"));
    const auto nativeStringAssign =
        reinterpret_cast<NativeStringAssignFn>(api->Symbol(mod, "room_titles.ui_native_string_assign"));
    const auto buttonTextItem =
        reinterpret_cast<ButtonTextItemFn>(api->Symbol(mod, "room_titles.ui_button_text_item"));
    const auto staticTextSet =
        reinterpret_cast<StaticTextSetFn>(api->Symbol(mod, "room_titles.ui_static_text_set"));
    const void* editVtable = api->Symbol(mod, "room_titles.ui_edit_vtable");
    const void* buttonVtable = api->Symbol(mod, "room_titles.ui_button_vtable");
    const void* staticTextVtable = api->Symbol(mod, "room_titles.ui_static_text_vtable");
    const void* titleBarVtable = api->Symbol(mod, "room_titles.ui_titlebar_vtable");
    if (!findControl || !getText || !setText || !wideToNarrow || !narrowToWide ||
        !nativeStringAssign || !buttonTextItem || !staticTextSet || !editVtable ||
        !buttonVtable || !staticTextVtable || !titleBarVtable)
        return false;

    g_api = api;
    g_mod = mod;
    g_findControl = findControl;
    g_getText = getText;
    g_setText = setText;
    g_wideToNarrow = wideToNarrow;
    g_narrowToWide = narrowToWide;
    g_nativeStringAssign = nativeStringAssign;
    g_buttonTextItem = buttonTextItem;
    g_staticTextSet = staticTextSet;
    g_editVtable = editVtable;
    g_buttonVtable = buttonVtable;
    g_staticTextVtable = staticTextVtable;
    g_titleBarVtable = titleBarVtable;
    g_bound = true;
    return true;
}

bool InstallInitHooks(InitCallback callback)
{
    if (!g_bound || g_hooksInstalled)
        return false;
    g_callback = callback;

    if (!g_api->HookSlot(g_mod, "room_titles.ui_pvp_create_init",
                         reinterpret_cast<void*>(&PvpCreateInit),
                         reinterpret_cast<void**>(&g_pvpCreateInit)) ||
        !g_api->HookSlot(g_mod, "room_titles.ui_mission_create_init",
                         reinterpret_cast<void*>(&MissionCreateInit),
                         reinterpret_cast<void**>(&g_missionCreateInit)) ||
        !g_api->HookSlot(g_mod, "room_titles.ui_mission_settings_init",
                         reinterpret_cast<void*>(&MissionSettingsInit),
                         reinterpret_cast<void**>(&g_missionSettingsInit)) ||
        !g_api->HookSlot(g_mod, "room_titles.ui_pvp_title_edit_init",
                         reinterpret_cast<void*>(&PvpTitleEditInit),
                         reinterpret_cast<void**>(&g_pvpTitleEditInit)))
        return false;

    g_hooksInstalled = true;
    return true;
}

bool InstallTitleDisplayHooks(TitleLookupCallback callback)
{
    if (!g_bound || !callback || g_titleHooksInstalled || !g_api->HookCode)
        return false;
    g_titleLookup = callback;
    if (!g_api->HookCode(g_mod, "room_titles.ui_pvp_titlebar_render",
                         reinterpret_cast<void*>(&PvpTitleRender),
                         reinterpret_cast<void**>(&g_pvpTitleRender)) ||
        !g_api->HookCode(g_mod, "room_titles.ui_mission_titlebar_render",
                         reinterpret_cast<void*>(&MissionTitleRender),
                         reinterpret_cast<void**>(&g_missionTitleRender)) ||
        !g_api->HookCode(g_mod, "room_titles.ui_pvp_room_list",
                         reinterpret_cast<void*>(&PvpRoomList),
                         reinterpret_cast<void**>(&g_pvpRoomList)) ||
        !g_api->HookCode(g_mod, "room_titles.ui_mission_room_row",
                         reinterpret_cast<void*>(&MissionRoomRow),
                         reinterpret_cast<void**>(&g_missionRoomRow)))
        return false;
    g_titleHooksInstalled = true;
    return true;
}

void* FindRoomNameEdit(void* service, Surface surface)
{
    if (!g_bound || !service)
        return nullptr;

    if (surface == Surface::PvpTitleEdit) {
        void* edit = nullptr;
        return ReadPointer(service, kPvpTitleEditOffset, &edit) && IsEdit(edit) ? edit : nullptr;
    }

    std::size_t dialogOffset = 0;
    switch (surface) {
    case Surface::PvpCreate: dialogOffset = kPvpCreateDialogOffset; break;
    case Surface::MissionCreate: dialogOffset = kMissionCreateDialogOffset; break;
    case Surface::MissionSettings: dialogOffset = kMissionSettingsDialogOffset; break;
    default: return nullptr;
    }

    void* dialog = nullptr;
    if (!ReadPointer(service, dialogOffset, &dialog))
        return nullptr;
    constexpr wchar_t kRoomNameControl[] = L"GameRoomNameEdit";
    const HostWString24 name = MakeStringView(kRoomNameControl, std::size(kRoomNameControl) - 1);
    void* edit = g_findControl(dialog, &name, kEditControlType);
    return IsEdit(edit) ? edit : nullptr;
}

bool SetRoomNameLimit(void* edit)
{
    if (!g_bound || !IsEdit(edit))
        return false;
    auto* limit = reinterpret_cast<std::uint32_t*>(
        reinterpret_cast<std::uint8_t*>(edit) + kEditLimitOffset);
    if (!IsAccessibleRange(limit, sizeof(*limit), true))
        return false;
    *limit = kTitleLimitBytes;
    return true;
}

bool ReadEditText(void* edit, std::wstring* copy)
{
    if (!g_bound || !copy || !IsEdit(edit))
        return false;
    const HostWString24* native = g_getText(edit);
    return ReadNativeString(native, copy);
}

bool ReadNativeString(const void* native_string, std::wstring* copy)
{
    const auto* native = static_cast<const HostWString24*>(native_string);
    return CopyNativeString(native, kMaximumTitleCodeUnits, copy);
}

bool SetEditText(void* edit, const std::wstring& text, bool selection_flag)
{
    if (!g_bound || !IsEdit(edit) || text.size() > kMaximumTitleCodeUnits ||
        text.find(L'\0') != std::wstring::npos)
        return false;
    const HostWString24 view = MakeStringView(text.c_str(), text.size());
    g_setText(edit, &view, selection_flag ? 1 : 0);
    return true;
}

bool SetTitleBarCachedTitle(void* title_bar, const std::wstring& text)
{
    if (!g_bound || !IsObjectWithVtable(title_bar, g_titleBarVtable) ||
        text.empty() || text.size() > kMaximumTitleCodeUnits ||
        text.find(L'\0') != std::wstring::npos)
        return false;
    auto* cached = reinterpret_cast<HostWString24*>(
        static_cast<std::uint8_t*>(title_bar) + kTitleBarCachedTitleOffset);
    return AssignNativeString(cached, text);
}

bool LegacyTitleBytes(const std::wstring& text, std::string* copy)
{
    if (!g_bound || !copy || text.size() > kMaximumTitleCodeUnits ||
        text.find(L'\0') != std::wstring::npos)
        return false;

    constexpr UINT kHostLegacyCodePage = 874;
    constexpr std::size_t kHostConversionBufferBytes = 0x400;
    const char* converted = g_wideToNarrow(kHostLegacyCodePage, text.c_str());
    if (!converted || !IsAccessibleRange(converted, kHostConversionBufferBytes, false))
        return false;

    std::size_t length = 0;
    while (length < kHostConversionBufferBytes && converted[length] != '\0')
        ++length;
    if (length == kHostConversionBufferBytes)
        return false;

    try {
        std::string temporary(converted, length);
        copy->swap(temporary);
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace native_ui
}  // namespace room_titles
}  // namespace sdgo
