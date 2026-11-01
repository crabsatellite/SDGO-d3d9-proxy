#ifndef SDGO_ROOM_TITLES_CLIENT_STATE_H
#define SDGO_ROOM_TITLES_CLIENT_STATE_H

#include "room_metadata_common.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace sdgo {
namespace room_titles {

using room_metadata::Message;
using room_metadata::RemoteError;
using room_metadata::RoomKey;
using room_metadata::SnapshotBatch;
using room_metadata::SnapshotRecord;

using ConnectionId = std::uint32_t;

constexpr std::size_t kMaximumClientConnections = 32;
constexpr std::size_t kMaximumRoomsPerConnection = 2048;
constexpr std::uint64_t kHelloTimeoutMilliseconds = 5000;
constexpr std::uint64_t kSetTitleTimeoutMilliseconds = 5000;

enum class PendingStatus {
    Awaiting,
    Failed,
    TimedOut
};

enum class ClientError {
    None,
    ConnectionLimit,
    RequestIdExhausted,
    NotReady,
    InvalidTitle,
    PendingExists,
    RoomNotFound,
    CacheLimit,
    ProtocolError,
    MismatchedResponse,
    StaleResponse,
    EpochChanged,
    ServerRejected,
    TransportError,
    Timeout
};

struct ClientErrorInfo {
    ClientError code = ClientError::None;
    RemoteError remote_error = RemoteError::None;
    std::uint32_t request_id = 0;
};

struct PendingDraft {
    PendingStatus status = PendingStatus::Awaiting;
    Message request;
    ClientErrorInfo error;
    std::uint64_t started_at_ms = 0;
};

// One owner calls this class serially. It performs no I/O, persistence, native
// packet inspection, account handling, or synchronization.
class ClientState {
public:
    Message BeginHello(ConnectionId connection, std::uint64_t now_ms);
    bool AcceptBatch(ConnectionId connection, const SnapshotBatch& batch);
    bool AcceptMessage(ConnectionId connection, const Message& message);

    // room_id == 0 creates a room and intentionally emits the common protocol's
    // keyless SetTitle shape. Existing rooms use their cached key and revision.
    Message BeginSetTitle(ConnectionId connection,
                          std::uint32_t room_id,
                          const std::string& title,
                          std::uint64_t now_ms);

    void AdvanceTime(ConnectionId connection, std::uint64_t now_ms);
    bool IsReady(ConnectionId connection) const;
    std::optional<SnapshotRecord> Query(ConnectionId connection,
                                         std::uint32_t room_id) const;
    std::optional<PendingDraft> Pending(ConnectionId connection) const;
    bool HasPending(ConnectionId connection) const;
    bool CanStartBattle(ConnectionId connection) const;
    ClientErrorInfo LastError(ConnectionId connection) const;
    bool FailPending(ConnectionId connection,
                     std::uint32_t request_id,
                     ClientError error = ClientError::TransportError);
    void CancelPending(ConnectionId connection);
    void DropConnection(ConnectionId connection);

    std::size_t connection_count() const { return connections_.size(); }
    std::size_t room_count(ConnectionId connection) const;

private:
    struct HelloOffer {
        std::uint32_t request_id = 0;
        std::uint64_t started_at_ms = 0;
    };

    struct Connection {
        bool ready = false;
        std::uint64_t server_epoch = 0;
        std::uint32_t last_response_id = 0;
        std::optional<HelloOffer> hello;
        std::optional<PendingDraft> pending;
        ClientErrorInfo last_error;
        std::unordered_map<std::uint32_t, SnapshotRecord> rooms;
    };

    Connection* GetOrCreate(ConnectionId connection);
    Connection* Find(ConnectionId connection);
    const Connection* Find(ConnectionId connection) const;
    std::uint32_t AllocateRequestId();
    static bool Expired(std::uint64_t started_at,
                        std::uint64_t now,
                        std::uint64_t timeout);
    static ClientErrorInfo Error(ClientError code,
                                 std::uint32_t request_id = 0,
                                 RemoteError remote = RemoteError::None);
    void SetError(Connection* connection, const ClientErrorInfo& error);
    void InvalidateEpoch(Connection* connection);
    bool AcceptResponseId(Connection* connection, std::uint32_t request_id);
    bool ApplyRecords(Connection* connection,
                      const SnapshotRecord* records,
                      std::size_t count);
    bool PendingResponseMatches(Connection* connection,
                                std::uint32_t response_id,
                                const SnapshotRecord* records,
                                std::size_t count,
                                bool* completes);

    std::uint64_t next_request_id_ = 1;
    ClientErrorInfo global_error_;
    std::unordered_map<ConnectionId, Connection> connections_;
};

}  // namespace room_titles
}  // namespace sdgo

#endif
