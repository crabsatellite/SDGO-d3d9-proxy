#pragma once

#include <stddef.h>
#include <stdint.h>

#define SDGO_GAME_TEST_PAGE_OBSERVER_VERSION 1u

enum SdgoHangarGameTestActionV1 {
    SDGO_HANGAR_GAME_TEST_ACTION_NONE = 0,
    SDGO_HANGAR_GAME_TEST_ACTION_SORT = 1,
    SDGO_HANGAR_GAME_TEST_ACTION_OPEN = 2,
    SDGO_HANGAR_GAME_TEST_ACTION_GB = 3,
    SDGO_HANGAR_GAME_TEST_ACTION_ITEM = 4,
    SDGO_HANGAR_GAME_TEST_ACTION_CONFIRM = 5,
    SDGO_HANGAR_GAME_TEST_ACTION_CANCEL = 6
};

enum SdgoHangarGameTestUnitFlagsV1 {
    SDGO_HANGAR_GAME_TEST_UNIT_RENTAL = 1u << 0,
    SDGO_HANGAR_GAME_TEST_UNIT_SHIP = 1u << 1,
    SDGO_HANGAR_GAME_TEST_UNIT_LOCKED = 1u << 2,
    SDGO_HANGAR_GAME_TEST_UNIT_SPECIAL = 1u << 3,
    SDGO_HANGAR_GAME_TEST_UNIT_FULL_READY = 1u << 4
};

enum SdgoHangarGameTestProtectionV1 {
    SDGO_HANGAR_GAME_TEST_PROTECTED_ACTIVE = 1u << 0,
    SDGO_HANGAR_GAME_TEST_PROTECTED_RENTAL = 1u << 1,
    SDGO_HANGAR_GAME_TEST_PROTECTED_SHIP = 1u << 2,
    SDGO_HANGAR_GAME_TEST_PROTECTED_LOCKED = 1u << 3,
    SDGO_HANGAR_GAME_TEST_PROTECTED_SPECIAL = 1u << 4,
    SDGO_HANGAR_GAME_TEST_PROTECTED_DEVELOPED = 1u << 5,
    SDGO_HANGAR_GAME_TEST_PROTECTED_DETAIL_MISSING = 1u << 6
};

enum SdgoHangarGameTestRunnerStateV1 {
    SDGO_HANGAR_GAME_TEST_RUNNER_IDLE = 0,
    SDGO_HANGAR_GAME_TEST_RUNNER_READY = 1,
    SDGO_HANGAR_GAME_TEST_RUNNER_WAITING = 2,
    SDGO_HANGAR_GAME_TEST_RUNNER_COMPLETE = 3,
    SDGO_HANGAR_GAME_TEST_RUNNER_STOPPED = 4,
    SDGO_HANGAR_GAME_TEST_RUNNER_UNKNOWN = 5
};

enum SdgoStorageGameTestActionV1 {
    SDGO_STORAGE_GAME_TEST_ACTION_NONE = 0,
    SDGO_STORAGE_GAME_TEST_ACTION_CLAIM_ALL = 1
};

enum SdgoStorageGameTestTabV1 {
    SDGO_STORAGE_GAME_TEST_TAB_ITEMS = 0,
    SDGO_STORAGE_GAME_TEST_TAB_GIFTS = 1
};

enum SdgoStorageGameTestPhaseV1 {
    SDGO_STORAGE_GAME_TEST_PHASE_IDLE = 0,
    SDGO_STORAGE_GAME_TEST_PHASE_NEED_LIST = 1,
    SDGO_STORAGE_GAME_TEST_PHASE_LIST = 2,
    SDGO_STORAGE_GAME_TEST_PHASE_NEED_CHECK = 3,
    SDGO_STORAGE_GAME_TEST_PHASE_CHECK = 4,
    SDGO_STORAGE_GAME_TEST_PHASE_NEED_CLAIM = 5,
    SDGO_STORAGE_GAME_TEST_PHASE_CLAIM = 6,
    SDGO_STORAGE_GAME_TEST_PHASE_COMPLETE = 7,
    SDGO_STORAGE_GAME_TEST_PHASE_STOPPED = 8,
    SDGO_STORAGE_GAME_TEST_PHASE_UNKNOWN = 9
};

enum SdgoStorageGameTestReasonV1 {
    SDGO_STORAGE_GAME_TEST_REASON_NONE = 0,
    SDGO_STORAGE_GAME_TEST_REASON_CANCELLED = 1,
    SDGO_STORAGE_GAME_TEST_REASON_REJECTED = 2,
    SDGO_STORAGE_GAME_TEST_REASON_SELECTION = 3,
    SDGO_STORAGE_GAME_TEST_REASON_CHANGED = 4,
    SDGO_STORAGE_GAME_TEST_REASON_TIMEOUT = 5,
    SDGO_STORAGE_GAME_TEST_REASON_SEND_FAILED = 6,
    SDGO_STORAGE_GAME_TEST_REASON_LIMIT = 7,
    SDGO_STORAGE_GAME_TEST_REASON_DISCONNECTED = 8,
    SDGO_STORAGE_GAME_TEST_REASON_PAGE_LIMIT = 9
};

enum SdgoStorageGameTestPendingOperationV1 {
    SDGO_STORAGE_GAME_TEST_PENDING_NONE = 0,
    SDGO_STORAGE_GAME_TEST_PENDING_LIST = 1,
    SDGO_STORAGE_GAME_TEST_PENDING_CHECK = 2,
    SDGO_STORAGE_GAME_TEST_PENDING_CLAIM = 3
};

#pragma pack(push, 8)
typedef struct SdgoHangarGameTestSnapshotV1 {
    uint32_t size;
    uint32_t version;
    uint64_t run_hash;
    uint64_t session;
    uint32_t process_id;
    uint32_t ui_thread_id;
    uint32_t loaded;
    uint32_t visible;
    uint32_t generation;
    uint32_t action_sequence;
    uint32_t reply_sequence;
    uint32_t last_action;
    uint32_t capacity;
    uint32_t active_slot;
    uint32_t unit_count;
    uint32_t preview_removed;
    uint32_t busy;
    uint32_t armed;
    uint32_t needs_relog;
    uint32_t runner_state;
    uint32_t runner_completed;
    uint32_t runner_total;
    uint32_t submit_index;
    uint32_t submit_total;
    uint32_t reserved[6];
} SdgoHangarGameTestSnapshotV1;

typedef struct SdgoHangarGameTestUnitV1 {
    uint64_t uid;
    uint32_t unit_id;
    uint16_t slot;
    uint8_t level;
    uint8_t flags;
    uint32_t protection_flags;
    uint32_t reserved;
} SdgoHangarGameTestUnitV1;

typedef struct SdgoStorageGameTestSnapshotV1 {
    uint32_t size;
    uint32_t version;
    uint64_t run_hash;
    uint64_t session;
    uint32_t process_id;
    uint32_t ui_thread_id;
    uint32_t loaded;
    uint32_t visible;
    uint32_t generation;
    uint32_t action_sequence;
    uint32_t reply_sequence;
    uint32_t preflight_reply_sequence;
    uint32_t last_action;
    uint32_t current_tab;
    uint32_t page;
    uint32_t total;
    uint32_t page_entry_count;
    uint32_t batch_phase;
    uint32_t batch_reason;
    uint32_t completed;
    uint32_t busy;
    uint32_t armed;
    uint32_t outstanding;
    uint32_t timer_observed;
    uint32_t pending_operation;
    uint64_t pending_id;
    uint64_t pending_stamp;
    uint32_t reserved[4];
} SdgoStorageGameTestSnapshotV1;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(SdgoHangarGameTestSnapshotV1) == 128, "hangar observer ABI");
static_assert(sizeof(SdgoHangarGameTestUnitV1) == 24, "hangar unit observer ABI");
static_assert(sizeof(SdgoStorageGameTestSnapshotV1) == 144, "storage observer ABI");

namespace sdgo::game_test_page_observer {
inline bool ValidCanonicalRunId(const char* value, size_t length) {
    if (!value || length != 36) return false;
    bool nonzero = false;
    for (size_t i = 0; i < length; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
            continue;
        }
        const bool digit = value[i] >= '0' && value[i] <= '9';
        const bool lower = value[i] >= 'a' && value[i] <= 'f';
        if (!digit && !lower) return false;
        nonzero = nonzero || value[i] != '0';
    }
    return nonzero;
}

inline uint64_t ClientRunHash(
    const char* run, size_t run_length, const char* role, size_t role_length) {
    if (!ValidCanonicalRunId(run, run_length) || !role || role_length != 6 ||
        role[0] != 'c' || role[1] != 'l' || role[2] != 'i' ||
        role[3] != 'e' || role[4] != 'n' || role[5] != 't') return 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < run_length; ++i) {
        hash ^= static_cast<uint8_t>(run[i]);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}
}
#endif

#define SDGO_HANGAR_GAME_TEST_SNAPSHOT_EXPORT "SdgoHangarGameTestGetSnapshotV1"
#define SDGO_HANGAR_GAME_TEST_UNITS_EXPORT "SdgoHangarGameTestGetUnitsV1"
#define SDGO_HANGAR_GAME_TEST_ACTION_EXPORT "SdgoHangarGameTestActionV1"
#define SDGO_STORAGE_GAME_TEST_SNAPSHOT_EXPORT "SdgoStorageGameTestGetSnapshotV1"
#define SDGO_STORAGE_GAME_TEST_ACTION_EXPORT "SdgoStorageGameTestActionV1"

typedef int (__cdecl *SdgoHangarGameTestGetSnapshotV1Fn)(SdgoHangarGameTestSnapshotV1* output);
typedef int (__cdecl *SdgoHangarGameTestGetUnitsV1Fn)(
    uint64_t run_hash, uint32_t offset, SdgoHangarGameTestUnitV1* output,
    uint32_t element_size, uint32_t capacity);
typedef int (__cdecl *SdgoHangarGameTestActionV1Fn)(uint64_t run_hash, uint32_t action);
typedef int (__cdecl *SdgoStorageGameTestGetSnapshotV1Fn)(SdgoStorageGameTestSnapshotV1* output);
typedef int (__cdecl *SdgoStorageGameTestActionV1Fn)(uint64_t run_hash, uint32_t action);
