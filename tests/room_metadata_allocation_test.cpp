#include "room_metadata_common.h"
#include "../mods/room_titles_server/room_titles_server_logic.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

namespace {

bool g_allocation_failure_armed = false;
std::size_t g_fail_at = 0;
std::size_t g_allocation_count = 0;

void* Allocate(std::size_t size)
{
    if (g_allocation_failure_armed) {
        const std::size_t allocation = g_allocation_count++;
        if (allocation == g_fail_at)
            throw std::bad_alloc();
    }

    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}

void ArmAllocationFailure(std::size_t fail_at)
{
    g_fail_at = fail_at;
    g_allocation_count = 0;
    g_allocation_failure_armed = true;
}

std::size_t DisarmAllocationFailure()
{
    g_allocation_failure_armed = false;
    return g_allocation_count;
}

namespace rm = sdgo::room_metadata;

constexpr std::uint64_t kEpoch = 0x1020304050607080ull;
constexpr std::uint32_t kRoomId = 42;
constexpr std::uint32_t kFirstGeneration = 71;
constexpr rm::SessionId kOwner = 1001;
constexpr rm::SessionId kNewOwner = 2002;
constexpr std::size_t kMaximumAttempts = 64;

int g_failures = 0;

void Check(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", description);
    if (!condition)
        ++g_failures;
}

bool SameKey(const rm::RoomKey& left, const rm::RoomKey& right)
{
    return left.server_epoch == right.server_epoch &&
           left.room_id == right.room_id &&
           left.generation == right.generation;
}

bool SameSnapshot(const rm::RoomSnapshot& left,
                  const rm::RoomSnapshot& right)
{
    return SameKey(left.key, right.key) &&
           left.revision == right.revision &&
           left.owner_session == right.owner_session &&
           left.title == right.title;
}

std::string LongTitle(char value)
{
    return std::string(96, value);
}

void TestCreateRoomAllocationFailures()
{
    const std::string title = LongTitle('C');
    std::size_t failure_points = 0;
    std::size_t successful_allocation_count = 0;
    bool reached_success = false;

    for (std::size_t fail_at = 0; fail_at < kMaximumAttempts; ++fail_at) {
        rm::RoomStore store(kEpoch, kFirstGeneration);
        rm::StateResult result;
        bool threw = false;

        ArmAllocationFailure(fail_at);
        try {
            result = store.CreateRoom(kRoomId, kOwner, title);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        const std::size_t allocations = DisarmAllocationFailure();

        if (threw) {
            ++failure_points;
            const rm::RoomKey absent_key = {kEpoch, kRoomId, kFirstGeneration};
            const rm::StateResult absent = store.Query(absent_key);
            const rm::StateResult recovery =
                store.CreateRoom(kRoomId, kOwner, title);
            Check(store.room_count() == 1 &&
                      absent.error == rm::StateError::NotFound &&
                      recovery.ok() &&
                      recovery.snapshot.key.generation == kFirstGeneration &&
                      recovery.snapshot.revision == 1,
                  "CreateRoom allocation failure preserves room state and generation");
            continue;
        }

        reached_success = true;
        successful_allocation_count = allocations;
        const rm::StateResult stored = store.Query(result.snapshot.key);
        const rm::StateResult next =
            store.CreateRoom(kRoomId + 1, kOwner, title);
        Check(result.ok() &&
                  result.snapshot.key.generation == kFirstGeneration &&
                  result.snapshot.revision == 1 &&
                  result.snapshot.owner_session == kOwner &&
                  result.snapshot.title == title &&
                  stored.ok() &&
                  SameSnapshot(stored.snapshot, result.snapshot) &&
                  next.ok() &&
                  next.snapshot.key.generation == kFirstGeneration + 1,
              "CreateRoom commits exactly once after all allocation points pass");
        break;
    }

    Check(reached_success &&
              failure_points == successful_allocation_count &&
              failure_points != 0,
          "CreateRoom fail-at sweep covers every observed allocation");
}

void TestUpdateTitleAllocationFailures()
{
    const std::string old_title = LongTitle('O');
    const std::string new_title = LongTitle('N') + std::string(24, 'W');
    std::size_t failure_points = 0;
    std::size_t successful_allocation_count = 0;
    bool reached_success = false;

    for (std::size_t fail_at = 0; fail_at < kMaximumAttempts; ++fail_at) {
        rm::RoomStore store(kEpoch, kFirstGeneration);
        const rm::StateResult created =
            store.CreateRoom(kRoomId, kOwner, old_title);
        Check(created.ok(), "UpdateTitle fixture room is created");
        if (!created.ok())
            return;

        rm::StateResult result;
        bool threw = false;
        ArmAllocationFailure(fail_at);
        try {
            result = store.UpdateTitle(created.snapshot.key, kOwner,
                                       created.snapshot.revision, new_title);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        const std::size_t allocations = DisarmAllocationFailure();

        if (threw) {
            ++failure_points;
            const rm::StateResult unchanged = store.Query(created.snapshot.key);
            const rm::StateResult recovery =
                store.UpdateTitle(created.snapshot.key, kOwner,
                                  created.snapshot.revision, new_title);
            Check(unchanged.ok() &&
                      SameSnapshot(unchanged.snapshot, created.snapshot) &&
                      recovery.ok() &&
                      recovery.snapshot.revision ==
                          created.snapshot.revision + 1 &&
                      recovery.snapshot.title == new_title,
                  "UpdateTitle allocation failure preserves title and revision");
            continue;
        }

        reached_success = true;
        successful_allocation_count = allocations;
        const rm::StateResult stored = store.Query(created.snapshot.key);
        Check(result.ok() &&
                  result.snapshot.revision == created.snapshot.revision + 1 &&
                  result.snapshot.owner_session == kOwner &&
                  result.snapshot.title == new_title &&
                  stored.ok() &&
                  SameSnapshot(stored.snapshot, result.snapshot),
              "UpdateTitle commits title and revision after all allocation points pass");
        break;
    }

    Check(reached_success &&
              failure_points == successful_allocation_count &&
              failure_points != 0,
          "UpdateTitle fail-at sweep covers every observed allocation");
}

void TestTransferHostAllocationFailures()
{
    const std::string title = LongTitle('T');
    std::size_t failure_points = 0;
    std::size_t successful_allocation_count = 0;
    bool reached_success = false;

    for (std::size_t fail_at = 0; fail_at < kMaximumAttempts; ++fail_at) {
        rm::RoomStore store(kEpoch, kFirstGeneration);
        const rm::StateResult created =
            store.CreateRoom(kRoomId, kOwner, title);
        Check(created.ok(), "TransferHost fixture room is created");
        if (!created.ok())
            return;

        rm::StateResult result;
        bool threw = false;
        ArmAllocationFailure(fail_at);
        try {
            result = store.TransferHost(created.snapshot.key, kOwner, kNewOwner,
                                        created.snapshot.revision);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        const std::size_t allocations = DisarmAllocationFailure();

        if (threw) {
            ++failure_points;
            const rm::StateResult unchanged = store.Query(created.snapshot.key);
            const rm::StateResult recovery =
                store.TransferHost(created.snapshot.key, kOwner, kNewOwner,
                                   created.snapshot.revision);
            Check(unchanged.ok() &&
                      SameSnapshot(unchanged.snapshot, created.snapshot) &&
                      recovery.ok() &&
                      recovery.snapshot.revision ==
                          created.snapshot.revision + 1 &&
                      recovery.snapshot.owner_session == kNewOwner,
                  "TransferHost allocation failure preserves owner and revision");
            continue;
        }

        reached_success = true;
        successful_allocation_count = allocations;
        const rm::StateResult stored = store.Query(created.snapshot.key);
        Check(result.ok() &&
                  result.snapshot.revision == created.snapshot.revision + 1 &&
                  result.snapshot.owner_session == kNewOwner &&
                  result.snapshot.title == title &&
                  stored.ok() &&
                  SameSnapshot(stored.snapshot, result.snapshot),
              "TransferHost commits owner and revision after allocations pass");
        break;
    }

    Check(reached_success &&
              failure_points == successful_allocation_count &&
              failure_points != 0,
          "TransferHost fail-at sweep covers every observed allocation");
}

void TestServerCacheAllocationFailures()
{
    namespace server = sdgo::room_titles_server;
    const std::string oldTitle = LongTitle('O');
    const std::string newTitle = LongTitle('N');
    server::NativeTitle oldNative;
    oldNative.size = 20;
    oldNative.bytes.fill('O');
    auto newNative = oldNative;
    newNative.bytes.fill('N');
    for (int updating = 0; updating != 2; ++updating) {
        std::size_t failed = 0;
        bool succeeded = false;
        for (std::size_t failAt = 0; failAt < kMaximumAttempts; ++failAt) {
            server::ServerState state(kEpoch);
            rm::StateResult before;
            if (updating)
                before = state.CreateRoom(server::RoomDomain::Team, kRoomId, kOwner,
                                           oldNative, &oldTitle, false);
            rm::StateResult result;
            bool threw = false;
            ArmAllocationFailure(failAt);
            try {
                result = updating ? state.SetExtendedTitle(before.snapshot.key, kOwner,
                    before.snapshot.revision, newTitle, newNative) :
                    state.CreateRoom(server::RoomDomain::Team, kRoomId, kOwner,
                                     newNative, &newTitle, false);
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            const auto allocations = DisarmAllocationFailure();
            if (threw) {
                ++failed;
                rm::RoomSnapshot snapshot;
                server::NativeTitle native;
                if (updating) {
                    Check(state.QueryRoom(kRoomId, &snapshot) &&
                          SameSnapshot(snapshot, before.snapshot) &&
                          state.QueryNativeTitle(kRoomId, &native) && native == oldNative &&
                          state.SetExtendedTitle(before.snapshot.key, kOwner,
                              before.snapshot.revision, newTitle, newNative).ok(),
                          "server cache/store update allocation failure preserves both copies and retries");
                } else {
                    Check(!state.QueryRoom(kRoomId, &snapshot) && !state.QueryPilotWireId(kOwner) &&
                          state.CreateRoom(server::RoomDomain::Team, kRoomId, kOwner,
                              newNative, &newTitle, false).ok(),
                          "server cache/store create allocation failure leaves no ghost room or membership");
                }
                continue;
            }
            rm::RoomSnapshot snapshot;
            Check(result.ok() && state.QueryRoom(kRoomId, &snapshot) &&
                  SameSnapshot(snapshot, result.snapshot) && snapshot.title == newTitle &&
                  failed == allocations && failed != 0,
                  "server cache/store allocation sweep reaches a single consistent commit");
            succeeded = true;
            break;
        }
        Check(succeeded, "server allocation sweep covered all observed failure points");
    }
}

}  // namespace

void* operator new(std::size_t size)
{
    return Allocate(size);
}

void* operator new[](std::size_t size)
{
    return Allocate(size);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

int main()
{
    TestCreateRoomAllocationFailures();
    TestUpdateTitleAllocationFailures();
    TestTransferHostAllocationFailures();
    TestServerCacheAllocationFailures();
    std::printf("%s: %d failure(s)\n",
                g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
