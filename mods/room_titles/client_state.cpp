#include "client_state.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace sdgo {
namespace room_titles {
namespace {

Message EmptyMessage()
{
    return Message();
}

SnapshotRecord RecordFromMessage(const Message& message)
{
    SnapshotRecord record;
    record.key.server_epoch = message.server_epoch;
    record.key.room_id = message.room_id;
    record.key.generation = message.generation;
    record.revision = message.revision;
    record.title = message.title;
    return record;
}

}  // namespace

ClientState::Connection* ClientState::GetOrCreate(ConnectionId connection)
{
    const auto found = connections_.find(connection);
    if (found != connections_.end())
        return &found->second;
    if (connections_.size() >= kMaximumClientConnections) {
        global_error_ = Error(ClientError::ConnectionLimit);
        return nullptr;
    }
    const auto inserted = connections_.emplace(connection, Connection());
    return &inserted.first->second;
}

ClientState::Connection* ClientState::Find(ConnectionId connection)
{
    const auto found = connections_.find(connection);
    return found == connections_.end() ? nullptr : &found->second;
}

const ClientState::Connection* ClientState::Find(ConnectionId connection) const
{
    const auto found = connections_.find(connection);
    return found == connections_.end() ? nullptr : &found->second;
}

std::uint32_t ClientState::AllocateRequestId()
{
    if (next_request_id_ >
        std::numeric_limits<std::uint32_t>::max()) {
        global_error_ = Error(ClientError::RequestIdExhausted);
        return 0;
    }
    const std::uint32_t result =
        static_cast<std::uint32_t>(next_request_id_);
    ++next_request_id_;
    return result;
}

bool ClientState::Expired(std::uint64_t started_at,
                          std::uint64_t now,
                          std::uint64_t timeout)
{
    return now >= started_at && now - started_at >= timeout;
}

ClientErrorInfo ClientState::Error(ClientError code,
                                   std::uint32_t request_id,
                                   RemoteError remote)
{
    ClientErrorInfo result;
    result.code = code;
    result.remote_error = remote;
    result.request_id = request_id;
    return result;
}

void ClientState::SetError(Connection* connection,
                           const ClientErrorInfo& error)
{
    if (connection)
        connection->last_error = error;
    else
        global_error_ = error;
}

void ClientState::InvalidateEpoch(Connection* connection)
{
    if (!connection) return;
    connection->ready = false;
    connection->server_epoch = 0;
    connection->last_response_id = 0;
    connection->hello.reset();
    connection->rooms.clear();
    if (connection->pending) {
        connection->pending->status = PendingStatus::Failed;
        connection->pending->error =
            Error(ClientError::EpochChanged,
                  connection->pending->request.request_id);
    }
    connection->last_error = Error(ClientError::EpochChanged);
}

bool ClientState::AcceptResponseId(Connection* connection,
                                   std::uint32_t request_id)
{
    if (request_id == 0)
        return true;
    if (request_id <= connection->last_response_id) {
        SetError(connection, Error(ClientError::StaleResponse, request_id));
        return false;
    }
    const bool matches_hello =
        connection->hello &&
        connection->hello->request_id == request_id;
    const bool matches_pending =
        connection->pending &&
        connection->pending->request.request_id == request_id;
    if (!matches_hello && !matches_pending) {
        SetError(connection,
                 Error(ClientError::MismatchedResponse, request_id));
        return false;
    }
    return true;
}

bool ClientState::ApplyRecords(Connection* connection,
                               const SnapshotRecord* records,
                               std::size_t count)
{
    if (!records && count != 0) {
        SetError(connection, Error(ClientError::ProtocolError));
        return false;
    }

    std::size_t new_rooms = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const SnapshotRecord& incoming = records[index];
        if (incoming.key.server_epoch != connection->server_epoch ||
            incoming.key.room_id == 0 || incoming.key.generation == 0 ||
            incoming.revision == 0 ||
            !room_metadata::ValidateTitle(incoming.title.data(),
                                          incoming.title.size()).ok()) {
            SetError(connection, Error(ClientError::ProtocolError));
            return false;
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (records[previous].key.room_id == incoming.key.room_id) {
                SetError(connection, Error(ClientError::ProtocolError));
                return false;
            }
        }

        const auto found = connection->rooms.find(incoming.key.room_id);
        if (found == connection->rooms.end()) {
            ++new_rooms;
            continue;
        }
        const SnapshotRecord& current = found->second;
        if (incoming.key.generation < current.key.generation ||
            (incoming.key.generation == current.key.generation &&
             incoming.revision < current.revision)) {
            SetError(connection, Error(ClientError::StaleResponse));
            return false;
        }
        if (incoming.key.generation == current.key.generation &&
            incoming.revision == current.revision &&
            incoming.title != current.title) {
            SetError(connection, Error(ClientError::ProtocolError));
            return false;
        }
    }

    if (new_rooms > kMaximumRoomsPerConnection - connection->rooms.size()) {
        SetError(connection, Error(ClientError::CacheLimit));
        return false;
    }

    auto staged = connection->rooms;
    for (std::size_t index = 0; index < count; ++index) {
        const SnapshotRecord& incoming = records[index];
        const auto found = staged.find(incoming.key.room_id);
        if (found != staged.end() &&
            incoming.key.generation == found->second.key.generation &&
            incoming.revision == found->second.revision)
            continue;
        staged[incoming.key.room_id] = incoming;
    }
    connection->rooms.swap(staged);
    return true;
}

bool ClientState::PendingResponseMatches(Connection* connection,
                                         std::uint32_t response_id,
                                         const SnapshotRecord* records,
                                         std::size_t count,
                                         bool* completes)
{
    *completes = false;
    if (!connection->pending ||
        connection->pending->status != PendingStatus::Awaiting ||
        connection->pending->request.request_id != response_id)
        return true;

    const Message& request = connection->pending->request;
    std::size_t matches = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const SnapshotRecord& record = records[index];
        if (record.title != request.title)
            continue;
        if (request.room_id == 0) {
            if (record.key.server_epoch == connection->server_epoch)
                ++matches;
            continue;
        }
        if (record.key.server_epoch == request.server_epoch &&
            record.key.room_id == request.room_id &&
            record.key.generation == request.generation &&
            record.revision > request.revision)
            ++matches;
    }

    if (matches == 1) {
        *completes = true;
        return true;
    }

    connection->pending->status = PendingStatus::Failed;
    connection->pending->error =
        Error(ClientError::MismatchedResponse, response_id);
    SetError(connection, connection->pending->error);
    return false;
}

Message ClientState::BeginHello(ConnectionId connection_id,
                                std::uint64_t now_ms)
{
    Connection* connection = GetOrCreate(connection_id);
    if (!connection)
        return EmptyMessage();
    AdvanceTime(connection_id, now_ms);

    if (connection->pending &&
        connection->pending->status == PendingStatus::Awaiting) {
        SetError(connection,
                 Error(ClientError::PendingExists,
                       connection->pending->request.request_id));
        return EmptyMessage();
    }
    if (connection->hello) {
        Message hello;
        hello.kind = room_metadata::MessageKind::Hello;
        hello.request_id = connection->hello->request_id;
        return hello;
    }

    const std::uint32_t request_id = AllocateRequestId();
    if (request_id == 0) {
        SetError(connection, Error(ClientError::RequestIdExhausted));
        return EmptyMessage();
    }

    HelloOffer offer;
    offer.request_id = request_id;
    offer.started_at_ms = now_ms;
    connection->hello = offer;
    connection->last_error = ClientErrorInfo();

    Message hello;
    hello.kind = room_metadata::MessageKind::Hello;
    hello.request_id = request_id;
    return hello;
}

bool ClientState::AcceptBatch(ConnectionId connection_id,
                              const SnapshotBatch& batch)
{
    Connection* connection = Find(connection_id);
    if (!connection) {
        global_error_ = Error(ClientError::NotReady, batch.request_id);
        return false;
    }
    if (room_metadata::ValidateSnapshotBatch(batch) !=
        room_metadata::MessageError::None) {
        SetError(connection, Error(ClientError::ProtocolError,
                                   batch.request_id));
        return false;
    }

    if (!connection->ready) {
        if (!connection->hello || batch.request_id == 0 ||
            batch.request_id != connection->hello->request_id) {
            SetError(connection, Error(ClientError::MismatchedResponse,
                                       batch.request_id));
            return false;
        }

        connection->server_epoch = batch.server_epoch;
        if (!ApplyRecords(connection, batch.rooms.data(), batch.rooms.size())) {
            connection->server_epoch = 0;
            return false;
        }
        connection->ready = true;
        connection->last_response_id = batch.request_id;
        connection->hello.reset();
        connection->last_error = ClientErrorInfo();
        return true;
    }

    if (batch.server_epoch != connection->server_epoch) {
        const bool confirms_new_hello =
            connection->hello && batch.request_id != 0 &&
            batch.request_id == connection->hello->request_id;
        if (!confirms_new_hello) {
            InvalidateEpoch(connection);
            return false;
        }

        connection->rooms.clear();
        connection->server_epoch = batch.server_epoch;
        if (!ApplyRecords(connection, batch.rooms.data(), batch.rooms.size())) {
            InvalidateEpoch(connection);
            return false;
        }
        connection->ready = true;
        connection->last_response_id = batch.request_id;
        connection->hello.reset();
        if (connection->pending) {
            connection->pending->status = PendingStatus::Failed;
            connection->pending->error =
                Error(ClientError::EpochChanged,
                      connection->pending->request.request_id);
            connection->last_error = connection->pending->error;
        } else {
            connection->last_error = ClientErrorInfo();
        }
        return true;
    }

    if (!AcceptResponseId(connection, batch.request_id))
        return false;

    bool completes = false;
    if (!PendingResponseMatches(connection, batch.request_id,
                                batch.rooms.data(), batch.rooms.size(),
                                &completes))
        return false;
    if (!ApplyRecords(connection, batch.rooms.data(), batch.rooms.size()))
        return false;

    if (batch.request_id != 0)
        connection->last_response_id = batch.request_id;
    if (connection->hello &&
        connection->hello->request_id == batch.request_id)
        connection->hello.reset();
    if (completes)
        connection->pending.reset();
    if (connection->pending &&
        connection->pending->status != PendingStatus::Awaiting)
        connection->last_error = connection->pending->error;
    else
        connection->last_error = ClientErrorInfo();
    return true;
}

bool ClientState::AcceptMessage(ConnectionId connection_id,
                                const Message& message)
{
    Connection* connection = Find(connection_id);
    if (!connection) {
        global_error_ = Error(ClientError::NotReady, message.request_id);
        return false;
    }
    if (room_metadata::ValidateMessage(message) !=
        room_metadata::MessageError::None) {
        SetError(connection,
                 Error(ClientError::ProtocolError, message.request_id));
        return false;
    }

    if (message.kind == room_metadata::MessageKind::HelloAck) {
        if (!connection->hello ||
            message.request_id != connection->hello->request_id) {
            SetError(connection,
                     Error(ClientError::MismatchedResponse,
                           message.request_id));
            return false;
        }
        if (message.request_id <= connection->last_response_id) {
            SetError(connection,
                     Error(ClientError::StaleResponse, message.request_id));
            return false;
        }
        if (connection->ready &&
            message.server_epoch != connection->server_epoch) {
            connection->rooms.clear();
            if (connection->pending) {
                connection->pending->status = PendingStatus::Failed;
                connection->pending->error =
                    Error(ClientError::EpochChanged,
                          connection->pending->request.request_id);
            }
        }
        connection->ready = true;
        connection->server_epoch = message.server_epoch;
        connection->last_response_id = message.request_id;
        connection->hello.reset();
        if (connection->pending &&
            connection->pending->status != PendingStatus::Awaiting)
            connection->last_error = connection->pending->error;
        else
            connection->last_error = ClientErrorInfo();
        return true;
    }

    if (message.kind == room_metadata::MessageKind::Snapshot) {
        if (!connection->ready) {
            SetError(connection,
                     Error(ClientError::NotReady, message.request_id));
            return false;
        }
        if (message.server_epoch != connection->server_epoch) {
            InvalidateEpoch(connection);
            return false;
        }
        if (!AcceptResponseId(connection, message.request_id))
            return false;

        const SnapshotRecord record = RecordFromMessage(message);
        bool completes = false;
        if (!PendingResponseMatches(connection, message.request_id,
                                    &record, 1, &completes))
            return false;
        if (!ApplyRecords(connection, &record, 1))
            return false;

        if (message.request_id != 0)
            connection->last_response_id = message.request_id;
        if (connection->hello &&
            connection->hello->request_id == message.request_id)
            connection->hello.reset();
        if (completes)
            connection->pending.reset();
        if (connection->pending &&
            connection->pending->status != PendingStatus::Awaiting)
            connection->last_error = connection->pending->error;
        else
            connection->last_error = ClientErrorInfo();
        return true;
    }

    if (message.kind == room_metadata::MessageKind::Error) {
        if (connection->ready && message.server_epoch != 0 &&
            message.server_epoch != connection->server_epoch) {
            InvalidateEpoch(connection);
            return false;
        }
        if (!AcceptResponseId(connection, message.request_id))
            return false;

        if (connection->pending &&
            connection->pending->request.request_id == message.request_id) {
            connection->pending->status = PendingStatus::Failed;
            connection->pending->error =
                Error(ClientError::ServerRejected, message.request_id,
                      message.error);
            connection->last_error = connection->pending->error;

            if (message.error == RemoteError::RoomNotFound &&
                message.room_id != 0 && message.generation != 0) {
                const auto found = connection->rooms.find(message.room_id);
                if (found != connection->rooms.end() &&
                    found->second.key.generation == message.generation)
                    connection->rooms.erase(found);
            }
        } else if (connection->hello &&
                   connection->hello->request_id == message.request_id) {
            connection->hello.reset();
            connection->ready = false;
            connection->server_epoch = 0;
            connection->rooms.clear();
            connection->last_error =
                Error(ClientError::ServerRejected, message.request_id,
                      message.error);
        }
        connection->last_response_id = message.request_id;
        return true;
    }

    SetError(connection,
             Error(ClientError::ProtocolError, message.request_id));
    return false;
}

Message ClientState::BeginSetTitle(ConnectionId connection_id,
                                   std::uint32_t room_id,
                                   const std::string& title,
                                   std::uint64_t now_ms)
{
    Connection* connection = Find(connection_id);
    if (!connection) {
        global_error_ = Error(ClientError::NotReady);
        return EmptyMessage();
    }
    AdvanceTime(connection_id, now_ms);
    if (!connection->ready) {
        SetError(connection, Error(ClientError::NotReady));
        return EmptyMessage();
    }
    if (connection->pending) {
        SetError(connection,
                 Error(ClientError::PendingExists,
                       connection->pending->request.request_id));
        return EmptyMessage();
    }
    if (connection->hello) {
        SetError(connection, Error(ClientError::NotReady,
                                   connection->hello->request_id));
        return EmptyMessage();
    }
    if (title.empty() ||
        !room_metadata::ValidateTitle(title.data(), title.size()).ok()) {
        SetError(connection, Error(ClientError::InvalidTitle));
        return EmptyMessage();
    }

    const std::uint32_t request_id = AllocateRequestId();
    if (request_id == 0) {
        SetError(connection, Error(ClientError::RequestIdExhausted));
        return EmptyMessage();
    }

    Message request;
    request.kind = room_metadata::MessageKind::SetTitle;
    request.request_id = request_id;
    request.title = title;
    if (room_id != 0) {
        const auto found = connection->rooms.find(room_id);
        if (found == connection->rooms.end()) {
            SetError(connection,
                     Error(ClientError::RoomNotFound, request_id));
            return EmptyMessage();
        }
        request.server_epoch = found->second.key.server_epoch;
        request.room_id = found->second.key.room_id;
        request.generation = found->second.key.generation;
        request.revision = found->second.revision;
    }

    if (room_metadata::ValidateMessage(request) !=
        room_metadata::MessageError::None) {
        SetError(connection,
                 Error(ClientError::ProtocolError, request_id));
        return EmptyMessage();
    }

    PendingDraft pending;
    pending.request = request;
    pending.started_at_ms = now_ms;
    connection->pending = std::move(pending);
    connection->last_error = ClientErrorInfo();
    return request;
}

void ClientState::AdvanceTime(ConnectionId connection_id,
                              std::uint64_t now_ms)
{
    Connection* connection = Find(connection_id);
    if (!connection) return;

    if (connection->hello &&
        Expired(connection->hello->started_at_ms, now_ms,
                kHelloTimeoutMilliseconds)) {
        const std::uint32_t request_id = connection->hello->request_id;
        connection->hello.reset();
        SetError(connection, Error(ClientError::Timeout, request_id));
    }
    if (connection->pending &&
        connection->pending->status == PendingStatus::Awaiting &&
        Expired(connection->pending->started_at_ms, now_ms,
                kSetTitleTimeoutMilliseconds)) {
        connection->pending->status = PendingStatus::TimedOut;
        connection->pending->error =
            Error(ClientError::Timeout,
                  connection->pending->request.request_id);
        SetError(connection, connection->pending->error);
    }
}

bool ClientState::IsReady(ConnectionId connection_id) const
{
    const Connection* connection = Find(connection_id);
    return connection && connection->ready;
}

std::optional<SnapshotRecord> ClientState::Query(
    ConnectionId connection_id,
    std::uint32_t room_id) const
{
    const Connection* connection = Find(connection_id);
    if (!connection) return std::nullopt;
    const auto found = connection->rooms.find(room_id);
    if (found == connection->rooms.end()) return std::nullopt;
    return found->second;
}

std::optional<PendingDraft> ClientState::Pending(
    ConnectionId connection_id) const
{
    const Connection* connection = Find(connection_id);
    return connection ? connection->pending : std::nullopt;
}

bool ClientState::HasPending(ConnectionId connection_id) const
{
    const Connection* connection = Find(connection_id);
    return connection && connection->pending.has_value();
}

bool ClientState::CanStartBattle(ConnectionId connection_id) const
{
    const Connection* connection = Find(connection_id);
    return !connection || !connection->pending;
}

ClientErrorInfo ClientState::LastError(ConnectionId connection_id) const
{
    const Connection* connection = Find(connection_id);
    return connection ? connection->last_error : global_error_;
}

bool ClientState::FailPending(ConnectionId connection_id,
                              std::uint32_t request_id,
                              ClientError error)
{
    Connection* connection = Find(connection_id);
    if (!connection || !connection->pending ||
        request_id == 0 ||
        connection->pending->request.request_id != request_id ||
        error == ClientError::None ||
        error == ClientError::ServerRejected) {
        SetError(connection,
                 Error(ClientError::MismatchedResponse, request_id));
        return false;
    }

    connection->pending->status = PendingStatus::Failed;
    connection->pending->error = Error(error, request_id);
    connection->last_error = connection->pending->error;
    return true;
}

void ClientState::CancelPending(ConnectionId connection_id)
{
    Connection* connection = Find(connection_id);
    if (!connection) return;
    connection->pending.reset();
    connection->last_error = ClientErrorInfo();
}

void ClientState::DropConnection(ConnectionId connection_id)
{
    connections_.erase(connection_id);
}

std::size_t ClientState::room_count(ConnectionId connection_id) const
{
    const Connection* connection = Find(connection_id);
    return connection ? connection->rooms.size() : 0;
}

}  // namespace room_titles
}  // namespace sdgo
