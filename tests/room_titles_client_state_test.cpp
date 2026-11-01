#include "client_state.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rt = sdgo::room_titles;
namespace rm = sdgo::room_metadata;

namespace {

int g_failures = 0;

void Check(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", description);
    if (!condition) ++g_failures;
}

rm::SnapshotRecord Record(std::uint64_t epoch,
                          std::uint32_t room_id,
                          std::uint32_t generation,
                          std::uint32_t revision,
                          const std::string& title)
{
    rm::SnapshotRecord result;
    result.key.server_epoch = epoch;
    result.key.room_id = room_id;
    result.key.generation = generation;
    result.revision = revision;
    result.title = title;
    return result;
}

rm::SnapshotRecord Record(const rm::RoomSnapshot& snapshot)
{
    return Record(snapshot.key.server_epoch,
                  snapshot.key.room_id,
                  snapshot.key.generation,
                  snapshot.revision,
                  snapshot.title);
}

rm::SnapshotBatch Batch(std::uint64_t epoch,
                        std::uint32_t request_id,
                        std::vector<rm::SnapshotRecord> rooms = {})
{
    rm::SnapshotBatch result;
    result.server_epoch = epoch;
    result.request_id = request_id;
    result.rooms = std::move(rooms);
    return result;
}

rm::Message Snapshot(const rm::SnapshotRecord& record,
                     std::uint32_t request_id)
{
    rm::Message result;
    result.kind = rm::MessageKind::Snapshot;
    result.server_epoch = record.key.server_epoch;
    result.room_id = record.key.room_id;
    result.generation = record.key.generation;
    result.revision = record.revision;
    result.request_id = request_id;
    result.title = record.title;
    return result;
}

rm::Message HelloAck(std::uint64_t epoch, std::uint32_t request_id)
{
    rm::Message result;
    result.kind = rm::MessageKind::HelloAck;
    result.server_epoch = epoch;
    result.request_id = request_id;
    return result;
}

rm::Message ErrorMessage(std::uint64_t epoch,
                         const rm::RoomKey& key,
                         std::uint32_t revision,
                         std::uint32_t request_id,
                         rm::RemoteError error)
{
    rm::Message result;
    result.kind = rm::MessageKind::Error;
    result.error = error;
    result.server_epoch = epoch;
    result.room_id = key.room_id;
    result.generation = key.generation;
    result.revision = revision;
    result.request_id = request_id;
    return result;
}

bool ReadyWithEmptyBatch(rt::ClientState* state,
                         rt::ConnectionId connection,
                         std::uint64_t epoch,
                         std::uint64_t now)
{
    const rm::Message hello = state->BeginHello(connection, now);
    return hello.request_id != 0 &&
           state->AcceptBatch(connection,
                              Batch(epoch, hello.request_id));
}

void TestCapabilityAndEpoch()
{
    rt::ClientState state;
    Check(state.CanStartBattle(999),
          "missing capability state preserves legacy battle start");
    const rm::Message hello = state.BeginHello(1, 100);
    const rm::Message repeated = state.BeginHello(1, 200);
    Check(hello.kind == rm::MessageKind::Hello &&
              hello.request_id != 0 &&
              repeated.request_id == hello.request_id,
          "unexpired capability offer reuses request ID");

    Check(!state.AcceptBatch(1, Batch(10, hello.request_id + 100)) &&
              !state.IsReady(1),
          "mismatched capability nonce is rejected");
    Check(state.AcceptBatch(1, Batch(10, hello.request_id)) &&
              state.IsReady(1) &&
              state.room_count(1) == 0 &&
              state.CanStartBattle(1),
          "matching empty batch establishes epoch and capability");
    Check(!state.AcceptBatch(1, Batch(10, hello.request_id)) &&
              state.LastError(1).code == rt::ClientError::StaleResponse,
          "replayed capability response is stale");

    const rm::SnapshotRecord room = Record(10, 7, 1, 1, "room seven");
    Check(state.AcceptBatch(1, Batch(10, 0, {room})) &&
              state.Query(1, 7).has_value(),
          "ready connection accepts unsolicited same-epoch snapshot");

    Check(!state.AcceptBatch(1, Batch(20, 0)) &&
              !state.IsReady(1) &&
              !state.Query(1, 7).has_value() &&
              state.LastError(1).code == rt::ClientError::EpochChanged,
          "unconfirmed epoch change clears cache and readiness");

    const rm::Message reconnect = state.BeginHello(1, 300);
    Check(reconnect.request_id != 0 &&
              reconnect.request_id != hello.request_id &&
              !state.AcceptBatch(1, Batch(20, hello.request_id)) &&
              state.AcceptBatch(1, Batch(20, reconnect.request_id)) &&
              state.IsReady(1),
          "new epoch requires a fresh matching Hello offer");

    rt::ClientState hello_ack_state;
    const rm::Message hello_ack_offer =
        hello_ack_state.BeginHello(2, 1000);
    Check(hello_ack_state.AcceptMessage(
              2, HelloAck(30, hello_ack_offer.request_id)) &&
              hello_ack_state.IsReady(2),
          "single HelloAck establishes capability");

    rt::ClientState timeout_state;
    const rm::Message first = timeout_state.BeginHello(3, 100);
    const rm::Message after_timeout =
        timeout_state.BeginHello(3, 100 + rt::kHelloTimeoutMilliseconds);
    Check(first.request_id != 0 &&
              after_timeout.request_id != 0 &&
              after_timeout.request_id != first.request_id,
          "expired Hello offer receives a new request ID");
}

void TestPendingAndAcknowledgement()
{
    rt::ClientState state;
    Check(ReadyWithEmptyBatch(&state, 1, 100, 0),
          "pending fixture establishes capability");
    const rm::SnapshotRecord initial =
        Record(100, 50, 5, 8, "original room");
    const rm::SnapshotRecord other =
        Record(100, 60, 7, 2, "unrelated room");
    Check(state.AcceptBatch(1, Batch(100, 0, {initial, other})),
          "snapshot batch seeds two independent rooms");

    const std::string long_title =
        "mission title beyond twenty bytes !ai count=4";
    const rm::Message set =
        state.BeginSetTitle(1, 50, long_title, 100);
    Check(set.kind == rm::MessageKind::SetTitle &&
              set.request_id != 0 &&
              set.server_epoch == 100 &&
              set.room_id == 50 &&
              set.generation == 5 &&
              set.revision == 8 &&
              set.title == long_title &&
              state.HasPending(1) &&
              !state.CanStartBattle(1),
          "existing room SetTitle carries cached key and blocks start");

    const rm::Message overlapping =
        state.BeginSetTitle(1, 50, "overlap", 101);
    Check(overlapping.request_id == 0 &&
              state.LastError(1).code == rt::ClientError::PendingExists,
          "one pending title per connection prevents overlap");
    Check(state.BeginHello(1, 102).request_id == 0 &&
              state.LastError(1).code == rt::ClientError::PendingExists,
          "capability probe cannot overtake an awaiting title request");

    const rm::SnapshotRecord acknowledged =
        Record(100, 50, 5, 9, long_title);
    Check(state.AcceptMessage(1, Snapshot(acknowledged, set.request_id)) &&
              !state.HasPending(1) &&
              state.CanStartBattle(1) &&
              state.Query(1, 50)->title == long_title &&
              state.Query(1, 60)->title == "unrelated room" &&
              state.Query(1, 60)->revision == 2,
          "matching request, title and key Snapshot completes pending update");

    const std::string create_title =
        "new PvP room title and rules beyond byte twenty";
    const rm::Message create =
        state.BeginSetTitle(1, 0, create_title, 200);
    Check(create.request_id != 0 &&
              create.server_epoch == 0 &&
              create.room_id == 0 &&
              create.generation == 0 &&
              create.revision == 0,
          "room creation emits keyless SetTitle shape");
    const rm::SnapshotRecord created =
        Record(100, 51, 6, 1, create_title);
    Check(state.AcceptBatch(1, Batch(100, create.request_id, {created})) &&
              !state.HasPending(1) &&
              state.Query(1, 51)->title == create_title,
          "server-assigned creation key completes through matching batch");
}

void TestLegacyRoomUpgradeAndFallback()
{
    rm::RoomStore server(700);
    const rm::StateResult observed = server.CreateLegacyRoom(80, 8000);
    Check(observed.ok() && observed.snapshot.title.empty(),
          "server observes legacy room with identity-only metadata");

    rt::ClientState client;
    const rm::Message hello = client.BeginHello(1, 0);
    Check(client.AcceptBatch(
              1, Batch(700, hello.request_id, {Record(observed.snapshot)})) &&
              client.Query(1, 80).has_value() &&
              client.Query(1, 80)->title.empty() &&
              client.Query(1, 80)->revision == 1,
          "client caches legacy key while extended title remains absent");

    const std::string full_title =
        "legacy room upgraded to full title beyond twenty bytes";
    const rm::Message first_request =
        client.BeginSetTitle(1, 80, full_title, 100);
    Check(first_request.request_id != 0 &&
              first_request.server_epoch == observed.snapshot.key.server_epoch &&
              first_request.room_id == observed.snapshot.key.room_id &&
              first_request.generation == observed.snapshot.key.generation &&
              first_request.revision == observed.snapshot.revision,
          "identity-only legacy room can submit an authorized full title");

    rm::SnapshotRecord empty_ack = Record(observed.snapshot);
    empty_ack.revision = 2;
    Check(!client.AcceptMessage(
              1, Snapshot(empty_ack, first_request.request_id)) &&
              client.Pending(1)->status == rt::PendingStatus::Failed &&
              client.Query(1, 80)->revision == 1 &&
              client.Query(1, 80)->title.empty(),
          "empty Snapshot cannot falsely acknowledge a nonempty title draft");
    client.CancelPending(1);
    const rm::Message request =
        client.BeginSetTitle(1, 80, full_title, 200);

    const rm::StateResult updated =
        server.UpdateTitle(observed.snapshot.key, 8000,
                           observed.snapshot.revision, full_title);
    Check(updated.ok() &&
              client.AcceptMessage(
                  1, Snapshot(Record(updated.snapshot), request.request_id)) &&
              client.Query(1, 80)->title == full_title &&
              client.Query(1, 80)->revision == 2 &&
              !client.HasPending(1),
          "confirmed server update upgrades legacy room metadata");

    rm::SnapshotRecord cleared = Record(updated.snapshot);
    cleared.revision = 3;
    cleared.title.clear();
    Check(client.AcceptMessage(1, Snapshot(cleared, 0)) &&
              client.Query(1, 80)->title.empty() &&
              client.Query(1, 80)->revision == 3 &&
              client.Query(1, 80)->key.generation ==
                  updated.snapshot.key.generation,
          "empty Snapshot clears full title while preserving identity revision");
    Check(!client.AcceptMessage(
              1, Snapshot(Record(updated.snapshot), 0)) &&
              client.Query(1, 80)->title.empty() &&
              client.Query(1, 80)->revision == 3,
          "older full-title Snapshot cannot undo empty-title downgrade");
}

void TestFailuresPreserveDraft()
{
    rt::ClientState state;
    Check(ReadyWithEmptyBatch(&state, 1, 200, 0),
          "failure fixture establishes capability");
    const rm::SnapshotRecord room = Record(200, 9, 3, 1, "base");
    Check(state.AcceptMessage(1, Snapshot(room, 0)),
          "failure fixture seeds room");

    const std::string rejected_title = "rejected long title !ai";
    const rm::Message rejected =
        state.BeginSetTitle(1, 9, rejected_title, 10);
    const rm::Message error =
        ErrorMessage(200, room.key, room.revision, rejected.request_id,
                     rm::RemoteError::Unauthorized);
    Check(state.AcceptMessage(1, error) &&
              state.Pending(1).has_value() &&
              state.Pending(1)->status == rt::PendingStatus::Failed &&
              state.Pending(1)->request.title == rejected_title &&
              state.Pending(1)->error.remote_error ==
                  rm::RemoteError::Unauthorized &&
              !state.CanStartBattle(1),
          "server error keeps failed draft and blocks battle start");
    state.CancelPending(1);
    Check(!state.HasPending(1) && state.CanStartBattle(1),
          "explicit cancel resolves failed pending state");

    const std::string timeout_title = "timeout draft after byte twenty";
    const rm::Message timed =
        state.BeginSetTitle(1, 9, timeout_title, 100);
    state.AdvanceTime(1, 100 + rt::kSetTitleTimeoutMilliseconds);
    Check(timed.request_id != 0 &&
              state.Pending(1)->status == rt::PendingStatus::TimedOut &&
              state.Pending(1)->request.title == timeout_title &&
              state.LastError(1).code == rt::ClientError::Timeout &&
              !state.CanStartBattle(1),
          "timeout preserves draft and blocks silent legacy fallback");
    state.CancelPending(1);

    const rm::Message local_failure =
        state.BeginSetTitle(1, 9, "local transport failure draft", 900);
    Check(state.FailPending(1, local_failure.request_id) &&
              state.Pending(1)->status == rt::PendingStatus::Failed &&
              state.Pending(1)->request.title ==
                  "local transport failure draft" &&
              state.Pending(1)->error.code ==
                  rt::ClientError::TransportError &&
              state.Pending(1)->error.remote_error == rm::RemoteError::None &&
              !state.CanStartBattle(1),
          "local send failure preserves draft without server rejection");
    Check(!state.FailPending(1, local_failure.request_id + 1) &&
              state.Pending(1)->request.request_id ==
                  local_failure.request_id,
          "local failure requires the exact pending request ID");
    state.CancelPending(1);

    const std::string invalid("bad\0title", 9);
    const rm::Message invalid_request =
        state.BeginSetTitle(1, 9, invalid, 1000);
    Check(invalid_request.request_id == 0 &&
              !state.HasPending(1) &&
              state.LastError(1).code == rt::ClientError::InvalidTitle,
          "invalid UTF-8 title is rejected before pending state");

    const rm::Message mismatch =
        state.BeginSetTitle(1, 9, "expected title", 1100);
    const rm::SnapshotRecord wrong =
        Record(200, 9, 3, 2, "different title");
    Check(!state.AcceptMessage(1, Snapshot(wrong, mismatch.request_id)) &&
              state.Pending(1)->status == rt::PendingStatus::Failed &&
              state.Pending(1)->request.title == "expected title" &&
              state.Query(1, 9)->revision == 1,
          "mismatched acknowledgement fails draft without cache mutation");
}

void TestStaleReuseAndDestroy()
{
    rt::ClientState state;
    Check(ReadyWithEmptyBatch(&state, 1, 300, 0),
          "stale fixture establishes capability");
    const rm::SnapshotRecord current =
        Record(300, 70, 10, 5, "current");
    Check(state.AcceptMessage(1, Snapshot(current, 0)),
          "stale fixture seeds current generation");

    Check(!state.AcceptMessage(
              1, Snapshot(Record(300, 70, 10, 4, "old revision"), 0)) &&
              state.Query(1, 70)->revision == 5 &&
              state.Query(1, 70)->title == "current",
          "older revision cannot replace cached metadata");
    Check(!state.AcceptBatch(
              1, Batch(300, 0,
                       {Record(300, 71, 12, 1, "must not partially insert"),
                        Record(300, 70, 10, 4, "old revision")})) &&
              !state.Query(1, 71).has_value() &&
              state.Query(1, 70)->title == "current",
          "mixed stale batch is rejected atomically across rooms");
    Check(!state.AcceptMessage(
              1, Snapshot(Record(300, 70, 9, 99, "old generation"), 0)) &&
              state.Query(1, 70)->key.generation == 10,
          "older generation cannot replace reused room metadata");
    Check(!state.AcceptMessage(
              1, Snapshot(Record(300, 70, 10, 5, "conflict"), 0)) &&
              state.Query(1, 70)->title == "current",
          "same revision with another title is rejected");

    const rm::SnapshotRecord reused =
        Record(300, 70, 11, 1, "new generation");
    Check(state.AcceptMessage(1, Snapshot(reused, 0)) &&
              state.Query(1, 70)->key.generation == 11 &&
              state.Query(1, 70)->title == "new generation",
          "new generation replaces destroyed and reused room ID");

    const rm::Message set =
        state.BeginSetTitle(1, 70, "will be gone", 100);
    const rm::Message gone =
        ErrorMessage(300, reused.key, reused.revision, set.request_id,
                     rm::RemoteError::RoomNotFound);
    Check(state.AcceptMessage(1, gone) &&
              !state.Query(1, 70).has_value() &&
              state.Pending(1)->status == rt::PendingStatus::Failed,
          "RoomNotFound error removes matching destroyed generation");
}

void TestConnectionIsolation()
{
    rt::ClientState state;
    Check(ReadyWithEmptyBatch(&state, 10, 400, 0) &&
              ReadyWithEmptyBatch(&state, 20, 500, 0),
          "two native connections establish separate epochs");
    Check(state.AcceptMessage(
              10, Snapshot(Record(400, 1, 1, 1, "connection A"), 0)) &&
              state.AcceptMessage(
                  20, Snapshot(Record(500, 1, 2, 1, "connection B"), 0)),
          "same room ID is accepted independently per connection");
    Check(state.Query(10, 1)->title == "connection A" &&
              state.Query(20, 1)->title == "connection B",
          "room caches do not cross native connections");

    const rm::Message pending_a =
        state.BeginSetTitle(10, 1, "A update", 100);
    Check(!state.AcceptMessage(
              20, Snapshot(Record(500, 1, 2, 2, "A update"),
                           pending_a.request_id)) &&
              state.LastError(20).code ==
                  rt::ClientError::MismatchedResponse &&
              state.HasPending(10),
          "request IDs cannot complete pending work on another connection");

    state.DropConnection(10);
    Check(!state.Query(10, 1).has_value() &&
              state.Query(20, 1)->title == "connection B",
          "dropping one connection erases only its state");
}

void TestCacheBounds()
{
    rt::ClientState state;
    Check(ReadyWithEmptyBatch(&state, 1, 600, 0),
          "cache-bound fixture establishes capability");

    std::uint32_t room_id = 1;
    bool accepted_all_batches = true;
    while (room_id <= rt::kMaximumRoomsPerConnection) {
        std::vector<rm::SnapshotRecord> rooms;
        for (std::size_t index = 0;
             index < rm::kMaximumSnapshotBatchEntries &&
             room_id <= rt::kMaximumRoomsPerConnection;
             ++index, ++room_id) {
            rooms.push_back(
                Record(600, room_id, room_id, 1, "bounded room"));
        }
        accepted_all_batches =
            state.AcceptBatch(1, Batch(600, 0, std::move(rooms))) &&
            accepted_all_batches;
    }
    Check(accepted_all_batches &&
              state.room_count(1) == rt::kMaximumRoomsPerConnection,
          "room cache accepts batches through explicit 2,048 entry bound");
    Check(!state.AcceptBatch(
              1, Batch(600, 0,
                       {Record(600, 5000, 5000, 1, "overflow")})) &&
              state.LastError(1).code == rt::ClientError::CacheLimit &&
              state.room_count(1) == rt::kMaximumRoomsPerConnection &&
              !state.Query(1, 5000).has_value(),
          "cache overflow is explicit and leaves cache unchanged");
}

void TestConnectionBound()
{
    rt::ClientState state;
    bool accepted_all_connections = true;
    for (std::uint32_t connection = 1;
         connection <= rt::kMaximumClientConnections; ++connection) {
        accepted_all_connections =
            state.BeginHello(connection, connection).request_id != 0 &&
            accepted_all_connections;
    }
    Check(accepted_all_connections &&
              state.connection_count() == rt::kMaximumClientConnections,
          "connection cache accepts all 32 in-range connections");
    Check(state.BeginHello(1000, 1000).request_id == 0 &&
              state.connection_count() == rt::kMaximumClientConnections &&
              state.LastError(1000).code == rt::ClientError::ConnectionLimit,
          "connection cache rejects entry above fixed bound");
}

}  // namespace

int main()
{
    TestCapabilityAndEpoch();
    TestPendingAndAcknowledgement();
    TestLegacyRoomUpgradeAndFallback();
    TestFailuresPreserveDraft();
    TestStaleReuseAndDestroy();
    TestConnectionIsolation();
    TestCacheBounds();
    TestConnectionBound();
    std::printf("%s: %d failure(s)\n",
                g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
