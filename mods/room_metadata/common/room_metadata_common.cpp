#include "room_metadata_common.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace sdgo {
namespace room_metadata {
namespace {

constexpr std::uint32_t kTrailerMagic = 0x444D5452u;  // "RTMD" in little endian.
constexpr std::size_t kMessageHeaderBytes = 32;
constexpr std::size_t kBatchHeaderBytes = 16;
constexpr std::size_t kBatchRecordHeaderBytes = 24;

void SetMessageError(MessageError* output, MessageError value)
{
    if (output) *output = value;
}

void SetTrailerError(TrailerError* output, TrailerError value)
{
    if (output) *output = value;
}

std::uint16_t Read16(const std::uint8_t* data)
{
    return static_cast<std::uint16_t>(data[0]) |
           static_cast<std::uint16_t>(data[1]) << 8;
}

std::uint32_t Read32(const std::uint8_t* data)
{
    return static_cast<std::uint32_t>(data[0]) |
           static_cast<std::uint32_t>(data[1]) << 8 |
           static_cast<std::uint32_t>(data[2]) << 16 |
           static_cast<std::uint32_t>(data[3]) << 24;
}

std::uint64_t Read64(const std::uint8_t* data)
{
    return static_cast<std::uint64_t>(Read32(data)) |
           static_cast<std::uint64_t>(Read32(data + 4)) << 32;
}

void Write16(std::vector<std::uint8_t>* output, std::uint16_t value)
{
    output->push_back(static_cast<std::uint8_t>(value));
    output->push_back(static_cast<std::uint8_t>(value >> 8));
}

void Write32(std::vector<std::uint8_t>* output, std::uint32_t value)
{
    output->push_back(static_cast<std::uint8_t>(value));
    output->push_back(static_cast<std::uint8_t>(value >> 8));
    output->push_back(static_cast<std::uint8_t>(value >> 16));
    output->push_back(static_cast<std::uint8_t>(value >> 24));
}

void Write64(std::vector<std::uint8_t>* output, std::uint64_t value)
{
    Write32(output, static_cast<std::uint32_t>(value));
    Write32(output, static_cast<std::uint32_t>(value >> 32));
}

bool IsKnownKind(MessageKind kind)
{
    const auto value = static_cast<std::uint8_t>(kind);
    return value >= static_cast<std::uint8_t>(MessageKind::Hello) &&
           value <= static_cast<std::uint8_t>(MessageKind::SnapshotBatch);
}

bool IsKnownRemoteError(RemoteError error)
{
    const auto value = static_cast<std::uint16_t>(error);
    return value <= static_cast<std::uint16_t>(RemoteError::Internal);
}

bool EmptyRoomIdentity(const Message& message)
{
    return message.room_id == 0 && message.generation == 0;
}

bool CompleteRoomIdentity(const Message& message)
{
    return message.room_id != 0 && message.generation != 0;
}

bool IsControlScalar(std::uint32_t scalar)
{
    return scalar <= 0x1Fu || (scalar >= 0x7Fu && scalar <= 0x9Fu);
}

TitleValidation InvalidTitle(TitleError error,
                             std::size_t scalar_count,
                             std::size_t byte_count)
{
    TitleValidation result;
    result.error = error;
    result.scalar_count = scalar_count;
    result.byte_count = byte_count;
    return result;
}

StateResult StateFailure(StateError error)
{
    StateResult result;
    result.error = error;
    return result;
}

TrailerParseResult ParseOneTrailer(const std::uint8_t* data, std::size_t size)
{
    TrailerParseResult result;
    if (!data && size != 0) {
        result.status = TrailerStatus::Invalid;
        result.error = TrailerError::NullInput;
        return result;
    }
    if (size > kMaximumPacketBytes) {
        result.status = TrailerStatus::Invalid;
        result.error = TrailerError::PacketTooLarge;
        return result;
    }

    if (size < sizeof(std::uint32_t) ||
        Read32(data + size - sizeof(std::uint32_t)) != kTrailerMagic)
        return result;

    result.status = TrailerStatus::Invalid;
    if (size < kTrailerFooterBytes) {
        result.error = TrailerError::TruncatedFooter;
        return result;
    }

    const std::uint8_t* footer = data + size - kTrailerFooterBytes;
    const std::uint16_t version = Read16(footer);
    const std::uint16_t reserved = Read16(footer + 2);
    const std::uint32_t native_size = Read32(footer + 4);
    const std::uint32_t body_size = Read32(footer + 8);
    if (version != kProtocolVersion) {
        result.error = TrailerError::UnsupportedVersion;
        return result;
    }
    if (reserved != 0) {
        result.error = TrailerError::ReservedField;
        return result;
    }
    if (native_size < 2) {
        result.error = TrailerError::NativePayloadTooShort;
        return result;
    }
    if (body_size == 0) {
        result.error = TrailerError::EmptyBody;
        return result;
    }

    const std::uint64_t expected =
        static_cast<std::uint64_t>(native_size) + body_size + kTrailerFooterBytes;
    if (expected != size) {
        result.error = TrailerError::InconsistentLength;
        return result;
    }

    result.status = TrailerStatus::Valid;
    result.error = TrailerError::None;
    result.view.native_data = data;
    result.view.native_size = native_size;
    result.view.body_data = data + native_size;
    result.view.body_size = body_size;
    result.view.version = version;
    return result;
}

}  // namespace

TitleValidation ValidateTitle(const char* data, std::size_t size)
{
    if (!data && size != 0)
        return InvalidTitle(TitleError::NullInput, 0, size);
    if (size > kMaximumTitleBytes)
        return InvalidTitle(TitleError::TooManyBytes, 0, size);

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
    std::size_t offset = 0;
    std::size_t scalars = 0;
    while (offset < size) {
        const std::uint8_t first = bytes[offset];
        std::uint32_t scalar = 0;
        std::size_t width = 0;
        std::uint32_t minimum = 0;

        if (first <= 0x7F) {
            scalar = first;
            width = 1;
        } else if (first >= 0xC2 && first <= 0xDF) {
            scalar = first & 0x1F;
            width = 2;
            minimum = 0x80;
        } else if (first >= 0xE0 && first <= 0xEF) {
            scalar = first & 0x0F;
            width = 3;
            minimum = 0x800;
        } else if (first >= 0xF0 && first <= 0xF4) {
            scalar = first & 0x07;
            width = 4;
            minimum = 0x10000;
        } else {
            return InvalidTitle(TitleError::InvalidUtf8, scalars, size);
        }

        if (offset + width > size)
            return InvalidTitle(TitleError::InvalidUtf8, scalars, size);
        for (std::size_t index = 1; index < width; ++index) {
            const std::uint8_t continuation = bytes[offset + index];
            if ((continuation & 0xC0) != 0x80)
                return InvalidTitle(TitleError::InvalidUtf8, scalars, size);
            scalar = (scalar << 6) | (continuation & 0x3F);
        }

        if ((width > 1 && scalar < minimum) ||
            scalar > 0x10FFFFu ||
            (scalar >= 0xD800u && scalar <= 0xDFFFu))
            return InvalidTitle(TitleError::InvalidUtf8, scalars, size);
        if (scalar == 0)
            return InvalidTitle(TitleError::EmbeddedNull, scalars, size);
        if (IsControlScalar(scalar))
            return InvalidTitle(TitleError::ControlCharacter, scalars, size);

        ++scalars;
        if (scalars > kMaximumTitleScalars)
            return InvalidTitle(TitleError::TooManyScalars, scalars, size);
        offset += width;
    }

    TitleValidation result;
    result.scalar_count = scalars;
    result.byte_count = size;
    return result;
}

MessageError ValidateMessage(const Message& message)
{
    if (!IsKnownKind(message.kind))
        return MessageError::UnknownKind;
    if (!IsKnownRemoteError(message.error))
        return MessageError::UnknownRemoteError;
    if (!ValidateTitle(message.title.data(), message.title.size()).ok())
        return MessageError::InvalidTitle;

    const bool no_error = message.error == RemoteError::None;
    switch (message.kind) {
    case MessageKind::Hello:
        if (!no_error || message.server_epoch != 0 || !EmptyRoomIdentity(message) ||
            message.revision != 0 || message.request_id == 0 || !message.title.empty())
            return MessageError::InvalidFields;
        break;
    case MessageKind::HelloAck:
        if (!no_error || message.server_epoch == 0 || !EmptyRoomIdentity(message) ||
            message.revision != 0 || message.request_id == 0 || !message.title.empty())
            return MessageError::InvalidFields;
        break;
    case MessageKind::SetTitle:
    {
        // A keyless SetTitle is accepted only as a wire shape. The native
        // wrapper must restrict it to an authenticated room-creation packet.
        const bool creating = message.server_epoch == 0 &&
                              EmptyRoomIdentity(message) &&
                              message.revision == 0;
        const bool existing = message.server_epoch != 0 &&
                              CompleteRoomIdentity(message);
        if (!no_error || !(creating || existing) ||
            message.request_id == 0 || message.title.empty())
            return MessageError::InvalidFields;
        break;
    }
    case MessageKind::Snapshot:
        if (!no_error || message.server_epoch == 0 || !CompleteRoomIdentity(message) ||
            message.revision == 0)
            return MessageError::InvalidFields;
        break;
    case MessageKind::Query:
        if (!no_error || message.server_epoch == 0 || !CompleteRoomIdentity(message) ||
            message.request_id == 0 || !message.title.empty())
            return MessageError::InvalidFields;
        break;
    case MessageKind::Error:
        if (no_error || message.request_id == 0 || !message.title.empty() ||
            !(EmptyRoomIdentity(message) || CompleteRoomIdentity(message)))
            return MessageError::InvalidFields;
        break;
    case MessageKind::SnapshotBatch:
        return MessageError::InvalidFields;
    }
    return MessageError::None;
}

bool EncodeMessageBody(const Message& message,
                       std::vector<std::uint8_t>* output,
                       MessageError* error)
{
    if (!output) {
        SetMessageError(error, MessageError::NullOutput);
        return false;
    }
    const MessageError validation = ValidateMessage(message);
    if (validation != MessageError::None) {
        SetMessageError(error, validation);
        return false;
    }

    std::vector<std::uint8_t> encoded;
    encoded.reserve(kMessageHeaderBytes + message.title.size());
    encoded.push_back(static_cast<std::uint8_t>(message.kind));
    encoded.push_back(0);
    Write16(&encoded, static_cast<std::uint16_t>(message.error));
    Write64(&encoded, message.server_epoch);
    Write32(&encoded, message.room_id);
    Write32(&encoded, message.generation);
    Write32(&encoded, message.revision);
    Write32(&encoded, message.request_id);
    Write16(&encoded, static_cast<std::uint16_t>(message.title.size()));
    Write16(&encoded, 0);
    encoded.insert(encoded.end(), message.title.begin(), message.title.end());
    output->swap(encoded);
    SetMessageError(error, MessageError::None);
    return true;
}

bool DecodeMessageBody(const std::uint8_t* data,
                       std::size_t size,
                       Message* output,
                       MessageError* error)
{
    if (!output) {
        SetMessageError(error, MessageError::NullOutput);
        return false;
    }
    if (!data && size != 0) {
        SetMessageError(error, MessageError::BodyTooShort);
        return false;
    }
    if (size < kMessageHeaderBytes) {
        SetMessageError(error, MessageError::BodyTooShort);
        return false;
    }
    if (size > kMessageHeaderBytes + kMaximumTitleBytes) {
        SetMessageError(error, MessageError::BodyTooLong);
        return false;
    }
    if (data[1] != 0 || Read16(data + 30) != 0) {
        SetMessageError(error, MessageError::ReservedField);
        return false;
    }

    const std::uint16_t title_size = Read16(data + 28);
    if (static_cast<std::size_t>(title_size) != size - kMessageHeaderBytes) {
        SetMessageError(error, MessageError::InvalidLength);
        return false;
    }

    Message decoded;
    decoded.kind = static_cast<MessageKind>(data[0]);
    decoded.error = static_cast<RemoteError>(Read16(data + 2));
    decoded.server_epoch = Read64(data + 4);
    decoded.room_id = Read32(data + 12);
    decoded.generation = Read32(data + 16);
    decoded.revision = Read32(data + 20);
    decoded.request_id = Read32(data + 24);
    decoded.title.assign(reinterpret_cast<const char*>(data + kMessageHeaderBytes),
                         title_size);

    const MessageError validation = ValidateMessage(decoded);
    if (validation != MessageError::None) {
        SetMessageError(error, validation);
        return false;
    }
    *output = std::move(decoded);
    SetMessageError(error, MessageError::None);
    return true;
}

TrailerParseResult ParseTrailer(const std::uint8_t* data, std::size_t size)
{
    TrailerParseResult result = ParseOneTrailer(data, size);
    if (result.status != TrailerStatus::Valid)
        return result;

    // A packet may carry exactly one trailer. Check the declared native prefix
    // once, without recursively walking attacker-controlled nesting. Treat any
    // trailing-magic candidate as a duplicate, including a malformed one; this
    // is the same conservative rule AppendTrailer applies to native input.
    const TrailerParseResult nested =
        ParseOneTrailer(data, result.view.native_size);
    if (nested.status != TrailerStatus::NoTrailer) {
        result.view = TrailerView();
        result.status = TrailerStatus::Invalid;
        result.error = TrailerError::DuplicateTrailer;
    }
    return result;
}

bool AppendTrailer(const std::uint8_t* native_data,
                   std::size_t native_size,
                   const std::uint8_t* body_data,
                   std::size_t body_size,
                   std::vector<std::uint8_t>* output,
                   TrailerError* error)
{
    if (!output) {
        SetTrailerError(error, TrailerError::NullOutput);
        return false;
    }
    if ((!native_data && native_size != 0) || (!body_data && body_size != 0)) {
        SetTrailerError(error, TrailerError::NullInput);
        return false;
    }
    if (native_size < 2) {
        SetTrailerError(error, TrailerError::NativePayloadTooShort);
        return false;
    }
    if (body_size == 0) {
        SetTrailerError(error, TrailerError::EmptyBody);
        return false;
    }
    const std::uint64_t total_size =
        static_cast<std::uint64_t>(native_size) + body_size + kTrailerFooterBytes;
    if (native_size > std::numeric_limits<std::uint32_t>::max() ||
        body_size > std::numeric_limits<std::uint32_t>::max() ||
        total_size > kMaximumPacketBytes) {
        SetTrailerError(error, TrailerError::PacketTooLarge);
        return false;
    }

    const TrailerParseResult existing = ParseTrailer(native_data, native_size);
    if (existing.status != TrailerStatus::NoTrailer) {
        SetTrailerError(error, TrailerError::DuplicateTrailer);
        return false;
    }

    std::vector<std::uint8_t> packet;
    packet.reserve(native_size + body_size + kTrailerFooterBytes);
    packet.insert(packet.end(), native_data, native_data + native_size);
    packet.insert(packet.end(), body_data, body_data + body_size);
    Write16(&packet, kProtocolVersion);
    Write16(&packet, 0);
    Write32(&packet, static_cast<std::uint32_t>(native_size));
    Write32(&packet, static_cast<std::uint32_t>(body_size));
    Write32(&packet, kTrailerMagic);
    output->swap(packet);
    SetTrailerError(error, TrailerError::None);
    return true;
}

bool EncodePacket(const std::uint8_t* native_data,
                  std::size_t native_size,
                  const Message& message,
                  std::vector<std::uint8_t>* output,
                  TrailerError* trailer_error,
                  MessageError* message_error)
{
    std::vector<std::uint8_t> body;
    if (!EncodeMessageBody(message, &body, message_error))
        return false;
    return AppendTrailer(native_data, native_size, body.data(), body.size(),
                         output, trailer_error);
}

bool DecodePacket(const std::uint8_t* data,
                  std::size_t size,
                  TrailerView* trailer,
                  Message* message,
                  TrailerError* trailer_error,
                  MessageError* message_error)
{
    if (!trailer || !message) {
        SetTrailerError(trailer_error, TrailerError::NullOutput);
        return false;
    }
    const TrailerParseResult parsed = ParseTrailer(data, size);
    if (parsed.status != TrailerStatus::Valid) {
        SetTrailerError(trailer_error, parsed.error);
        return false;
    }

    Message decoded;
    if (!DecodeMessageBody(parsed.view.body_data, parsed.view.body_size,
                           &decoded, message_error)) {
        SetTrailerError(trailer_error, TrailerError::InvalidMessageBody);
        return false;
    }
    *trailer = parsed.view;
    *message = std::move(decoded);
    SetTrailerError(trailer_error, TrailerError::None);
    return true;
}

MessageError ValidateSnapshotBatch(const SnapshotBatch& batch)
{
    if (batch.server_epoch == 0)
        return MessageError::InvalidFields;
    if (batch.rooms.size() > kMaximumSnapshotBatchEntries)
        return MessageError::TooManyEntries;

    for (std::size_t index = 0; index < batch.rooms.size(); ++index) {
        const SnapshotRecord& room = batch.rooms[index];
        if (room.key.server_epoch != batch.server_epoch || room.key.room_id == 0 ||
            room.key.generation == 0 || room.revision == 0)
            return MessageError::InvalidFields;
        if (!ValidateTitle(room.title.data(), room.title.size()).ok())
            return MessageError::InvalidTitle;
        for (std::size_t previous = 0; previous < index; ++previous) {
            const RoomKey& other = batch.rooms[previous].key;
            if (other.server_epoch == room.key.server_epoch &&
                other.room_id == room.key.room_id &&
                other.generation == room.key.generation)
                return MessageError::DuplicateRoom;
        }
    }
    return MessageError::None;
}

bool EncodeSnapshotBatchBody(const SnapshotBatch& batch,
                             std::vector<std::uint8_t>* output,
                             MessageError* error)
{
    if (!output) {
        SetMessageError(error, MessageError::NullOutput);
        return false;
    }
    const MessageError validation = ValidateSnapshotBatch(batch);
    if (validation != MessageError::None) {
        SetMessageError(error, validation);
        return false;
    }

    std::size_t body_size = kBatchHeaderBytes;
    for (const SnapshotRecord& room : batch.rooms)
        body_size += kBatchRecordHeaderBytes + room.title.size();

    std::vector<std::uint8_t> encoded;
    encoded.reserve(body_size);
    encoded.push_back(static_cast<std::uint8_t>(MessageKind::SnapshotBatch));
    encoded.push_back(0);
    Write16(&encoded, static_cast<std::uint16_t>(batch.rooms.size()));
    Write32(&encoded, batch.request_id);
    Write64(&encoded, batch.server_epoch);
    for (const SnapshotRecord& room : batch.rooms) {
        Write64(&encoded, room.key.server_epoch);
        Write32(&encoded, room.key.room_id);
        Write32(&encoded, room.key.generation);
        Write32(&encoded, room.revision);
        Write16(&encoded, static_cast<std::uint16_t>(room.title.size()));
        Write16(&encoded, 0);
        encoded.insert(encoded.end(), room.title.begin(), room.title.end());
    }
    output->swap(encoded);
    SetMessageError(error, MessageError::None);
    return true;
}

bool DecodeSnapshotBatchBody(const std::uint8_t* data,
                             std::size_t size,
                             SnapshotBatch* output,
                             MessageError* error)
{
    if (!output) {
        SetMessageError(error, MessageError::NullOutput);
        return false;
    }
    if (!data && size != 0) {
        SetMessageError(error, MessageError::BodyTooShort);
        return false;
    }
    if (size < kBatchHeaderBytes) {
        SetMessageError(error, MessageError::BodyTooShort);
        return false;
    }
    if (data[0] != static_cast<std::uint8_t>(MessageKind::SnapshotBatch)) {
        SetMessageError(error, MessageError::UnknownKind);
        return false;
    }
    if (data[1] != 0) {
        SetMessageError(error, MessageError::ReservedField);
        return false;
    }

    const std::uint16_t count = Read16(data + 2);
    if (count > kMaximumSnapshotBatchEntries) {
        SetMessageError(error, MessageError::TooManyEntries);
        return false;
    }

    SnapshotBatch decoded;
    decoded.request_id = Read32(data + 4);
    decoded.server_epoch = Read64(data + 8);
    decoded.rooms.reserve(count);
    std::size_t offset = kBatchHeaderBytes;
    for (std::uint16_t index = 0; index < count; ++index) {
        if (size - offset < kBatchRecordHeaderBytes) {
            SetMessageError(error, MessageError::BodyTooShort);
            return false;
        }

        SnapshotRecord room;
        room.key.server_epoch = Read64(data + offset);
        room.key.room_id = Read32(data + offset + 8);
        room.key.generation = Read32(data + offset + 12);
        room.revision = Read32(data + offset + 16);
        const std::uint16_t title_size = Read16(data + offset + 20);
        if (Read16(data + offset + 22) != 0) {
            SetMessageError(error, MessageError::ReservedField);
            return false;
        }
        offset += kBatchRecordHeaderBytes;
        if (title_size > size - offset) {
            SetMessageError(error, MessageError::InvalidLength);
            return false;
        }
        room.title.assign(reinterpret_cast<const char*>(data + offset), title_size);
        offset += title_size;
        decoded.rooms.push_back(std::move(room));
    }
    if (offset != size) {
        SetMessageError(error, MessageError::InvalidLength);
        return false;
    }

    const MessageError validation = ValidateSnapshotBatch(decoded);
    if (validation != MessageError::None) {
        SetMessageError(error, validation);
        return false;
    }
    *output = std::move(decoded);
    SetMessageError(error, MessageError::None);
    return true;
}

bool EncodeSnapshotBatchPacket(const std::uint8_t* native_data,
                               std::size_t native_size,
                               const SnapshotBatch& batch,
                               std::vector<std::uint8_t>* output,
                               TrailerError* trailer_error,
                               MessageError* message_error)
{
    std::vector<std::uint8_t> body;
    if (!EncodeSnapshotBatchBody(batch, &body, message_error))
        return false;
    return AppendTrailer(native_data, native_size, body.data(), body.size(),
                         output, trailer_error);
}

bool DecodeSnapshotBatchPacket(const std::uint8_t* data,
                               std::size_t size,
                               TrailerView* trailer,
                               SnapshotBatch* batch,
                               TrailerError* trailer_error,
                               MessageError* message_error)
{
    if (!trailer || !batch) {
        SetTrailerError(trailer_error, TrailerError::NullOutput);
        return false;
    }
    const TrailerParseResult parsed = ParseTrailer(data, size);
    if (parsed.status != TrailerStatus::Valid) {
        SetTrailerError(trailer_error, parsed.error);
        return false;
    }

    SnapshotBatch decoded;
    if (!DecodeSnapshotBatchBody(parsed.view.body_data, parsed.view.body_size,
                                 &decoded, message_error)) {
        SetTrailerError(trailer_error, TrailerError::InvalidMessageBody);
        return false;
    }
    *trailer = parsed.view;
    *batch = std::move(decoded);
    SetTrailerError(trailer_error, TrailerError::None);
    return true;
}

RoomStore::RoomStore(std::uint64_t server_epoch, std::uint32_t first_generation)
    : server_epoch_(server_epoch),
      next_generation_(first_generation)
{
}

RoomSnapshot RoomStore::Snapshot(std::uint32_t room_id, const Entry& entry) const
{
    RoomSnapshot snapshot;
    snapshot.key.server_epoch = server_epoch_;
    snapshot.key.room_id = room_id;
    snapshot.key.generation = entry.generation;
    snapshot.revision = entry.revision;
    snapshot.owner_session = entry.owner_session;
    snapshot.title = entry.title;
    return snapshot;
}

StateResult RoomStore::Find(const RoomKey& key) const
{
    if (key.server_epoch != server_epoch_)
        return StateFailure(StateError::StaleEpoch);
    const auto found = rooms_.find(key.room_id);
    if (found == rooms_.end())
        return StateFailure(StateError::NotFound);
    if (found->second.generation != key.generation)
        return StateFailure(StateError::StaleGeneration);
    StateResult result;
    result.snapshot = Snapshot(found->first, found->second);
    return result;
}

StateResult RoomStore::CreateRoom(std::uint32_t room_id,
                                  SessionId owner_session,
                                  const std::string& title)
{
    if (server_epoch_ == 0 || room_id == 0 || owner_session == 0 || title.empty())
        return StateFailure(StateError::InvalidArgument);
    if (!ValidateTitle(title.data(), title.size()).ok())
        return StateFailure(StateError::InvalidTitle);
    return CreateRoomRecord(room_id, owner_session, title);
}

StateResult RoomStore::CreateLegacyRoom(std::uint32_t room_id,
                                        SessionId owner_session)
{
    if (server_epoch_ == 0 || room_id == 0 || owner_session == 0)
        return StateFailure(StateError::InvalidArgument);
    return CreateRoomRecord(room_id, owner_session, std::string());
}

StateResult RoomStore::CreateRoomRecord(std::uint32_t room_id,
                                        SessionId owner_session,
                                        const std::string& title)
{
    if (rooms_.find(room_id) != rooms_.end())
        return StateFailure(StateError::AlreadyExists);
    if (next_generation_ == 0 ||
        next_generation_ > std::numeric_limits<std::uint32_t>::max())
        return StateFailure(StateError::GenerationExhausted);

    Entry entry;
    entry.generation = static_cast<std::uint32_t>(next_generation_);
    entry.revision = 1;
    entry.owner_session = owner_session;
    entry.title = title;
    StateResult result;
    // Prepare every allocating result before publishing the new entry. A
    // failed allocation must not consume an ID or leave an unacknowledged room.
    result.snapshot = Snapshot(room_id, entry);
    const auto inserted = rooms_.emplace(room_id, std::move(entry));
    if (!inserted.second)
        return StateFailure(StateError::AlreadyExists);
    ++next_generation_;
    return result;
}

StateResult RoomStore::UpdateTitle(const RoomKey& key,
                                   SessionId owner_session,
                                   std::uint32_t expected_revision,
                                   const std::string& title)
{
    if (owner_session == 0 || title.empty())
        return StateFailure(StateError::InvalidArgument);
    if (!ValidateTitle(title.data(), title.size()).ok())
        return StateFailure(StateError::InvalidTitle);

    StateResult current = Find(key);
    if (!current.ok())
        return current;
    Entry& entry = rooms_.find(key.room_id)->second;
    if (entry.owner_session != owner_session)
        return StateFailure(StateError::Unauthorized);
    if (entry.revision != expected_revision)
        return StateFailure(StateError::StaleRevision);
    if (entry.revision == std::numeric_limits<std::uint32_t>::max())
        return StateFailure(StateError::RevisionExhausted);

    // Both the stored title and returned snapshot may allocate. Finish those
    // copies before the no-throw swap and revision commit.
    std::string replacement(title);
    current.snapshot.title = title;
    ++current.snapshot.revision;
    entry.title.swap(replacement);
    entry.revision = current.snapshot.revision;
    return current;
}

StateResult RoomStore::ClearNativeTitle(const RoomKey& key)
{
    if (key.server_epoch != server_epoch_)
        return StateFailure(StateError::StaleEpoch);
    const auto found = rooms_.find(key.room_id);
    if (found == rooms_.end())
        return StateFailure(StateError::NotFound);
    Entry& entry = found->second;
    if (entry.generation != key.generation)
        return StateFailure(StateError::StaleGeneration);

    StateResult result;
    result.snapshot.key.server_epoch = server_epoch_;
    result.snapshot.key.room_id = key.room_id;
    result.snapshot.key.generation = entry.generation;
    result.snapshot.owner_session = entry.owner_session;

    if (entry.title.empty()) {
        result.snapshot.revision = entry.revision;
        return result;
    }
    if (entry.revision == std::numeric_limits<std::uint32_t>::max())
        return StateFailure(StateError::RevisionExhausted);

    // The native title has already changed. Prepare an identity-only result
    // without copying the obsolete extended title, then commit with operations
    // that cannot allocate or throw.
    result.snapshot.revision = entry.revision + 1;
    entry.title.clear();
    entry.revision = result.snapshot.revision;
    return result;
}

StateResult RoomStore::TransferHost(const RoomKey& key,
                                    SessionId owner_session,
                                    SessionId new_owner_session,
                                    std::uint32_t expected_revision)
{
    if (owner_session == 0 || new_owner_session == 0)
        return StateFailure(StateError::InvalidArgument);

    StateResult current = Find(key);
    if (!current.ok())
        return current;
    Entry& entry = rooms_.find(key.room_id)->second;
    if (entry.owner_session != owner_session)
        return StateFailure(StateError::Unauthorized);
    if (entry.revision != expected_revision)
        return StateFailure(StateError::StaleRevision);
    if (entry.revision == std::numeric_limits<std::uint32_t>::max())
        return StateFailure(StateError::RevisionExhausted);

    ++current.snapshot.revision;
    current.snapshot.owner_session = new_owner_session;
    entry.revision = current.snapshot.revision;
    entry.owner_session = new_owner_session;
    return current;
}

StateResult RoomStore::Query(const RoomKey& key) const
{
    return Find(key);
}

StateError RoomStore::DestroyRoom(const RoomKey& key)
{
    if (key.server_epoch != server_epoch_)
        return StateError::StaleEpoch;
    const auto found = rooms_.find(key.room_id);
    if (found == rooms_.end())
        return StateError::NotFound;
    if (found->second.generation != key.generation)
        return StateError::StaleGeneration;
    rooms_.erase(found);
    return StateError::None;
}

}  // namespace room_metadata
}  // namespace sdgo
