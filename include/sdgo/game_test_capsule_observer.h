#pragma once
#include <stdint.h>
// Optional read-only receipt of the original activity-list load. The ordinary
// currency correction and its native request flow do not depend on GameTest.
#define SDGO_CAPSULE_GAME_TEST_SNAPSHOT_EXPORT "SdgoCapsuleGameTestGetSnapshotV1"
#pragma pack(push, 8)
struct SdgoCapsuleGameTestSnapshotV1 {
    uint32_t size,version;
    uint64_t run_hash;
    uint32_t process_id,ui_thread_id,generation,result;
    uint32_t grid_id,machine_id,price,currency,registry_currency;
};
#pragma pack(pop)
static_assert(sizeof(SdgoCapsuleGameTestSnapshotV1)==56,"capsule GameTest observer ABI");
typedef int (__cdecl *SdgoCapsuleGameTestGetSnapshotV1Fn)(SdgoCapsuleGameTestSnapshotV1*);
