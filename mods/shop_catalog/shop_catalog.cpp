#include <windows.h>
#include "sdgo/sdgo_api.h"

// The Manager is the sole publisher of shop configuration. This dormant host
// component preserves build identification; loading WorldServer never applies
// SQL, reads owner credentials or starts a configuration worker.
SDGO_MOD_ENTRY
{
    if (!api || api->version != SDGO_API_VERSION || !api->host_base || !api->Log)
        return 0;
    const auto* base = static_cast<const uint8_t*>(api->host_base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x400)
        return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != 0x548A86E3 ||
        nt->OptionalHeader.SizeOfImage != 0x0010C000)
        return 0;
    api->Log(mod, SDGO_LOG_INFO, "shop configuration is Manager-owned; no host-startup SQL or background controller");
    return 1;
}
