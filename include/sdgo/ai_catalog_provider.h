#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include "sdgo_api.h"

namespace sdgo {
// Optional process-local extension. With no active catalog mod, the consumer's
// existing settings and native AI-NPC filtering remain exactly the same.
inline bool AiCatalogPool(const SdgoApi* api, SdgoMod* mod, int level, std::vector<uint32_t>& pool) {
    if (!api || !api->ModDirectory) return false;
    std::wstring path = api->ModDirectory(mod);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    path.resize(slash);
    path += L"\\ai_catalog\\ai_catalog.dll";
    HMODULE provider = GetModuleHandleW(path.c_str());
    if (!provider) return false;
    using GetPool = int(__cdecl*)(int, uint32_t*, uint32_t);
    auto getPool = reinterpret_cast<GetPool>(GetProcAddress(provider, "SdgoAiCatalogPoolV1"));
    if (!getPool) return false;
    std::vector<uint32_t> ids(3000);
    const int count = getPool(level, ids.data(), static_cast<uint32_t>(ids.size()));
    pool.clear();
    if (count < 0 || count > static_cast<int>(ids.size())) {
        api->Log(mod, SDGO_LOG_ERROR, "AI catalog refused level %d; no expanded AI added", level);
        return true;
    }
    ids.resize(static_cast<size_t>(count));
    pool.swap(ids);
    return true;
}
} // namespace sdgo
