#ifndef SDGO_ROOM_METADATA_COMMON_H
#define SDGO_ROOM_METADATA_COMMON_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sdgo {
namespace room_metadata {

constexpr std::size_t kMaximumTitleScalars = 128;
constexpr std::size_t kMaximumTitleBytes = 512;
constexpr std::size_t kMaximumPacketBytes = 65535;
constexpr std::size_t kMaximumSnapshotBatchEntries = 32;
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::size_t kTrailerFooterBytes = 16;

enum class TitleError {
    None,
    NullInput,
    TooManyBytes,
    InvalidUtf8,
    EmbeddedNull,
    ControlCharacter,
    TooManyScalars
};

struct TitleValidation {
    TitleError error = TitleError::None;
    std::size_t scalar_count = 0;
    std::size_t byte_count = 0;

    bool ok() const { return error == TitleError::None; }
};

TitleValidation ValidateTitle(const char* data, std::size_t size);

enum class MessageKind : std::uint8_t {
    Hello = 1,
    HelloAck = 2,
    SetTitle = 3,
    Snapshot = 4,
    Query = 5,
    Error = 6,
    SnapshotBatch = 7
};

enum class RemoteError : std::uint16_t {
    None = 0,
    InvalidRequest = 1,
    UnsupportedVersion = 2,
    RoomNotFound = 3,
    StaleGeneration = 4,
    StaleRevision = 5,
    Unauthorized = 6,
    Conflict = 7,
    Internal = 8
};

struct Message {
    MessageKind kind = MessageKind::Hello;
    RemoteError error = RemoteError::None;
    std::uint64_t server_epoch = 0;
    std::uint32_t room_id = 0;
    std::uint32_t generation = 0;
    std::uint32_t revision = 0;
    std::uint32_t request_id = 0;
    std::string title;
};

enum class MessageError {
    None,
    NullOutput,
    BodyTooShort,
    BodyTooLong,
    UnknownKind,
    UnknownRemoteError,
    ReservedField,
    InvalidLength,
    InvalidFields,
    InvalidTitle,
    TooManyEntries,
    DuplicateRoom
};

MessageError ValidateMessage(const Message& message);
bool EncodeMessageBody(const Message& message,
                       std::vector<std::uint8_t>* output,
                       MessageError* error = nullptr);
bool DecodeMessageBody(const std::uint8_t* data,
                       std::size_t size,
                       Message* output,
                       MessageError* error = nullptr);

enum class TrailerStatus {
    NoTrailer,
    Valid,
    Invalid
};

enum class TrailerError {
    None,
    NullInput,
    NullOutput,
    PacketTooLarge,
    NativePayloadTooShort,
    EmptyBody,
    TruncatedFooter,
    UnsupportedVersion,
    ReservedField,
    InconsistentLength,
    DuplicateTrailer,
    InvalidMessageBody
};

struct TrailerView {
    const std::uint8_t* native_data = nullptr;
    std::size_t native_size = 0;
    const std::uint8_t* body_data = nullptr;
    std::size_t body_size = 0;
    std::uint16_t version = 0;
};

struct TrailerParseResult {
    TrailerStatus status = TrailerStatus::NoTrailer;
    TrailerError error = TrailerError::None;
    TrailerView view;
};

// The trailer footer is little endian and ends the packet:
// version:u16, reserved:u16, native_length:u32, body_length:u32, "RTMD".
// Only a footer with trailing magic is a candidate; malformed candidates are
// Invalid rather than silently falling back to the native packet.
TrailerParseResult ParseTrailer(const std::uint8_t* data, std::size_t size);
bool AppendTrailer(const std::uint8_t* native_data,
                   std::size_t native_size,
                   const std::uint8_t* body_data,
                   std::size_t body_size,
                   std::vector<std::uint8_t>* output,
                   TrailerError* error = nullptr);
bool EncodePacket(const std::uint8_t* native_data,
                  std::size_t native_size,
                  const Message& message,
                  std::vector<std::uint8_t>* output,
                  TrailerError* trailer_error = nullptr,
                  MessageError* message_error = nullptr);
bool DecodePacket(const std::uint8_t* data,
                  std::size_t size,
                  TrailerView* trailer,
                  Message* message,
                  TrailerError* trailer_error = nullptr,
                  MessageError* message_error = nullptr);

using SessionId = std::uint64_t;

struct RoomKey {
    std::uint64_t server_epoch = 0;
    std::uint32_t room_id = 0;
    std::uint32_t generation = 0;
};

struct SnapshotRecord {
    RoomKey key;
    std::uint32_t revision = 0;
    // Empty means identity-only metadata or clearing the extended title.
    // Consumers display the native short title while retaining key/revision.
    std::string title;
};

struct SnapshotBatch {
    std::uint64_t server_epoch = 0;
    std::uint32_t request_id = 0;
    std::vector<SnapshotRecord> rooms;
};

// A batch is one message body with at most 32 room records. It never encodes
// owner sessions and is intended for one native room-list response trailer.
// Its nonzero server epoch also makes an empty batch a capability response.
MessageError ValidateSnapshotBatch(const SnapshotBatch& batch);
bool EncodeSnapshotBatchBody(const SnapshotBatch& batch,
                             std::vector<std::uint8_t>* output,
                             MessageError* error = nullptr);
bool DecodeSnapshotBatchBody(const std::uint8_t* data,
                             std::size_t size,
                             SnapshotBatch* output,
                             MessageError* error = nullptr);
bool EncodeSnapshotBatchPacket(const std::uint8_t* native_data,
                               std::size_t native_size,
                               const SnapshotBatch& batch,
                               std::vector<std::uint8_t>* output,
                               TrailerError* trailer_error = nullptr,
                               MessageError* message_error = nullptr);
bool DecodeSnapshotBatchPacket(const std::uint8_t* data,
                               std::size_t size,
                               TrailerView* trailer,
                               SnapshotBatch* batch,
                               TrailerError* trailer_error = nullptr,
                               MessageError* message_error = nullptr);

struct RoomSnapshot {
    RoomKey key;
    std::uint32_t revision = 0;
    SessionId owner_session = 0;
    std::string title;
};

enum class StateError {
    None,
    InvalidArgument,
    InvalidTitle,
    AlreadyExists,
    NotFound,
    StaleEpoch,
    StaleGeneration,
    StaleRevision,
    Unauthorized,
    RevisionExhausted,
    GenerationExhausted
};

struct StateResult {
    StateError error = StateError::None;
    RoomSnapshot snapshot;

    bool ok() const { return error == StateError::None; }
};

class RoomStore {
public:
    explicit RoomStore(std::uint64_t server_epoch,
                       std::uint32_t first_generation = 1);

    std::uint64_t server_epoch() const { return server_epoch_; }
    std::size_t room_count() const { return rooms_.size(); }

    StateResult CreateRoom(std::uint32_t room_id,
                           SessionId owner_session,
                           const std::string& title);
    // Trusted native lifecycle observation. This creates an identity-only
    // record whose empty title means callers must fall back to native display.
    StateResult CreateLegacyRoom(std::uint32_t room_id,
                                 SessionId owner_session);
    StateResult UpdateTitle(const RoomKey& key,
                            SessionId owner_session,
                            std::uint32_t expected_revision,
                            const std::string& title);
    // Trusted native handler callback after the native short title has
    // successfully changed. Clears only the extended title.
    StateResult ClearNativeTitle(const RoomKey& key);
    StateResult TransferHost(const RoomKey& key,
                             SessionId owner_session,
                             SessionId new_owner_session,
                             std::uint32_t expected_revision);
    StateResult Query(const RoomKey& key) const;

    // Destruction is a trusted native room-lifecycle operation. Network requests
    // must not call it directly.
    StateError DestroyRoom(const RoomKey& key);

private:
    struct Entry {
        std::uint32_t generation = 0;
        std::uint32_t revision = 0;
        SessionId owner_session = 0;
        std::string title;
    };

    StateResult Find(const RoomKey& key) const;
    StateResult CreateRoomRecord(std::uint32_t room_id,
                                 SessionId owner_session,
                                 const std::string& title);
    RoomSnapshot Snapshot(std::uint32_t room_id, const Entry& entry) const;

    std::uint64_t server_epoch_;
    std::uint64_t next_generation_;
    std::unordered_map<std::uint32_t, Entry> rooms_;
};

}  // namespace room_metadata
}  // namespace sdgo

#endif
