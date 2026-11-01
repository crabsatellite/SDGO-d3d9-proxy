// Optional per-mod configuration without extending the framework ABI.
// config.ini is authoritative when present; otherwise read legacy mod.ini.
#pragma once
#include <windows.h>
#include <string>
#include <cstring>
#include "sdgo_api.h"

namespace sdgo {
class ModSettings {
    const SdgoApi* api_;
    SdgoMod* mod_;
    std::wstring path_;
public:
    ModSettings(const SdgoApi* api, SdgoMod* mod) : api_(api), mod_(mod) {
        path_ = std::wstring(api->ModDirectory(mod)) + L"\\config.ini";
        if (GetFileAttributesW(path_.c_str()) == INVALID_FILE_ATTRIBUTES) path_.clear();
    }
    uint32_t String(const char* key, const char* def, char* out, uint32_t size) const {
        if (path_.empty()) return api_->SettingString(mod_, key, def, out, size);
        if (!out || size < 2) return 0;
        const std::wstring wkey(key, key + strlen(key)), wdef(def, def + strlen(def));
        wchar_t value[2048] = {};
        DWORD n = GetPrivateProfileStringW(L"settings", wkey.c_str(), wdef.c_str(), value, 2048, path_.c_str());
        int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
        if (n == 2047 || needed <= 0 || static_cast<uint32_t>(needed) > size) {
            out[0] = '\0';
            return size - 1; // callers reject truncation, never use a partial route
        }
        WideCharToMultiByte(CP_UTF8, 0, value, -1, out, static_cast<int>(size), nullptr, nullptr);
        return static_cast<uint32_t>(needed - 1);
    }
};
}
