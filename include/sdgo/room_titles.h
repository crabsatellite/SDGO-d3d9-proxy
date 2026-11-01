/* Optional, process-local room title service supplied by room_titles_server.
 * This is a feature ABI, independent of the framework's core API.
 * UTF-8 data is copied into caller-owned storage; no allocation crosses DLLs.
 */
#pragma once

#include <stdint.h>

#define SDGO_ROOM_TITLE_API_VERSION 1
#define SDGO_ROOM_TITLE_MAX_UTF8_BYTES 512
#define SDGO_ROOM_TITLE_MAX_SCALARS 128
#define SDGO_ROOM_TITLE_LEGACY_BYTES 20

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SdgoRoomTitleApi {
    uint32_t size;
    uint32_t version;

    /* Look up the server-confirmed extended title for a pilot's current room.
     * Returns its byte count, excluding the terminating NUL, or zero when the
     * pilot has no extended title or capacity is insufficient. Never truncates.
     * Pilot identifiers are native in-process identities, not account names.
     * An absent provider means callers retain the native 20-byte title path.
     */
    uint32_t(__cdecl* QueryForPilot)(uint32_t pilot, char* utf8, uint32_t capacity);
} SdgoRoomTitleApi;

/* Export name: SdgoRoomTitleApiV1. Returns NULL until the provider is active.
 * A successful provider remains loaded for the process lifetime.
 */
typedef const SdgoRoomTitleApi*(__cdecl* SdgoRoomTitleApiV1Fn)(void);

#ifdef __cplusplus
}
#endif
