#ifndef SDGO_ROOM_TITLES_NATIVE_UI_H
#define SDGO_ROOM_TITLES_NATIVE_UI_H

#include "sdgo/sdgo_api.h"

#include <cstdint>
#include <string>

namespace sdgo {
namespace room_titles {
namespace native_ui {

// Each surface owns a room-title edit box but stores the dialog/edit pointer at
// a different native offset. The values are stable API identifiers, not native
// game mode numbers.
enum class Surface : std::uint8_t {
    PvpCreate,
    MissionCreate,
    MissionSettings,
    PvpTitleEdit
};

// Called after the native init returns and after the edit limit is expanded.
// edit is null when the verified surface did not expose its expected control.
using InitCallback = void (*)(Surface surface, void* service, void* edit);

// Resolve a confirmed full title for a native room id. The callback must copy
// into full_title and return true only when that room has an extension record.
using TitleLookupCallback = bool (*)(bool battle_domain,
                                     std::uint32_t native_room_id,
                                     std::wstring* full_title);

// Resolve the verified host helpers used below. This does not install hooks.
bool Bind(const SdgoApi* api, SdgoMod* mod);

// Hook all four verified init vtable slots. A false result must make the
// containing Mod decline to load so the core rolls back any earlier slot swap.
bool InstallInitHooks(InitCallback callback = nullptr);

// Hook the verified PvP and mission title-bar and lobby-list renderers. The
// native renderer runs first so existing layout, state and localized wrappers
// stay intact; confirmed full titles are then projected into native UI strings.
bool InstallTitleDisplayHooks(TitleLookupCallback callback);

// Find the native room-name edit owned by a service after its init completed.
void* FindRoomNameEdit(void* service, Surface surface);

// The host limit is measured in UTF-16 bytes. This sets only a verified room
// title edit to 512 bytes (256 UTF-16 code units).
bool SetRoomNameLimit(void* edit);

// Copy the edit's current native UTF-16 value into Mod-owned storage.
bool ReadEditText(void* edit, std::wstring* copy);

// Copy any read-only HostWString24 reference (for example event+0x2C) into
// Mod-owned storage, with the same layout and bounds checks as ReadEditText.
bool ReadNativeString(const void* native_string, std::wstring* copy);

// Copy Mod-owned text into the native edit through the host's const-reference
// setter. No native string allocation is retained or destroyed by the Mod.
bool SetEditText(void* edit, const std::wstring& text, bool selection_flag = false);

// Deep-copy text into the title bar's native owned cache through the host
// allocator. This keeps title-edit cancel/reopen behavior on the full title
// without changing either room structure's fixed legacy field.
bool SetTitleBarCachedTitle(void* title_bar, const std::wstring& text);

// Use the host's code-page-874 conversion helper and immediately copy its
// rotating-buffer result. This preserves any client-side IAT language mapping.
bool LegacyTitleBytes(const std::wstring& text, std::string* copy);

}  // namespace native_ui
}  // namespace room_titles
}  // namespace sdgo

#endif
