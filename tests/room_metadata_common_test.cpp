#include "room_metadata_common.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace rm = sdgo::room_metadata;

namespace {

int g_failures = 0;

void Check(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", description);
    if (!condition) ++g_failures;
}

void Put16(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint16_t value)
{
    (*bytes)[offset] = static_cast<std::uint8_t>(value);
    (*bytes)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void Put32(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint32_t value)
{
    (*bytes)[offset] = static_cast<std::uint8_t>(value);
    (*bytes)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    (*bytes)[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    (*bytes)[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

std::string Repeat(const char* utf8, std::size_t count)
{
    std::string output;
    for (std::size_t index = 0; index < count; ++index)
        output += utf8;
    return output;
}

rm::Message SetTitleMessage(const std::string& title)
{
    rm::Message message;
    message.kind = rm::MessageKind::SetTitle;
    message.server_epoch = 0x0102030405060708ull;
    message.room_id = 42;
    message.generation = 7;
    message.revision = 3;
    message.request_id = 99;
    message.title = title;
    return message;
}

void TestUtf8()
{
    const std::string twenty(20, 'a');
    const std::string twenty_one(21, 'b');
    Check(rm::ValidateTitle(twenty.data(), twenty.size()).ok(),
          "20-byte title is valid");
    Check(rm::ValidateTitle(twenty_one.data(), twenty_one.size()).ok(),
          "21-byte title is valid");

    const std::string four_byte = "\xF0\x9F\x98\x80";
    const std::string exact_limit = Repeat(four_byte.c_str(), 128);
    const rm::TitleValidation exact =
        rm::ValidateTitle(exact_limit.data(), exact_limit.size());
    Check(exact.ok() && exact.scalar_count == 128 && exact.byte_count == 512,
          "128 four-byte scalars and 512 bytes are valid");

    const std::string too_many_scalars(129, 'x');
    Check(rm::ValidateTitle(too_many_scalars.data(), too_many_scalars.size()).error ==
              rm::TitleError::TooManyScalars,
          "129 Unicode scalars are rejected");

    const std::string too_many_bytes = exact_limit + four_byte;
    Check(rm::ValidateTitle(too_many_bytes.data(), too_many_bytes.size()).error ==
              rm::TitleError::TooManyBytes,
          "more than 512 UTF-8 bytes are rejected");

    const std::string embedded_null("safe\0unsafe", 11);
    Check(rm::ValidateTitle(embedded_null.data(), embedded_null.size()).error ==
              rm::TitleError::EmbeddedNull,
          "embedded NUL is rejected");
    const std::string newline = "line\nbreak";
    Check(rm::ValidateTitle(newline.data(), newline.size()).error ==
              rm::TitleError::ControlCharacter,
          "C0 control character is rejected");
    const std::string c1_control = "\xC2\x85";
    Check(rm::ValidateTitle(c1_control.data(), c1_control.size()).error ==
              rm::TitleError::ControlCharacter,
          "C1 control character is rejected");

    const std::string overlong = "\xC0\xAF";
    const std::string truncated = "\xF0\x9F\x98";
    const std::string surrogate = "\xED\xA0\x80";
    const std::string out_of_range = "\xF4\x90\x80\x80";
    Check(rm::ValidateTitle(overlong.data(), overlong.size()).error ==
              rm::TitleError::InvalidUtf8,
          "overlong UTF-8 is rejected");
    Check(rm::ValidateTitle(truncated.data(), truncated.size()).error ==
              rm::TitleError::InvalidUtf8,
          "truncated four-byte UTF-8 is rejected");
    Check(rm::ValidateTitle(surrogate.data(), surrogate.size()).error ==
              rm::TitleError::InvalidUtf8,
          "UTF-8 surrogate is rejected");
    Check(rm::ValidateTitle(out_of_range.data(), out_of_range.size()).error ==
              rm::TitleError::InvalidUtf8,
          "Unicode value above U+10FFFF is rejected");
}

void TestMessages()
{
    std::vector<rm::Message> messages;

    rm::Message hello;
    hello.kind = rm::MessageKind::Hello;
    hello.request_id = 1;
    messages.push_back(hello);

    rm::Message ack;
    ack.kind = rm::MessageKind::HelloAck;
    ack.server_epoch = 10;
    ack.request_id = 1;
    messages.push_back(ack);

    messages.push_back(SetTitleMessage("mission !ai count=3"));

    rm::Message creating = SetTitleMessage("new room title before ACK");
    creating.server_epoch = 0;
    creating.room_id = 0;
    creating.generation = 0;
    creating.revision = 0;
    messages.push_back(creating);

    rm::Message snapshot;
    snapshot.kind = rm::MessageKind::Snapshot;
    snapshot.server_epoch = 10;
    snapshot.room_id = 3;
    snapshot.generation = 4;
    snapshot.revision = 5;
    snapshot.request_id = 2;
    snapshot.title = "PvP rules after byte twenty";
    messages.push_back(snapshot);

    rm::Message identity_snapshot = snapshot;
    identity_snapshot.room_id = 4;
    identity_snapshot.generation = 5;
    identity_snapshot.revision = 1;
    identity_snapshot.request_id = 0;
    identity_snapshot.title.clear();
    messages.push_back(identity_snapshot);

    rm::Message query;
    query.kind = rm::MessageKind::Query;
    query.server_epoch = 10;
    query.room_id = 3;
    query.generation = 4;
    query.revision = 5;
    query.request_id = 3;
    messages.push_back(query);

    rm::Message remote_error;
    remote_error.kind = rm::MessageKind::Error;
    remote_error.error = rm::RemoteError::StaleRevision;
    remote_error.server_epoch = 10;
    remote_error.room_id = 3;
    remote_error.generation = 4;
    remote_error.revision = 6;
    remote_error.request_id = 3;
    messages.push_back(remote_error);

    for (const rm::Message& expected : messages) {
        std::vector<std::uint8_t> body;
        rm::MessageError encode_error = rm::MessageError::InvalidFields;
        Check(rm::EncodeMessageBody(expected, &body, &encode_error) &&
                  encode_error == rm::MessageError::None,
              "valid message kind encodes");
        rm::Message actual;
        rm::MessageError decode_error = rm::MessageError::InvalidFields;
        Check(rm::DecodeMessageBody(body.data(), body.size(), &actual, &decode_error) &&
                  decode_error == rm::MessageError::None &&
                  actual.kind == expected.kind &&
                  actual.error == expected.error &&
                  actual.server_epoch == expected.server_epoch &&
                  actual.room_id == expected.room_id &&
                  actual.generation == expected.generation &&
                  actual.revision == expected.revision &&
                  actual.request_id == expected.request_id &&
                  actual.title == expected.title,
              "valid message round-trips");
    }

    rm::Message invalid = SetTitleMessage("");
    Check(rm::ValidateMessage(invalid) == rm::MessageError::InvalidFields,
          "SetTitle requires a title");
    invalid = SetTitleMessage("valid");
    invalid.request_id = 0;
    Check(rm::ValidateMessage(invalid) == rm::MessageError::InvalidFields,
          "SetTitle requires a request ID");
    invalid = SetTitleMessage("valid");
    invalid.error = rm::RemoteError::Conflict;
    Check(rm::ValidateMessage(invalid) == rm::MessageError::InvalidFields,
          "non-error messages reject an error code");
    invalid = SetTitleMessage("valid");
    invalid.server_epoch = 0;
    Check(rm::ValidateMessage(invalid) == rm::MessageError::InvalidFields,
          "SetTitle rejects a partial room key");

    std::vector<std::uint8_t> body;
    rm::EncodeMessageBody(SetTitleMessage("valid"), &body);
    Put16(&body, 28, 400);
    rm::Message ignored;
    Check(!rm::DecodeMessageBody(body.data(), body.size(), &ignored),
          "message body rejects inconsistent title length");
    rm::EncodeMessageBody(SetTitleMessage("valid"), &body);
    body[1] = 1;
    Check(!rm::DecodeMessageBody(body.data(), body.size(), &ignored),
          "message body rejects nonzero reserved fields");
}

rm::SnapshotRecord Snapshot(std::uint64_t epoch,
                            std::uint32_t room_id,
                            std::uint32_t generation,
                            std::uint32_t revision,
                            const std::string& title)
{
    rm::SnapshotRecord record;
    record.key.server_epoch = epoch;
    record.key.room_id = room_id;
    record.key.generation = generation;
    record.revision = revision;
    record.title = title;
    return record;
}

void TestSnapshotBatch()
{
    rm::SnapshotBatch expected;
    expected.server_epoch = 10;
    expected.request_id = 44;
    expected.rooms.push_back(Snapshot(10, 100, 1, 3, "mission !ai count=4"));
    expected.rooms.push_back(Snapshot(10, 200, 2, 7, "PvP team rules"));

    std::vector<std::uint8_t> body;
    Check(rm::EncodeSnapshotBatchBody(expected, &body),
          "snapshot batch encodes multiple rooms");
    rm::SnapshotBatch actual;
    Check(rm::DecodeSnapshotBatchBody(body.data(), body.size(), &actual) &&
              actual.server_epoch == expected.server_epoch &&
              actual.request_id == expected.request_id &&
              actual.rooms.size() == 2 &&
              actual.rooms[0].key.room_id == 100 &&
              actual.rooms[0].title == expected.rooms[0].title &&
              actual.rooms[1].key.room_id == 200 &&
              actual.rooms[1].title == expected.rooms[1].title,
          "snapshot batch round-trips mission and PvP rooms");

    rm::SnapshotBatch identity_only;
    identity_only.server_epoch = 10;
    identity_only.rooms.push_back(Snapshot(10, 300, 30, 1, ""));
    std::vector<std::uint8_t> identity_body;
    rm::SnapshotBatch decoded_identity;
    Check(rm::EncodeSnapshotBatchBody(identity_only, &identity_body) &&
              rm::DecodeSnapshotBatchBody(identity_body.data(),
                                           identity_body.size(),
                                           &decoded_identity) &&
              decoded_identity.rooms.size() == 1 &&
              decoded_identity.rooms[0].title.empty() &&
              decoded_identity.rooms[0].key.room_id == 300,
          "snapshot batch preserves identity-only empty title record");

    const std::vector<std::uint8_t> native = {0x91, 0x0F, 0x02, 0x00};
    std::vector<std::uint8_t> packet;
    Check(rm::EncodeSnapshotBatchPacket(native.data(), native.size(), expected,
                                        &packet),
          "snapshot batch uses one bounded native trailer");
    rm::TrailerView trailer;
    rm::SnapshotBatch packet_batch;
    Check(rm::DecodeSnapshotBatchPacket(packet.data(), packet.size(), &trailer,
                                        &packet_batch) &&
              trailer.native_size == native.size() &&
              packet_batch.rooms.size() == 2,
          "snapshot batch packet decodes without chained trailers");

    rm::SnapshotBatch maximum;
    maximum.server_epoch = 10;
    for (std::uint32_t index = 0;
         index < rm::kMaximumSnapshotBatchEntries; ++index) {
        maximum.rooms.push_back(
            Snapshot(10, index + 1, index + 10, 1, "bounded"));
    }
    Check(rm::ValidateSnapshotBatch(maximum) == rm::MessageError::None,
          "snapshot batch accepts 32 entries");
    maximum.rooms.push_back(Snapshot(10, 1000, 1000, 1, "too many"));
    Check(rm::ValidateSnapshotBatch(maximum) == rm::MessageError::TooManyEntries,
          "snapshot batch rejects more than 32 entries");

    rm::SnapshotBatch duplicate = expected;
    duplicate.rooms.push_back(expected.rooms[0]);
    Check(rm::ValidateSnapshotBatch(duplicate) == rm::MessageError::DuplicateRoom,
          "snapshot batch rejects a duplicate room key");

    rm::SnapshotBatch invalid_title = expected;
    invalid_title.rooms[1].title.assign("bad\0title", 9);
    Check(rm::ValidateSnapshotBatch(invalid_title) == rm::MessageError::InvalidTitle,
          "snapshot batch rejects any invalid title");

    rm::SnapshotBatch empty;
    empty.server_epoch = 10;
    empty.request_id = 0;
    std::vector<std::uint8_t> empty_body;
    Check(rm::EncodeSnapshotBatchBody(empty, &empty_body) &&
              empty_body.size() == 16,
          "empty snapshot batch carries server epoch capability");
    rm::SnapshotBatch decoded_empty;
    Check(rm::DecodeSnapshotBatchBody(empty_body.data(), empty_body.size(),
                                      &decoded_empty) &&
              decoded_empty.server_epoch == 10 &&
              decoded_empty.request_id == 0 &&
              decoded_empty.rooms.empty(),
          "empty unsolicited snapshot batch round-trips");

    rm::SnapshotBatch mismatched_epoch = expected;
    mismatched_epoch.rooms[1].key.server_epoch = 11;
    Check(rm::ValidateSnapshotBatch(mismatched_epoch) ==
              rm::MessageError::InvalidFields,
          "snapshot batch rejects a record from another server epoch");
    rm::SnapshotBatch missing_epoch = expected;
    missing_epoch.server_epoch = 0;
    Check(rm::ValidateSnapshotBatch(missing_epoch) ==
              rm::MessageError::InvalidFields,
          "snapshot batch requires a nonzero server epoch");

    std::vector<std::uint8_t> bad_count = body;
    Put16(&bad_count, 2, 3);
    rm::SnapshotBatch unchanged;
    unchanged.server_epoch = 99;
    unchanged.request_id = 999;
    unchanged.rooms.push_back(Snapshot(99, 99, 99, 99, "sentinel"));
    Check(!rm::DecodeSnapshotBatchBody(bad_count.data(), bad_count.size(),
                                       &unchanged) &&
              unchanged.request_id == 999 &&
              unchanged.server_epoch == 99 &&
              unchanged.rooms.size() == 1 &&
              unchanged.rooms[0].title == "sentinel",
          "bad batch count rejects the whole batch without partial output");

    std::vector<std::uint8_t> excessive_count(8, 0);
    excessive_count.resize(16);
    excessive_count[0] = static_cast<std::uint8_t>(rm::MessageKind::SnapshotBatch);
    Put16(&excessive_count, 2,
          static_cast<std::uint16_t>(rm::kMaximumSnapshotBatchEntries + 1));
    rm::MessageError batch_error = rm::MessageError::None;
    Check(!rm::DecodeSnapshotBatchBody(excessive_count.data(),
                                       excessive_count.size(), &unchanged,
                                       &batch_error) &&
              batch_error == rm::MessageError::TooManyEntries &&
              unchanged.request_id == 999,
          "wire batch count above 32 rejects the whole batch");

    std::vector<std::uint8_t> truncated_header = empty_body;
    truncated_header.resize(15);
    batch_error = rm::MessageError::None;
    Check(!rm::DecodeSnapshotBatchBody(truncated_header.data(),
                                       truncated_header.size(), &unchanged,
                                       &batch_error) &&
              batch_error == rm::MessageError::BodyTooShort &&
              unchanged.server_epoch == 99 &&
              unchanged.request_id == 999,
          "truncated batch header is rejected without changing output");

    std::vector<std::uint8_t> duplicate_wire = body;
    const std::size_t second_record =
        16 + 24 + expected.rooms[0].title.size();
    std::vector<std::uint8_t> mismatched_epoch_wire = body;
    mismatched_epoch_wire[second_record] = 11;
    batch_error = rm::MessageError::None;
    Check(!rm::DecodeSnapshotBatchBody(mismatched_epoch_wire.data(),
                                       mismatched_epoch_wire.size(), &unchanged,
                                       &batch_error) &&
              batch_error == rm::MessageError::InvalidFields &&
              unchanged.server_epoch == 99 &&
              unchanged.request_id == 999,
          "wire record epoch mismatch rejects the whole batch");

    Put32(&duplicate_wire, second_record + 8, expected.rooms[0].key.room_id);
    Put32(&duplicate_wire, second_record + 12,
          expected.rooms[0].key.generation);
    batch_error = rm::MessageError::None;
    Check(!rm::DecodeSnapshotBatchBody(duplicate_wire.data(),
                                       duplicate_wire.size(), &unchanged,
                                       &batch_error) &&
              batch_error == rm::MessageError::DuplicateRoom &&
              unchanged.request_id == 999,
          "duplicate wire room key rejects the whole batch");

    std::vector<std::uint8_t> bad_title;
    rm::EncodeSnapshotBatchBody(expected, &bad_title);
    const std::size_t second_title =
        16 + 24 + expected.rooms[0].title.size() + 24;
    bad_title[second_title] = 0;
    batch_error = rm::MessageError::None;
    Check(!rm::DecodeSnapshotBatchBody(bad_title.data(), bad_title.size(),
                                       &unchanged, &batch_error) &&
              batch_error == rm::MessageError::InvalidTitle &&
              unchanged.request_id == 999 &&
              unchanged.rooms.size() == 1,
          "one invalid room title rejects the whole decoded batch");
}

void TestTrailer()
{
    const std::vector<std::uint8_t> native = {0x34, 0x12, 0x52, 0x54, 0x4D, 0x44,
                                               0x78, 0x56};
    const std::vector<std::uint8_t> native_before = native;
    const rm::Message expected = SetTitleMessage("title beyond native field");
    std::vector<std::uint8_t> body;
    rm::EncodeMessageBody(expected, &body);
    std::vector<std::uint8_t> packet;
    Check(rm::EncodePacket(native.data(), native.size(), expected, &packet),
          "message trailer appends to native payload");
    Check(native == native_before &&
              packet.size() > native.size() &&
              std::memcmp(packet.data(), native.data(), native.size()) == 0,
          "appending leaves original and copied native bytes unchanged");

    rm::TrailerView trailer;
    rm::Message actual;
    Check(rm::DecodePacket(packet.data(), packet.size(), &trailer, &actual) &&
              trailer.native_size == native.size() &&
              actual.title == expected.title,
          "message trailer decodes with bounded native and body views");

    const std::vector<std::uint8_t> false_positive =
        {0x10, 0x20, 0x52, 0x54, 0x4D, 0x44, 0x30, 0x40};
    Check(rm::ParseTrailer(false_positive.data(), false_positive.size()).status ==
              rm::TrailerStatus::NoTrailer,
          "magic away from packet tail is not a trailer");

    const std::vector<std::uint8_t> no_trailer = {1, 2, 3, 4};
    Check(rm::ParseTrailer(no_trailer.data(), no_trailer.size()).status ==
              rm::TrailerStatus::NoTrailer,
          "native packet reports no trailer");

    const std::vector<std::uint8_t> minimum_native = {1, 2};
    std::vector<std::uint8_t> maximum_body(
        rm::kMaximumPacketBytes - minimum_native.size() -
            rm::kTrailerFooterBytes,
        0x5A);
    std::vector<std::uint8_t> maximum_packet;
    Check(rm::AppendTrailer(minimum_native.data(), minimum_native.size(),
                            maximum_body.data(), maximum_body.size(),
                            &maximum_packet) &&
              maximum_packet.size() == rm::kMaximumPacketBytes &&
              rm::ParseTrailer(maximum_packet.data(), maximum_packet.size()).status ==
                  rm::TrailerStatus::Valid,
          "65,535-byte packet boundary is accepted");
    maximum_body.push_back(0x5A);
    Check(!rm::AppendTrailer(minimum_native.data(), minimum_native.size(),
                             maximum_body.data(), maximum_body.size(),
                             &maximum_packet),
          "packet larger than 65,535 bytes is rejected");
    Check(!rm::AppendTrailer(minimum_native.data(), 1, body.data(), body.size(),
                             &maximum_packet),
          "native payload shorter than two bytes is rejected by builder");

    const std::vector<std::uint8_t> truncated_magic = {0x52, 0x54, 0x4D, 0x44};
    const rm::TrailerParseResult truncated =
        rm::ParseTrailer(truncated_magic.data(), truncated_magic.size());
    Check(truncated.status == rm::TrailerStatus::Invalid &&
              truncated.error == rm::TrailerError::TruncatedFooter,
          "trailing magic with no footer reports invalid trailer");

    std::vector<std::uint8_t> wrapped_magic_native =
        {0x01, 0x02, 0x52, 0x54, 0x4D, 0x44};
    wrapped_magic_native.insert(wrapped_magic_native.end(),
                                body.begin(), body.end());
    wrapped_magic_native.resize(wrapped_magic_native.size() +
                                rm::kTrailerFooterBytes);
    const std::size_t wrapped_magic_footer =
        wrapped_magic_native.size() - rm::kTrailerFooterBytes;
    Put16(&wrapped_magic_native, wrapped_magic_footer, rm::kProtocolVersion);
    Put16(&wrapped_magic_native, wrapped_magic_footer + 2, 0);
    Put32(&wrapped_magic_native, wrapped_magic_footer + 4, 6);
    Put32(&wrapped_magic_native, wrapped_magic_footer + 8,
          static_cast<std::uint32_t>(body.size()));
    Put32(&wrapped_magic_native, wrapped_magic_footer + 12, 0x444D5452u);
    const rm::TrailerParseResult wrapped_magic_result =
        rm::ParseTrailer(wrapped_magic_native.data(), wrapped_magic_native.size());
    Check(wrapped_magic_result.status == rm::TrailerStatus::Invalid &&
              wrapped_magic_result.error == rm::TrailerError::DuplicateTrailer,
          "wrapped native tail-magic candidate follows strict duplicate policy");

    std::vector<std::uint8_t> corrupt = packet;
    const std::size_t footer = corrupt.size() - rm::kTrailerFooterBytes;
    Put32(&corrupt, footer + 8, 0xFFFF);
    const rm::TrailerParseResult corrupt_body =
        rm::ParseTrailer(corrupt.data(), corrupt.size());
    Check(corrupt_body.status == rm::TrailerStatus::Invalid &&
              corrupt_body.error == rm::TrailerError::InconsistentLength,
          "corrupt body length is rejected");

    corrupt = packet;
    Put32(&corrupt, footer + 4, 1);
    const rm::TrailerParseResult corrupt_native =
        rm::ParseTrailer(corrupt.data(), corrupt.size());
    Check(corrupt_native.status == rm::TrailerStatus::Invalid &&
              corrupt_native.error == rm::TrailerError::NativePayloadTooShort,
          "native payload shorter than two bytes is rejected");

    corrupt = packet;
    Put16(&corrupt, footer, rm::kProtocolVersion + 1);
    const rm::TrailerParseResult unsupported =
        rm::ParseTrailer(corrupt.data(), corrupt.size());
    Check(unsupported.status == rm::TrailerStatus::Invalid &&
              unsupported.error == rm::TrailerError::UnsupportedVersion,
          "unsupported trailer version is rejected");

    std::vector<std::uint8_t> duplicate;
    rm::TrailerError append_error = rm::TrailerError::None;
    Check(!rm::AppendTrailer(packet.data(), packet.size(), body.data(), body.size(),
                             &duplicate, &append_error) &&
              append_error == rm::TrailerError::DuplicateTrailer,
          "builder rejects an already extended packet");

    duplicate = packet;
    duplicate.insert(duplicate.end(), body.begin(), body.end());
    duplicate.resize(duplicate.size() + rm::kTrailerFooterBytes);
    const std::size_t outer_footer = duplicate.size() - rm::kTrailerFooterBytes;
    Put16(&duplicate, outer_footer, rm::kProtocolVersion);
    Put16(&duplicate, outer_footer + 2, 0);
    Put32(&duplicate, outer_footer + 4, static_cast<std::uint32_t>(packet.size()));
    Put32(&duplicate, outer_footer + 8, static_cast<std::uint32_t>(body.size()));
    Put32(&duplicate, outer_footer + 12, 0x444D5452u);
    const rm::TrailerParseResult duplicate_result =
        rm::ParseTrailer(duplicate.data(), duplicate.size());
    Check(duplicate_result.status == rm::TrailerStatus::Invalid &&
              duplicate_result.error == rm::TrailerError::DuplicateTrailer,
          "parser rejects nested duplicate trailers");

    std::vector<std::uint8_t> deeply_nested = {0x01, 0x02};
    for (std::uint32_t depth = 0; depth < 1200; ++depth) {
        const std::uint32_t nested_native_size =
            static_cast<std::uint32_t>(deeply_nested.size());
        deeply_nested.push_back(static_cast<std::uint8_t>(depth));
        const std::size_t nested_footer = deeply_nested.size();
        deeply_nested.resize(deeply_nested.size() + rm::kTrailerFooterBytes);
        Put16(&deeply_nested, nested_footer, rm::kProtocolVersion);
        Put16(&deeply_nested, nested_footer + 2, 0);
        Put32(&deeply_nested, nested_footer + 4, nested_native_size);
        Put32(&deeply_nested, nested_footer + 8, 1);
        Put32(&deeply_nested, nested_footer + 12, 0x444D5452u);
    }
    const rm::TrailerParseResult deeply_nested_result =
        rm::ParseTrailer(deeply_nested.data(), deeply_nested.size());
    Check(deeply_nested_result.status == rm::TrailerStatus::Invalid &&
              deeply_nested_result.error == rm::TrailerError::DuplicateTrailer,
          "1,200 nested trailers are rejected without recursive parsing");

    rm::TrailerView unchanged_view;
    unchanged_view.native_data = native.data();
    unchanged_view.native_size = 123;
    rm::Message unchanged_message = SetTitleMessage("sentinel");
    rm::TrailerError deeply_nested_error = rm::TrailerError::None;
    Check(!rm::DecodePacket(deeply_nested.data(), deeply_nested.size(),
                            &unchanged_view, &unchanged_message,
                            &deeply_nested_error) &&
              deeply_nested_error == rm::TrailerError::DuplicateTrailer &&
              unchanged_view.native_data == native.data() &&
              unchanged_view.native_size == 123 &&
              unchanged_message.title == "sentinel",
          "deep nesting rejection leaves decode outputs unchanged");

    std::vector<std::uint8_t> unchanged_packet = {0xAA, 0xBB, 0xCC};
    rm::TrailerError deeply_nested_append_error = rm::TrailerError::None;
    Check(!rm::AppendTrailer(deeply_nested.data(), deeply_nested.size(),
                             body.data(), body.size(), &unchanged_packet,
                             &deeply_nested_append_error) &&
              deeply_nested_append_error == rm::TrailerError::DuplicateTrailer &&
              unchanged_packet ==
                  std::vector<std::uint8_t>({0xAA, 0xBB, 0xCC}),
          "builder rejects deep nesting without modifying output");

    std::vector<std::uint8_t> invalid_body = packet;
    invalid_body[native.size()] = 0xFF;
    rm::TrailerError trailer_error = rm::TrailerError::None;
    rm::MessageError message_error = rm::MessageError::None;
    Check(!rm::DecodePacket(invalid_body.data(), invalid_body.size(), &trailer,
                            &actual, &trailer_error, &message_error) &&
              trailer_error == rm::TrailerError::InvalidMessageBody &&
              message_error == rm::MessageError::UnknownKind,
          "valid trailer with invalid message body is rejected");
}

void TestRoomStore()
{
    rm::RoomStore store(0xAABBCCDDEEFF0011ull);
    const rm::StateResult legacy = store.CreateLegacyRoom(300, 5000);
    Check(legacy.ok() &&
              legacy.snapshot.key.room_id == 300 &&
              legacy.snapshot.key.generation != 0 &&
              legacy.snapshot.revision == 1 &&
              legacy.snapshot.owner_session == 5000 &&
              legacy.snapshot.title.empty(),
          "trusted legacy observation creates identity-only room metadata");
    Check(store.CreateLegacyRoom(300, 5000).error ==
              rm::StateError::AlreadyExists,
          "legacy observation does not replace an existing room");
    Check(store.CreateRoom(301, 5000, "").error ==
              rm::StateError::InvalidArgument &&
              store.UpdateTitle(legacy.snapshot.key, 5000,
                                legacy.snapshot.revision, "").error ==
                  rm::StateError::InvalidArgument &&
              store.Query(legacy.snapshot.key).snapshot.title.empty(),
          "user create and update APIs still reject empty titles");
    const rm::StateResult extended_legacy =
        store.UpdateTitle(legacy.snapshot.key, 5000,
                          legacy.snapshot.revision,
                          "legacy room now has a full title");
    Check(extended_legacy.ok() &&
              extended_legacy.snapshot.revision == 2 &&
              extended_legacy.snapshot.title ==
                  "legacy room now has a full title",
          "legacy identity can be updated through normal owner authorization");
    const rm::StateResult cleared_legacy =
        store.ClearNativeTitle(legacy.snapshot.key);
    Check(cleared_legacy.ok() &&
              cleared_legacy.snapshot.key.generation ==
                  legacy.snapshot.key.generation &&
              cleared_legacy.snapshot.owner_session == 5000 &&
              cleared_legacy.snapshot.revision == 3 &&
              cleared_legacy.snapshot.title.empty() &&
              store.Query(legacy.snapshot.key).snapshot.title.empty(),
          "trusted native clear keeps identity and advances revision");
    const rm::StateResult repeated_clear =
        store.ClearNativeTitle(legacy.snapshot.key);
    Check(repeated_clear.ok() &&
              repeated_clear.snapshot.revision == 3 &&
              repeated_clear.snapshot.title.empty(),
          "repeated native clear is idempotent");
    Check(store.UpdateTitle(legacy.snapshot.key, 5000, 2,
                            "stale after clear").error ==
              rm::StateError::StaleRevision,
          "pre-clear revision cannot update identity after native clear");
    const rm::StateResult restored_legacy =
        store.UpdateTitle(legacy.snapshot.key, 5000,
                          cleared_legacy.snapshot.revision,
                          "new full title after native clear");
    Check(restored_legacy.ok() &&
              restored_legacy.snapshot.revision == 4 &&
              restored_legacy.snapshot.title ==
                  "new full title after native clear",
          "cleared identity accepts a new authorized nonempty title");
    rm::RoomKey wrong_clear_key = legacy.snapshot.key;
    ++wrong_clear_key.generation;
    Check(store.ClearNativeTitle(wrong_clear_key).error ==
              rm::StateError::StaleGeneration,
          "native clear rejects a stale room generation");
    wrong_clear_key = legacy.snapshot.key;
    ++wrong_clear_key.server_epoch;
    Check(store.ClearNativeTitle(wrong_clear_key).error ==
              rm::StateError::StaleEpoch,
          "native clear rejects a stale server epoch");
    wrong_clear_key = legacy.snapshot.key;
    wrong_clear_key.room_id = 9999;
    Check(store.ClearNativeTitle(wrong_clear_key).error ==
              rm::StateError::NotFound,
          "native clear rejects an unknown room");

    const rm::StateResult mission = store.CreateRoom(100, 1000, "mission !ai");
    const rm::StateResult pvp = store.CreateRoom(200, 2000, "pvp teams=2");
    Check(mission.ok() && pvp.ok() && store.room_count() == 3 &&
              mission.snapshot.key.generation != pvp.snapshot.key.generation,
          "multiple rooms receive isolated generations");

    rm::StateResult updated =
        store.UpdateTitle(mission.snapshot.key, 1000, mission.snapshot.revision,
                          "mission rule after byte 20 !ai");
    Check(updated.ok() && updated.snapshot.revision == 2 &&
              store.Query(pvp.snapshot.key).snapshot.title == "pvp teams=2",
          "mission update does not affect PvP room");

    const rm::StateResult unauthorized =
        store.UpdateTitle(mission.snapshot.key, 9999, updated.snapshot.revision,
                          "unauthorized");
    Check(unauthorized.error == rm::StateError::Unauthorized &&
              store.Query(mission.snapshot.key).snapshot.title == updated.snapshot.title,
          "non-owner title update is rejected without mutation");

    const rm::StateResult stale_revision =
        store.UpdateTitle(mission.snapshot.key, 1000, mission.snapshot.revision,
                          "stale");
    Check(stale_revision.error == rm::StateError::StaleRevision,
          "stale title revision is rejected");

    const rm::StateResult transfer =
        store.TransferHost(mission.snapshot.key, 1000, 3000,
                           updated.snapshot.revision);
    Check(transfer.ok() && transfer.snapshot.owner_session == 3000 &&
              transfer.snapshot.revision == updated.snapshot.revision + 1,
          "host transfer updates authority and revision");
    Check(store.UpdateTitle(mission.snapshot.key, 1000, transfer.snapshot.revision,
                            "old host").error == rm::StateError::Unauthorized,
          "old host loses update authority");
    updated = store.UpdateTitle(mission.snapshot.key, 3000, transfer.snapshot.revision,
                                "new host title");
    Check(updated.ok(), "new host can update title");

    const rm::RoomKey old_key = pvp.snapshot.key;
    Check(store.DestroyRoom(old_key) == rm::StateError::None &&
              store.Query(old_key).error == rm::StateError::NotFound,
          "room destruction removes its snapshot");
    const rm::StateResult reused = store.CreateRoom(200, 4000, "reused room ID");
    Check(reused.ok() && reused.snapshot.key.generation != old_key.generation &&
              store.Query(old_key).error == rm::StateError::StaleGeneration,
          "room ID reuse gets a new generation and rejects old keys");

    rm::RoomKey stale_epoch = reused.snapshot.key;
    stale_epoch.server_epoch ^= 1;
    Check(store.Query(stale_epoch).error == rm::StateError::StaleEpoch,
          "reconnect query rejects a stale server epoch");

    const std::string invalid_title("bad\0title", 9);
    Check(store.UpdateTitle(reused.snapshot.key, 4000, reused.snapshot.revision,
                            invalid_title).error == rm::StateError::InvalidTitle,
          "state rejects invalid UTF-8 title data");
}

}  // namespace

int main()
{
    TestUtf8();
    TestMessages();
    TestTrailer();
    TestSnapshotBatch();
    TestRoomStore();
    std::printf("%s: %d failure(s)\n",
                g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
