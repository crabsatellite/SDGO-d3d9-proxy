#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "sdgo/sdgo_api.h"
#include "sdgo/shop_labels.h"

namespace {
using sdgo::shop::Option;
using PopulateFn = void(__thiscall*)(uint8_t*);
using TooltipFn = void(__thiscall*)(uint8_t*, const Option*, int);
using ConvertFn = const wchar_t*(__cdecl*)(UINT, const char*);
using GridDataFn = void*(__cdecl*)();
using GridFindFn = int(__thiscall*)(void*, void**, int, int, char);

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
PopulateFn g_buy = nullptr;
PopulateFn g_gift = nullptr;
TooltipFn g_tooltip = nullptr;
ConvertFn g_convert = nullptr;
GridDataFn g_gridData = nullptr;
GridFindFn g_gridFind = nullptr;
bool g_showRental = true;
bool g_showPermanent = true;

struct View { uintptr_t begin = 0; uintptr_t end = 0; };
thread_local View g_view;
// The original converter returns a borrowed ring-buffer pointer. Its caller
// immediately copies into a wstring; it never frees that pointer.
thread_local std::array<std::wstring, 16> g_labels;
thread_local size_t g_labelIndex = 0;

struct Override {
    uint32_t category = 0;
    int validity = 0;
    int payment = 0;
    int price = 0;
    std::wstring text;
};
std::map<uint32_t, Override> g_overrides;

View ReadView(uint8_t* self, size_t offset)
{
    View result;
    __try {
        result.begin = *reinterpret_cast<uintptr_t*>(self + offset);
        result.end = *reinterpret_cast<uintptr_t*>(self + offset + 4);
        if (!result.begin || result.end < result.begin ||
            result.end - result.begin > 128 * sizeof(Option) ||
            (result.end - result.begin) % sizeof(Option) != 0)
            result = {};
    } __except (EXCEPTION_EXECUTE_HANDLER) { result = {}; }
    return result;
}

bool ReadOption(const char* text, Option* out)
{
    const auto address = reinterpret_cast<uintptr_t>(text);
    if (!g_view.begin || address < g_view.begin + offsetof(Option, text) || address >= g_view.end)
        return false;
    const uintptr_t offset = address - g_view.begin - offsetof(Option, text);
    if (offset % sizeof(Option) != 0 || offset + sizeof(Option) > g_view.end - g_view.begin)
        return false;
    __try {
        memcpy(out, reinterpret_cast<const void*>(g_view.begin + offset), sizeof(*out));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

int TimeLimit(uint32_t category)
{
    // Reuse the core's already verified native table accessor and lookup.
    __try {
        void* record = nullptr;
        if (g_gridFind(g_gridData(), &record, static_cast<int>(category), 0x22, 0) == 1 && record &&
            *reinterpret_cast<uint32_t*>(record) == category)
            return static_cast<uint8_t*>(record)[0x6D];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return -1;
}

const wchar_t* Borrow(std::wstring text)
{
    std::wstring& slot = g_labels[g_labelIndex++ % g_labels.size()];
    slot = std::move(text);
    return slot.c_str();
}

const wchar_t* __cdecl Convert(UINT codePage, const char* text)
{
    Option option = {};
    if (codePage == 874 && ReadOption(text, &option)) {
        // A host caption must never hide the actual rental/permanent status.
        const int mode = TimeLimit(option.category);
        if ((mode == 1 && g_showRental) || (mode == 0 && g_showPermanent)) {
            std::wstring label = sdgo::shop::DurationLabel(option, mode);
            if (!label.empty())
                return Borrow(std::move(label));
        }
        const auto found = g_overrides.find(option.id);
        if (found != g_overrides.end()) {
            const Override& value = found->second;
            if (value.category == option.category && value.validity == option.validity &&
                value.payment == option.payment && value.price == option.price)
                return Borrow(value.text);
        }
    }
    return g_convert(codePage, text);
}

void Populate(uint8_t* self, size_t offset, PopulateFn original)
{
    const View before = g_view;
    g_view = ReadView(self, offset);
    __try { original(self); }
    __finally { g_view = before; }
}

void __fastcall Buy(uint8_t* self, void*) { Populate(self, 0xB0, g_buy); }
void __fastcall Gift(uint8_t* self, void*) { Populate(self, 0xB4, g_gift); }
void __fastcall Tooltip(uint8_t* self, void*, const Option* option, int row)
{
    const View before = g_view;
    g_view.begin = reinterpret_cast<uintptr_t>(option);
    g_view.end = option ? g_view.begin + sizeof(Option) : 0;
    __try { g_tooltip(self, option, row); }
    __finally { g_view = before; }
}

bool LoadOverrides(const wchar_t* directory)
{
    const std::wstring file = std::wstring(directory) + L"\\display.ini";
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES)
        return true;
    std::vector<wchar_t> sections(65536);
    const DWORD count = GetPrivateProfileSectionNamesW(sections.data(), static_cast<DWORD>(sections.size()), file.c_str());
    if (count >= sections.size() - 2)
        return false;
    for (const wchar_t* section = sections.data(); *section; section += wcslen(section) + 1) {
        if (wcsncmp(section, L"option:", 7) != 0)
            continue;
        wchar_t* end = nullptr;
        const unsigned long id = wcstoul(section + 7, &end, 10);
        if (!id || !end || *end || id > 0x7FFFFFFF || g_overrides.count(id))
            return false;
        Override value;
        value.category = GetPrivateProfileIntW(section, L"Category", 0, file.c_str());
        value.validity = static_cast<int>(GetPrivateProfileIntW(section, L"Validity", 0, file.c_str()));
        value.payment = static_cast<int>(GetPrivateProfileIntW(section, L"Payment", 0, file.c_str()));
        value.price = static_cast<int>(GetPrivateProfileIntW(section, L"Price", -1, file.c_str()));
        wchar_t text[256] = {};
        const DWORD length = GetPrivateProfileStringW(section, L"Text", L"", text, 256, file.c_str());
        if (!value.category || value.validity <= 0 || value.price < 0 ||
            (value.payment != 1 && value.payment != 2) || !length || length >= 255)
            return false;
        value.text = text;
        g_overrides.emplace(id, std::move(value));
    }
    return true;
}
} // namespace

SDGO_MOD_ENTRY
{
    if (!api || api->version != SDGO_API_VERSION || !api->Symbol || !api->HookCode || !api->ModDirectory)
        return 0;
    g_api = api;
    g_mod = mod;
    g_showRental = api->SettingInt(mod, "ShowRental", 1) != 0;
    g_showPermanent = api->SettingInt(mod, "ShowPermanent", 1) != 0;
    g_gridData = reinterpret_cast<GridDataFn>(api->Symbol(mod, "shop_labels.grid_data"));
    g_gridFind = reinterpret_cast<GridFindFn>(api->Symbol(mod, "shop_labels.grid_find"));
    if (!g_gridData || !g_gridFind || (api->SettingInt(mod, "UseUnicodeOverrides", 1) && !LoadOverrides(api->ModDirectory(mod)))) {
        api->Log(mod, SDGO_LOG_ERROR, "invalid shop display configuration");
        return 0;
    }
    if (!api->HookCode(mod, "shop.buy_options.populate", reinterpret_cast<void*>(&Buy), reinterpret_cast<void**>(&g_buy)) ||
        !api->HookCode(mod, "shop.gift_options.populate", reinterpret_cast<void*>(&Gift), reinterpret_cast<void**>(&g_gift)) ||
        !api->HookCode(mod, "shop.tooltip.option", reinterpret_cast<void*>(&Tooltip), reinterpret_cast<void**>(&g_tooltip)) ||
        !api->HookCode(mod, "text.ansi_to_wide", reinterpret_cast<void*>(&Convert), reinterpret_cast<void**>(&g_convert)))
        return 0;
    api->Log(mod, SDGO_LOG_INFO, "rental/permanent labels enabled; %u Unicode option overrides; prices and currency untouched",
             static_cast<unsigned>(g_overrides.size()));
    return 1;
}
