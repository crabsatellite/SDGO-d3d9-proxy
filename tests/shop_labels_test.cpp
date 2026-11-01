#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "sdgo/sdgo_api.h"
#include "sdgo/shop_labels.h"

namespace {
using sdgo::shop::Option;
using PopulateHook = void(__fastcall*)(uint8_t*, void*);
using TooltipHook = void(__fastcall*)(uint8_t*, void*, const Option*, int);
using ConvertHook = const wchar_t*(__cdecl*)(UINT, const char*);
PopulateHook g_buy = nullptr, g_gift = nullptr;
TooltipHook g_tooltip = nullptr;
ConvertHook g_convert = nullptr;
std::wstring g_dir;
std::vector<std::wstring> g_result;
unsigned g_nativeCalls = 0;
bool g_inGift = false;
std::array<uint8_t, 8> g_manager = {};
uint8_t* g_managerPtr = g_manager.data();
std::array<uint8_t, 614> g_rentalRecord = {}, g_permanentRecord = {};

bool Check(bool value, const char* text)
{
    if (!value) fprintf(stderr, "FAIL: %s\n", text);
    return value;
}
void BuildMap()
{
    *reinterpret_cast<uint32_t*>(g_rentalRecord.data()) = 1110549;
    g_rentalRecord[0x6D] = 1;
    *reinterpret_cast<uint32_t*>(g_permanentRecord.data()) = 1120549;
    g_permanentRecord[0x6D] = 0;
}
void* __cdecl NativeGridData() { return g_managerPtr; }
int __fastcall NativeGridFind(void*, void*, void** out, int id, int type, char)
{
    if (type != 0x22) return 0;
    if (id == 1110549) *out = g_rentalRecord.data();
    else if (id == 1120549) *out = g_permanentRecord.data();
    else return 0;
    return 1;
}
const wchar_t* __cdecl NativeConvert(UINT, const char*) { ++g_nativeCalls; return L"native"; }
void __fastcall NativePopulate(uint8_t* self, void*)
{
    const size_t offset = g_inGift ? 0xB4 : 0xB0;
    auto* begin = *reinterpret_cast<Option**>(self + offset);
    auto* end = *reinterpret_cast<Option**>(self + offset + 4);
    for (auto* p = begin; p != end; ++p)
        g_result.emplace_back(g_convert(874, p->text));
}
void __fastcall NativeTooltip(uint8_t*, void*, const Option* option, int)
{
    g_result.emplace_back(g_convert(874, option->text));
}
void __cdecl Log(SdgoMod*, int, const char*, ...) {}
void* Symbol(SdgoMod*, const char* name)
{
    if (strcmp(name, "shop_labels.grid_data") == 0) return reinterpret_cast<void*>(&NativeGridData);
    if (strcmp(name, "shop_labels.grid_find") == 0) return reinterpret_cast<void*>(&NativeGridFind);
    return nullptr;
}
int HookCode(SdgoMod*, const char* name, void* hook, void** original)
{
    if (strcmp(name, "text.ansi_to_wide") == 0) {
        g_convert = reinterpret_cast<ConvertHook>(hook); *original = reinterpret_cast<void*>(&NativeConvert);
    } else if (strcmp(name, "shop.buy_options.populate") == 0) {
        g_buy = reinterpret_cast<PopulateHook>(hook); *original = reinterpret_cast<void*>(&NativePopulate);
    } else if (strcmp(name, "shop.gift_options.populate") == 0) {
        g_gift = reinterpret_cast<PopulateHook>(hook); *original = reinterpret_cast<void*>(&NativePopulate);
    } else if (strcmp(name, "shop.tooltip.option") == 0) {
        g_tooltip = reinterpret_cast<TooltipHook>(hook); *original = reinterpret_cast<void*>(&NativeTooltip);
    } else return 0;
    return 1;
}
int SettingInt(SdgoMod*, const char*, int fallback) { return fallback; }
const wchar_t* ModDirectory(SdgoMod*) { return g_dir.c_str(); }
Option MakeOption(uint32_t id, uint32_t category, int days, int price, uint8_t payment)
{
    Option option = {}; option.id = id; option.category = category; option.validity = days;
    option.price = price; option.payment = payment; strcpy_s(option.text, "7???");
    return option;
}
void Populate(std::vector<Option>& options, bool gift)
{
    std::array<uint8_t, 0x100> self = {};
    const size_t offset = gift ? 0xB4 : 0xB0;
    *reinterpret_cast<Option**>(self.data() + offset) = options.data();
    *reinterpret_cast<Option**>(self.data() + offset + 4) = options.data() + options.size();
    g_inGift = gift;
    (gift ? g_gift : g_buy)(self.data(), nullptr);
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) return 2;
    g_dir = argv[2];
    CreateDirectoryW(g_dir.c_str(), nullptr);
    const std::wstring ini = L"\xFEFF[option:3806]\r\nCategory=2220001\r\nValidity=7\r\nPayment=2\r\nPrice=9900\r\nText=中文数量标签\r\n[option:1]\r\nCategory=1110549\r\nValidity=7\r\nPayment=2\r\nPrice=9900\r\nText=永久\r\n";
    std::ofstream file(g_dir + L"\\display.ini", std::ios::binary);
    file.write(reinterpret_cast<const char*>(ini.data()), static_cast<std::streamsize>(ini.size() * sizeof(wchar_t)));
    file.close();
    HMODULE module = LoadLibraryW(argv[1]);
    if (!module) return 3;
    auto load = reinterpret_cast<SdgoModLoadFn>(GetProcAddress(module, "SdgoModLoad"));
    BuildMap();
    SdgoApi api = {}; api.size = sizeof(api); api.version = 1;
    api.Log = Log; api.Symbol = Symbol; api.HookCode = HookCode;
    api.SettingInt = SettingInt; api.ModDirectory = ModDirectory;
    if (!load || !load(&api, nullptr)) return 4;
    std::vector<Option> options{MakeOption(1, 1110549, 7, 9900, 2),
        MakeOption(2, 1120549, 1, 1200, 1), MakeOption(3806, 2220001, 7, 9900, 2),
        MakeOption(3, 1999999, 7, 2000, 1), MakeOption(4, 1110549, -1, 1000, 1)};
    const auto before = options;
    Populate(options, false);
    bool ok = Check(g_result[0] == L"租赁 7 天", "rental validity rendered as Unicode") &&
        Check(g_result[1] == L"永久", "permanent uses category time-limit metadata") &&
        Check(g_result[2] == L"中文数量标签", "Unicode profile bypasses CP874 loss") &&
        Check(g_result[3] == L"native" && g_result[4] == L"native", "unknown/invalid business data falls back") &&
        Check(memcmp(options.data(), before.data(), options.size() * sizeof(Option)) == 0, "all prices/currency/records unchanged");
    g_result.clear(); options[2].price++;
    Populate(options, true);
    ok = Check(g_result[0] == L"租赁 7 天" && g_result[2] == L"native", "gift popup, truthful rental metadata and stale profile guard") && ok;
    ok = Check(std::wstring(g_convert(874, options[0].text)) == L"native", "context restored after popup") && ok;
    ok = Check(std::wstring(g_convert(65001, "UTF8")) == L"native", "other code pages preserved") && ok;
    g_result.clear();
    g_tooltip(nullptr, nullptr, &options[0], 0);
    g_tooltip(nullptr, nullptr, &options[1], 1);
    ok = Check(g_result[0] == L"租赁 7 天" && g_result[1] == L"永久", "hover tooltip renders explicit lease before purchase") && ok;
    printf("SHOP LABELS DLL CONTRACT %s (%u native fallbacks)\n", ok ? "PASS" : "FAIL", g_nativeCalls);
    return ok ? 0 : 1;
}
