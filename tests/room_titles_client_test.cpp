// Root client integration tests. The actual room_titles.cpp is included so
// Prepare/BeforeSend/AfterSend/OnReceive/OnInit/LookupTitle are tested without
// duplicating their wrapper logic. Native UI functions are a local CP936 shim;
// this is not evidence about a live client language or SDPlus runtime.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../mods/room_titles/room_titles.cpp"

namespace sdgo::room_titles::native_ui {
namespace {
struct FakeEdit
{
    std::wstring text;
    unsigned writes = 0;
};
std::wstring g_title_bar_cache;
}  // namespace

bool Bind(const SdgoApi*, SdgoMod*) { return true; }
bool InstallInitHooks(InitCallback) { return true; }
bool InstallTitleDisplayHooks(TitleLookupCallback) { return true; }
void* FindRoomNameEdit(void* service, Surface) { return service; }
bool SetRoomNameLimit(void*) { return true; }

bool ReadEditText(void* edit, std::wstring* copy)
{
    if (!edit || !copy)
        return false;
    *copy = static_cast<FakeEdit*>(edit)->text;
    return true;
}

bool ReadNativeString(const void* native_string, std::wstring* copy)
{
    if (!native_string || !copy)
        return false;
    *copy = static_cast<const FakeEdit*>(native_string)->text;
    return true;
}

bool SetEditText(void* edit, const std::wstring& text, bool)
{
    if (!edit)
        return false;
    auto* target = static_cast<FakeEdit*>(edit);
    target->text = text;
    ++target->writes;
    return true;
}

bool SetTitleBarCachedTitle(void*, const std::wstring& title) { g_title_bar_cache = title; return true; }

bool ReadTitleBarRoom(void* self, bool* battle, uint32_t* id, std::wstring* legacy)
{
    if (!self || !battle || !id || !legacy) return false;
    auto bytes = static_cast<uint8_t*>(self);
    void* room = nullptr;
    std::memcpy(&room, bytes + 0x108, sizeof(room));
    *battle = room != nullptr;
    if (!room) std::memcpy(&room, bytes + 0x10C, sizeof(room));
    if (!room) return false;
    std::memcpy(id, room, sizeof(*id));
    *legacy = L"native title";
    return *id != 0;
}

bool LegacyTitleBytes(const std::wstring& text, std::string* copy)
{
    if (!copy || text.empty())
        return false;
    // Simulate the user's SDPlus-style code-page remap with Windows CP936.
    // Unrepresentable Unicode follows the native default-character behavior;
    // Projection's reverse conversion decides whether legacy fallback is exact.
    const int bytes = WideCharToMultiByte(
        936, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
        nullptr, nullptr);
    if (bytes <= 0)
        return false;
    std::string encoded(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(
            936, 0, text.data(), static_cast<int>(text.size()),
            encoded.data(), bytes, nullptr, nullptr) != bytes)
        return false;
    copy->swap(encoded);
    return true;
}
}  // namespace sdgo::room_titles::native_ui

namespace {
namespace titles = sdgo::room_titles;
namespace metadata = sdgo::room_metadata;
namespace ui = sdgo::room_titles::native_ui;
namespace transport = sdgo::room_titles::native_transport;

constexpr uint16_t kHello = 0x07FB;
constexpr uint16_t kMissionHello = 0x0A08;
constexpr uint16_t kMissionRoomList = 0x0824;
constexpr uint16_t kCreate = 0x0640;
constexpr uint16_t kMissionSettings = 0x09ED;
constexpr uint16_t kPvpTitleEdit = 0x089E;
constexpr uint16_t kStartBattle = 0x08A7;
constexpr uint16_t kRoomList = 0x0617;
constexpr uint16_t kCreateAck = 0x0A33;
constexpr uint16_t kPvpRoom = 0x0550;
constexpr uint16_t kTitleNotice = 0x06BA;
constexpr uint32_t kBattleDomainForTest = 0x80000000u;

int g_failures = 0;
std::vector<std::wstring> g_notices;
uint32_t g_fake_mission_room = 0;
void* g_fake_mission_room_object = &g_fake_mission_room;
int g_title_event_calls = 0;
int g_title_event_result = 0;
int (*g_title_event_callback)(void*) = nullptr;
metadata::Message g_title_event_message;

int __fastcall FakeTitleEvent(void*, void*, void* event)
{
    ++g_title_event_calls;
    if (g_title_event_callback) return g_title_event_callback(event);
    return g_title_event_result;
}

void Check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

void Notice(const wchar_t* text)
{
    g_notices.emplace_back(text ? text : L"");
}

const wchar_t* __cdecl Cp936ToWide(UINT, const char* text)
{
    static thread_local std::wstring decoded;
    decoded.clear();
    if (!text)
        return nullptr;
    const int units =
        MultiByteToWideChar(936, 0, text, -1, nullptr, 0);
    if (units <= 0)
        return nullptr;
    decoded.resize(static_cast<size_t>(units));
    if (MultiByteToWideChar(936, 0, text, -1, decoded.data(), units) <= 0) {
        decoded.clear();
        return nullptr;
    }
    decoded.resize(static_cast<size_t>(units - 1));
    return decoded.c_str();
}

void Put16(void* pointer, uint16_t value)
{
    std::memcpy(pointer, &value, sizeof(value));
}

void Put32(void* pointer, uint32_t value)
{
    std::memcpy(pointer, &value, sizeof(value));
}

std::vector<uint8_t> NativePacket(size_t size, uint16_t opcode,
                                  uint8_t seed = 0x31)
{
    std::vector<uint8_t> packet(size);
    for (size_t index = 0; index < size; ++index)
        packet[index] = static_cast<uint8_t>(seed + index * 7u);
    Put16(packet.data(), opcode);
    return packet;
}

std::string Utf8Bytes(const std::wstring& text)
{
    const int bytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};
    std::string encoded(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), encoded.data(), bytes,
            nullptr, nullptr) != bytes)
        return {};
    return encoded;
}

metadata::SnapshotRecord Record(uint64_t epoch, uint32_t room,
                                uint32_t generation, uint32_t revision,
                                const std::string& title)
{
    metadata::SnapshotRecord record;
    record.key.server_epoch = epoch;
    record.key.room_id = room;
    record.key.generation = generation;
    record.revision = revision;
    record.title = title;
    return record;
}

metadata::SnapshotBatch Batch(uint64_t epoch, uint32_t request,
                              std::vector<metadata::SnapshotRecord> rooms = {})
{
    metadata::SnapshotBatch batch;
    batch.server_epoch = epoch;
    batch.request_id = request;
    batch.rooms = std::move(rooms);
    return batch;
}

metadata::Message Snapshot(const metadata::SnapshotRecord& record,
                           uint32_t request)
{
    metadata::Message message;
    message.kind = metadata::MessageKind::Snapshot;
    message.server_epoch = record.key.server_epoch;
    message.room_id = record.key.room_id;
    message.generation = record.key.generation;
    message.revision = record.revision;
    message.request_id = request;
    message.title = record.title;
    return message;
}

void ResetRoot()
{
    g_state = titles::ClientState();
    g_activeConnection = 0;
    g_activeRoom = 0;
    g_drafts.clear();
    for (SavedDraft& saved : g_saved)
        saved = SavedDraft();
    g_notices.clear();
    g_notice = &Notice;
    g_nativeWide = &Cp936ToWide;
    g_fake_mission_room = 0;
    g_fake_mission_room_object = &g_fake_mission_room;
    g_missionRoom = &g_fake_mission_room_object;
    g_title_event_calls = 0;
    g_title_event_result = 0;
    g_title_event_callback = nullptr;
    g_title_event_message = {};
    ui::g_title_bar_cache.clear();
    g_titleEvent = reinterpret_cast<EventFn>(&FakeTitleEvent);
}

bool DecodeMessagePacket(const std::vector<uint8_t>& packet,
                         metadata::TrailerView* view,
                         metadata::Message* message)
{
    return metadata::DecodePacket(packet.data(), packet.size(), view,
                                  message);
}

bool Ready(uint32_t connection, uint64_t epoch,
           std::vector<metadata::SnapshotRecord> rooms = {})
{
    auto hello_native = NativePacket(0x16, kHello);
    std::vector<uint8_t> replacement;
    if (BeforeSend(connection, hello_native.data(), hello_native.size(), 0,
                   &replacement) != transport::SendDecision::Replace)
        return false;
    metadata::TrailerView view;
    metadata::Message hello;
    if (!DecodeMessagePacket(replacement, &view, &hello) ||
        hello.kind != metadata::MessageKind::Hello ||
        !hello.request_id)
        return false;
    std::vector<uint8_t> body;
    if (!metadata::EncodeSnapshotBatchBody(
            Batch(epoch, hello.request_id, std::move(rooms)), &body))
        return false;
    auto list = NativePacket(ReceiveSize(kRoomList), kRoomList);
    OnReceive(connection, list.data(), list.size(), body.data(),
              body.size());
    return g_state.IsReady(connection);
}

void Seed(uint32_t connection, uint64_t epoch,
          std::vector<metadata::SnapshotRecord> rooms)
{
    std::vector<uint8_t> body;
    Check(metadata::EncodeSnapshotBatchBody(
              Batch(epoch, 0, std::move(rooms)), &body),
          "common protocol encodes an unsolicited identity batch");
    auto list = NativePacket(ReceiveSize(kRoomList), kRoomList);
    OnReceive(connection, list.data(), list.size(), body.data(),
              body.size());
}

std::vector<uint8_t> DraftPacket(uint16_t opcode,
                                 const std::wstring& text,
                                 int incoming_override = -1)
{
    const TitleField field = Field(opcode);
    std::vector<uint8_t> packet =
        NativePacket(field.size, opcode,
                     static_cast<uint8_t>(opcode));
    if (opcode == kPvpTitleEdit)
        packet[6] = 1;
    std::string legacy;
    Check(ui::LegacyTitleBytes(text, &legacy),
          "CP936 shim projects the native builder title");
    const size_t direct = std::min<size_t>(legacy.size(), 20);
    const size_t incoming =
        incoming_override >= 0 ? static_cast<size_t>(incoming_override)
                               : direct;
    packet[field.length] = static_cast<uint8_t>(incoming);
    std::memset(packet.data() + field.text, 0, 21);
    if (incoming)
        std::memcpy(packet.data() + field.text, legacy.data(), incoming);
    return packet;
}

struct PreparedSend
{
    transport::SendDecision decision = transport::SendDecision::Pass;
    std::vector<uint8_t> original;
    std::vector<uint8_t> replacement;
    metadata::TrailerView view;
    metadata::Message message;
    std::vector<uint8_t> native;
};

PreparedSend SendDraft(uint32_t connection, uint16_t opcode,
                       const std::wstring& text,
                       int incoming_override = -1)
{
    PreparedSend sent;
    sent.original = DraftPacket(opcode, text, incoming_override);
    const auto caller_copy = sent.original;
    sent.decision =
        BeforeSend(connection, sent.original.data(), sent.original.size(),
                   0x55667788, &sent.replacement);
    Check(sent.original == caller_copy,
          "BeforeSend leaves the original native caller buffer unchanged");
    if (sent.decision == transport::SendDecision::Replace &&
        DecodeMessagePacket(sent.replacement, &sent.view, &sent.message)) {
        sent.native.assign(sent.view.native_data,
                           sent.view.native_data + sent.view.native_size);
    }
    return sent;
}

bool OnlyTitleProjectionChanged(const PreparedSend& sent,
                                uint16_t opcode,
                                const std::wstring& text)
{
    const TitleField field = Field(opcode);
    if (sent.native.size() != sent.original.size())
        return false;
    std::string full;
    std::string shortened;
    bool exact = false;
    if (!Projection(text, &full, &shortened, &exact))
        return false;
    for (size_t index = 0; index < sent.native.size(); ++index) {
        if (index == field.length ||
            (index >= field.text && index < field.text + 21))
            continue;
        if (sent.native[index] != sent.original[index])
            return false;
    }
    if (sent.native[field.length] != shortened.size())
        return false;
    for (size_t index = 0; index < 21; ++index) {
        const uint8_t expected =
            index < shortened.size()
                ? static_cast<uint8_t>(shortened[index])
                : 0;
        if (sent.native[field.text + index] != expected)
            return false;
    }
    return true;
}

void AckBatch(uint32_t connection, uint32_t room, uint64_t epoch,
              uint32_t generation, uint32_t revision,
              const std::string& title, uint32_t request)
{
    std::vector<uint8_t> body;
    Check(metadata::EncodeSnapshotBatchBody(
              Batch(epoch, request,
                    {Record(epoch, room, generation, revision, title)}),
              &body),
          "common protocol encodes the matching create acknowledgement");
    auto packet = NativePacket(ReceiveSize(kCreateAck), kCreateAck);
    Put32(packet.data() + 6, room);
    OnReceive(connection, packet.data(), packet.size(), body.data(),
              body.size());
}

void AckSnapshot(uint32_t connection,
                 const metadata::SnapshotRecord& record,
                 uint32_t request)
{
    std::vector<uint8_t> body;
    Check(metadata::EncodeMessageBody(Snapshot(record, request), &body),
          "common protocol encodes the matching title acknowledgement");
    auto packet = NativePacket(ReceiveSize(kTitleNotice), kTitleNotice);
    OnReceive(connection, packet.data(), packet.size(), body.data(),
              body.size());
}

transport::SendDecision StartDecision(uint32_t connection)
{
    auto start = NativePacket(2, kStartBattle);
    std::vector<uint8_t> replacement;
    return BeforeSend(connection, start.data(), start.size(), 0,
                      &replacement);
}

void ActivatePvpRoom(uint32_t connection, uint32_t native_room)
{
    auto packet = NativePacket(ReceiveSize(kPvpRoom), kPvpRoom);
    Put32(packet.data() + 6, native_room);
    OnReceive(connection, packet.data(), packet.size(), nullptr, 0);
}

std::wstring EmojiTitle(size_t count)
{
    std::wstring text;
    text.reserve(count * 2);
    for (size_t index = 0; index < count; ++index) {
        text.push_back(static_cast<wchar_t>(0xD83D));
        text.push_back(static_cast<wchar_t>(0xDE00));
    }
    return text;
}

void TestCapabilityAndLegacyFallback()
{
    ResetRoot();
    auto mission_hello_native =
        NativePacket(0x16, kMissionHello);
    std::vector<uint8_t> mission_replacement;
    metadata::TrailerView mission_view;
    metadata::Message mission_hello;
    Check(BeforeSend(2, mission_hello_native.data(),
                     mission_hello_native.size(), 0,
                     &mission_replacement) ==
                  transport::SendDecision::Replace &&
              DecodeMessagePacket(mission_replacement, &mission_view,
                                  &mission_hello) &&
              mission_view.native_size == mission_hello_native.size() &&
              mission_hello.kind == metadata::MessageKind::Hello &&
              mission_hello.request_id != 0 &&
              g_activeConnection == 2,
          "mission Team-list 0xA08 carries the same capability Hello as the Battle list");
    std::vector<uint8_t> mission_body;
    Check(metadata::EncodeSnapshotBatchBody(
              Batch(99, mission_hello.request_id,
                    {Record(99, 3, 1, 1,
                            "mission lobby complete title")}),
              &mission_body),
          "common protocol encodes a mission Team-list capability batch");
    auto mission_list =
        NativePacket(0x13 + 0x123, kMissionRoomList);
    mission_list[0x12] = 1;
    OnReceive(2, mission_list.data(), mission_list.size(),
              mission_body.data(), mission_body.size());
    Check(g_state.IsReady(2) &&
              g_state.Query(2, 3).has_value() &&
              g_state.Query(2, 3)->title ==
                  "mission lobby complete title",
          "0x824 mission Team-list response completes capability negotiation and seeds Team titles");
    for (uint8_t count : {uint8_t{0}, uint8_t{6}, uint8_t{10}}) {
        auto page =
            NativePacket(0x13 + static_cast<size_t>(count) * 0x123,
                         kMissionRoomList);
        page[0x12] = count;
        Check(ReceiveSize(kMissionRoomList, page.data(), page.size()) ==
                  page.size(),
              count == 0
                  ? "mission Team-list computes the zero-row native prefix"
                  : count == 6
                        ? "mission Team-list computes a six-row native prefix"
                        : "mission Team-list computes the ten-row native page maximum");
    }

    ResetRoot();
    Check(Ready(1, 100),
          "Hello followed by an empty capability batch makes the connection ready");
    const auto identity =
        Record(100, 5, 1, 1, "");
    Seed(1, 100, {identity});
    Check(g_state.Query(1, 5).has_value() &&
              g_state.Query(1, 5)->title.empty(),
          "identity-only room batch is retained for later full-title updates");

    ResetRoot();
    const std::wstring short_exact = L"Legacy room";
    Check(Prepare(ui::Surface::PvpCreate, short_exact) &&
              g_drafts.empty() && !g_state.HasPending(1) &&
              !g_saved[static_cast<size_t>(
                  ui::Surface::PvpCreate)]
                   .valid,
          "unnegotiated short CP936-roundtrip title keeps the exact legacy path");
    auto short_packet = DraftPacket(kCreate, short_exact);
    std::vector<uint8_t> replacement;
    Check(BeforeSend(1, short_packet.data(), short_packet.size(), 0,
                     &replacement) == transport::SendDecision::Pass &&
              replacement.empty(),
          "exact legacy fallback does not add metadata or pending state");

    const std::wstring legacy_long =
        L"Legacy title longer than twenty bytes";
    Check(!Prepare(ui::Surface::PvpCreate, legacy_long) &&
              g_drafts.empty() && !g_state.HasPending(1) &&
              !g_notices.empty(),
          "unnegotiated long title is explicitly rejected and preserved");
    auto long_packet = DraftPacket(kCreate, legacy_long);
    replacement.clear();
    Check(BeforeSend(1, long_packet.data(), long_packet.size(), 0,
                     &replacement) == transport::SendDecision::Pass &&
              replacement.empty() && !g_state.HasPending(1),
          "rejected unnegotiated long title is not transmitted or staged");

    const std::wstring not_roundtrip = EmojiTitle(1);
    Check(!Prepare(ui::Surface::PvpCreate, not_roundtrip) &&
              g_drafts.empty() && !g_state.HasPending(1),
          "unnegotiated Unicode that cannot round-trip through CP936 is explicitly rejected");

    std::wstring unpaired(1, static_cast<wchar_t>(0xD800));
    Check(!Prepare(ui::Surface::PvpCreate, unpaired) &&
              !Prepare(ui::Surface::PvpCreate, L"bad\ncontrol") &&
              g_drafts.empty() && !g_state.HasPending(1),
          "invalid UTF-16 and control-character titles are rejected before staging");
}

void TestCreateMissionAndPvpUpdates()
{
    ResetRoot();
    Check(Ready(10, 500), "create fixture negotiates capability");
    const std::wstring create_title =
        L"Create room full UTF-8 rules !ai count=4";
    Check(Prepare(ui::Surface::PvpCreate, create_title),
          "long create title prepares on a negotiated connection");
    PreparedSend created = SendDraft(10, kCreate, create_title);
    const std::string create_utf8 = Utf8Bytes(create_title);
    Check(created.decision == transport::SendDecision::Replace &&
              created.message.kind == metadata::MessageKind::SetTitle &&
              created.message.title == create_utf8 &&
              created.message.room_id == 0 &&
              OnlyTitleProjectionChanged(created, kCreate, create_title),
          "create Send keeps the complete UTF-8 title while changing only the native title field");
    AckBatch(10, 51, 500, 7, 1, create_utf8,
             created.message.request_id);
    std::wstring looked_up;
    Check(!g_state.HasPending(10) && g_drafts.count(10) == 0 &&
              LookupTitle(false, 51, &looked_up) &&
              looked_up == create_title &&
              StartDecision(10) == transport::SendDecision::Pass,
          "matching A33 batch saves the create title and restores battle start");

    ResetRoot();
    Check(Ready(20, 600, {Record(600, 5, 2, 1, "")}),
          "mission settings fixture receives room identity");
    g_fake_mission_room = 5;
    const std::wstring mission_title =
        L"任务规则：全员集合后开始，AI数量四";
    Check(Prepare(ui::Surface::MissionSettings, mission_title),
          "mission settings accepts a complete UTF-8 title");
    PreparedSend mission =
        SendDraft(20, kMissionSettings, mission_title);
    const std::string mission_utf8 = Utf8Bytes(mission_title);
    Check(mission.decision == transport::SendDecision::Replace &&
              mission.message.room_id == 5 &&
              mission.message.title == mission_utf8 &&
              OnlyTitleProjectionChanged(mission, kMissionSettings,
                                         mission_title),
          "mission settings preserves password, mode and tail outside its exact 21-byte title field");
    AckSnapshot(20, Record(600, 5, 2, 2, mission_utf8),
                mission.message.request_id);
    looked_up.clear();
    Check(LookupTitle(false, 5, &looked_up) &&
              looked_up == mission_title && !g_state.HasPending(20),
          "matching mission Snapshot saves the complete UTF-8 title");

    ResetRoot();
    const uint32_t battle_room = kBattleDomainForTest | 5;
    Check(Ready(30, 700,
                {Record(700, battle_room, 3, 1, "old battle")}),
          "PvP title fixture receives battle-domain identity");
    ActivatePvpRoom(30, 5);
    const std::wstring pvp_title =
        L"PvP BDB full title teams=2 score=10";
    Check(Prepare(ui::Surface::PvpTitleEdit, pvp_title),
          "PvP BDB edit prepares against the active battle-domain room");
    PreparedSend pvp = SendDraft(30, kPvpTitleEdit, pvp_title);
    const std::string pvp_utf8 = Utf8Bytes(pvp_title);
    Check(pvp.decision == transport::SendDecision::Replace &&
              pvp.message.room_id == battle_room &&
              pvp.message.title == pvp_utf8 &&
              OnlyTitleProjectionChanged(pvp, kPvpTitleEdit, pvp_title) &&
              pvp.native[6] == 1,
          "PvP BDB update preserves mode byte and changes only its native title field");
    AckSnapshot(30, Record(700, battle_room, 3, 2, pvp_utf8),
                pvp.message.request_id);
    looked_up.clear();
    Check(LookupTitle(true, 5, &looked_up) &&
              looked_up == pvp_title && !g_state.HasPending(30),
          "matching PvP Snapshot saves the complete UTF-8 title");
}

void TestProjectionBoundaries()
{
    ResetRoot();
    Check(Ready(40, 800), "projection fixture negotiates capability");
    const std::wstring chinese = L"一二三四五六七八九十甲";
    std::string chinese_legacy;
    Check(ui::LegacyTitleBytes(chinese, &chinese_legacy) &&
              chinese_legacy.size() == 22,
          "CP936 shim represents eleven Chinese characters as 22 bytes");
    Check(Prepare(ui::Surface::PvpCreate, chinese),
          "DBCS boundary title prepares successfully");
    PreparedSend dbcs = SendDraft(40, kCreate, chinese);
    const TitleField create_field = Field(kCreate);
    std::wstring first_ten = chinese.substr(0, 10);
    std::string first_ten_legacy;
    ui::LegacyTitleBytes(first_ten, &first_ten_legacy);
    Check(dbcs.decision == transport::SendDecision::Replace &&
              dbcs.native[create_field.length] == 20 &&
              std::memcmp(dbcs.native.data() + create_field.text,
                          first_ten_legacy.data(), 20) == 0 &&
              dbcs.message.title == Utf8Bytes(chinese),
          "projection stops on a complete CP936 DBCS boundary without splitting character bytes");

    ResetRoot();
    Check(Ready(41, 801), "maximum Unicode fixture negotiates capability");
    const std::wstring emoji = EmojiTitle(128);
    const std::string emoji_utf8 = Utf8Bytes(emoji);
    std::string emoji_legacy;
    Check(emoji.size() == 256 && emoji_utf8.size() == 512 &&
              ui::LegacyTitleBytes(emoji, &emoji_legacy) &&
              emoji_legacy.size() == 256,
          "CP936 shim exposes the native 256-byte low-eight-bit-zero builder case");
    Check(Prepare(ui::Surface::PvpCreate, emoji),
          "128-scalar 512-byte UTF-8 title is accepted at the protocol maximum");
    PreparedSend maximum = SendDraft(41, kCreate, emoji, 0);
    const metadata::TitleValidation validation =
        metadata::ValidateTitle(maximum.message.title.data(),
                                maximum.message.title.size());
    Check(maximum.original[create_field.length] == 0 &&
              maximum.decision == transport::SendDecision::Replace &&
              maximum.native[create_field.length] == 20 &&
              maximum.message.title == emoji_utf8 &&
              validation.ok() && validation.scalar_count == 128 &&
              validation.byte_count == 512 &&
              OnlyTitleProjectionChanged(maximum, kCreate, emoji),
          "packet-copy projection corrects the native low-eight-bit-zero length bug without losing full UTF-8");
}

void TestQueueFailureAndAcknowledgement()
{
    ResetRoot();
    Check(Ready(50, 900), "queue fixture negotiates capability");
    const std::wstring title =
        L"queue zero title remains one request";
    Check(Prepare(ui::Surface::PvpCreate, title),
          "queue fixture prepares a long create title");
    PreparedSend first = SendDraft(50, kCreate, title);
    AfterSend(50, true, first.replacement.data(),
              first.replacement.size(), 0);
    PreparedSend second = SendDraft(50, kCreate, title);
    Check(first.message.request_id != 0 &&
              second.message.request_id == first.message.request_id &&
              g_state.Pending(50)->status ==
                  titles::PendingStatus::Awaiting,
          "queue-zero fallback reuses the same staged request ID");

    g_notices.clear();
    Check(StartDecision(50) == transport::SendDecision::Reject &&
              !g_notices.empty(),
          "awaiting title blocks native 0x8A7 start and emits a notice");

    auto keepalive = NativePacket(12, 0x1234);
    AfterSend(50, false, keepalive.data(), keepalive.size(), -1);
    Check(g_state.Pending(50)->status ==
              titles::PendingStatus::Awaiting,
          "failed unrelated keepalive cannot fail a title pending request");

    metadata::Message wrong = second.message;
    ++wrong.request_id;
    std::vector<uint8_t> wrong_packet;
    Check(metadata::EncodePacket(
              second.view.native_data, second.view.native_size, wrong,
              &wrong_packet),
          "common protocol encodes a wrong-request failure fixture");
    AfterSend(50, false, wrong_packet.data(), wrong_packet.size(), -1);
    Check(g_state.Pending(50)->status ==
              titles::PendingStatus::Awaiting &&
              g_state.Pending(50)->request.request_id ==
                  first.message.request_id,
          "failed SetTitle with another request ID cannot fail the pending draft");

    AfterSend(50, false, second.replacement.data(),
              second.replacement.size(), -1);
    Check(g_state.Pending(50)->status ==
              titles::PendingStatus::Failed &&
              g_state.Pending(50)->request.title == Utf8Bytes(title) &&
              g_drafts.count(50) == 1 &&
              g_saved[static_cast<size_t>(
                  ui::Surface::PvpCreate)]
                  .valid,
          "matching transport failure preserves the exact draft for retry");
    Check(Prepare(ui::Surface::PvpCreate, title), "identical failed title can be resubmitted");
    const auto retry = SendDraft(50, kCreate, title);
    Check(retry.message.request_id == first.message.request_id &&
              g_state.Pending(50)->status == titles::PendingStatus::Awaiting,
          "user retry preserves the mutation nonce so a lost server ACK can be replayed");
    AckBatch(50, 33, 900, 1, 1, Utf8Bytes(title), retry.message.request_id);
    Check(!g_state.HasPending(50) && g_drafts.empty() &&
              StartDecision(50) == transport::SendDecision::Pass,
          "replayed confirmation resolves the retained draft and start gate");

    ResetRoot();
    Check(Ready(51, 901), "bare ACK fixture negotiates capability");
    const std::wstring bare_title =
        L"bare acknowledgement must not downgrade";
    Check(Prepare(ui::Surface::PvpCreate, bare_title),
          "bare ACK fixture prepares a title");
    PreparedSend bare = SendDraft(51, kCreate, bare_title);
    auto bare_ack = NativePacket(ReceiveSize(kCreateAck), kCreateAck);
    Put32(bare_ack.data() + 6, 61);
    OnReceive(51, bare_ack.data(), bare_ack.size(), nullptr, 0);
    Check(g_state.Pending(51)->status ==
              titles::PendingStatus::Failed &&
              g_drafts.count(51) == 1 &&
              StartDecision(51) == transport::SendDecision::Reject,
          "bare native A33 fails the draft and never silently falls back to a truncated title");

    ResetRoot();
    Check(Ready(52, 902), "matching ACK fixture negotiates capability");
    const std::wstring ack_title =
        L"matching ACK restores native start";
    Check(Prepare(ui::Surface::PvpCreate, ack_title),
          "matching ACK fixture prepares a title");
    PreparedSend ack = SendDraft(52, kCreate, ack_title);
    AckBatch(52, 62, 902, 8, 1, Utf8Bytes(ack_title),
             ack.message.request_id);
    Check(!g_state.HasPending(52) && g_drafts.count(52) == 0 &&
              StartDecision(52) == transport::SendDecision::Pass,
          "matching A33 SnapshotBatch completes pending work and restores start");
}

void TestSavedDraftAndDomains()
{
    ResetRoot();
    const uint32_t team_room = 5;
    const uint32_t battle_room = kBattleDomainForTest | 5;
    const uint32_t other_battle = kBattleDomainForTest | 6;
    Check(Ready(60, 1000,
                {Record(1000, team_room, 1, 1, "Team #5 full"),
                 Record(1000, battle_room, 2, 1, "Battle #5 full"),
                 Record(1000, other_battle, 3, 1,
                        "Battle #6 full")}),
          "saved-draft fixture seeds team and battle domains");
    ActivatePvpRoom(60, 5);
    const std::wstring saved =
        L"unsent saved title for battle five";
    Check(Prepare(ui::Surface::PvpTitleEdit, saved),
          "saved draft records its active connection and room");

    ui::FakeEdit same;
    OnInit(ui::Surface::PvpTitleEdit, nullptr, &same);
    Check(same.writes == 1 && same.text == saved,
          "saved draft refills only its matching connection and room");

    ActivatePvpRoom(60, 6);
    ui::FakeEdit other_room;
    OnInit(ui::Surface::PvpTitleEdit, nullptr, &other_room);
    Check(other_room.writes == 1 &&
              other_room.text == L"Battle #6 full" &&
              other_room.text != saved,
          "another room receives its confirmed cache instead of the saved draft");

    Check(Ready(61, 1001,
                {Record(1001, battle_room, 1, 1,
                        "Other connection battle #5")}),
          "second connection negotiates an independent cache");
    ActivatePvpRoom(61, 5);
    ui::FakeEdit other_connection;
    OnInit(ui::Surface::PvpTitleEdit, nullptr, &other_connection);
    Check(other_connection.writes == 1 &&
              other_connection.text ==
                  L"Other connection battle #5" &&
              other_connection.text != saved,
          "same native room number on another connection never receives the saved draft");

    g_activeConnection = 60;
    std::wstring team;
    std::wstring battle;
    Check(LookupTitle(false, 5, &team) &&
              LookupTitle(true, 5, &battle) &&
              team == L"Team #5 full" &&
              battle == L"Battle #5 full" &&
              team != battle,
          "Team #5 and battle-domain high-bit #5 remain distinct title keys");
}

void TestTitleEventNoSendAndServerError()
{
    struct FakeTitleBar
    {
        std::array<uint8_t, 0x108> prefix{};
        void* room = nullptr;
        void* missionRoom = nullptr;
    };
    struct FakeBdbEvent
    {
        std::array<uint8_t, 0x2C> prefix{};
        ui::FakeEdit title;
    };
    static_assert(offsetof(FakeTitleBar, room) == 0x108);
    static_assert(offsetof(FakeBdbEvent, title) == 0x2C);

    ResetRoot();
    const uint32_t battle_room = kBattleDomainForTest | 5;
    Check(Ready(70, 1100,
                {Record(1100, battle_room, 2, 1, "confirmed title")}),
          "title-event no-send fixture negotiates and seeds a Battle room");
    ActivatePvpRoom(70, 5);
    uint32_t native_room = 5;
    FakeTitleBar title_bar;
    title_bar.room = &native_room;
    FakeBdbEvent event;
    Put16(event.prefix.data() + 4, 0x0BDB);
    event.title.text = L"unchanged title event must not stage forever";
    g_title_event_result = 73;
    const int event_result =
        TitleEvent(&title_bar, nullptr, &event);
    Check(event_result == 73 && g_title_event_calls == 1 &&
              g_drafts.count(70) == 0 &&
              !g_state.HasPending(70) &&
              StartDecision(70) == transport::SendDecision::Pass,
          "PvP title event consumed without a native send clears its phantom draft and does not block battle start");

    ResetRoot();
    Check(Ready(71, 1101,
                {Record(1101, battle_room, 3, 1, "old title")}),
          "server-error fixture negotiates and seeds a Battle room");
    ActivatePvpRoom(71, 5);
    const std::wstring rejected_title =
        L"rejected full title remains editable !ai";
    Check(Prepare(ui::Surface::PvpTitleEdit, rejected_title),
          "server-error fixture prepares a full title");
    PreparedSend rejected =
        SendDraft(71, kPvpTitleEdit, rejected_title);
    metadata::Message error;
    error.kind = metadata::MessageKind::Error;
    error.error = metadata::RemoteError::Unauthorized;
    error.server_epoch = 1101;
    error.room_id = battle_room;
    error.generation = 3;
    error.revision = 1;
    error.request_id = rejected.message.request_id;
    std::vector<uint8_t> body;
    Check(metadata::EncodeMessageBody(error, &body),
          "common protocol encodes a matching server rejection");
    auto notice = NativePacket(ReceiveSize(kTitleNotice), kTitleNotice);
    g_notices.clear();
    OnReceive(71, notice.data(), notice.size(), body.data(), body.size());
    const auto pending = g_state.Pending(71);
    Check(pending &&
              pending->status == titles::PendingStatus::Failed &&
              pending->request.title == Utf8Bytes(rejected_title) &&
              pending->error.code == titles::ClientError::ServerRejected &&
              pending->error.remote_error ==
                  metadata::RemoteError::Unauthorized &&
              g_drafts.count(71) == 1 &&
              g_saved[static_cast<size_t>(
                  ui::Surface::PvpTitleEdit)]
                  .valid &&
              !g_notices.empty() &&
              StartDecision(71) == transport::SendDecision::Reject,
          "matching server rejection reports immediately while retaining the exact editable draft and blocking stale-rule start");
}

int NativeTitleApplyFixture(void* event)
{
    // Mirror only the verified host's compare/cache/send contract at A46380.
    const auto& text = reinterpret_cast<ui::FakeEdit*>(static_cast<uint8_t*>(event) + 0x2C)->text;
    if (ui::g_title_bar_cache == text) return 0;
    ui::g_title_bar_cache = text;
    auto packet = DraftPacket(kPvpTitleEdit, text);
    packet[6] = (g_activeRoom & kBattleDomainForTest) ? 1 : 2;
    std::vector<uint8_t> replacement;
    const auto decision = BeforeSend(g_activeConnection, packet.data(), packet.size(), 0, &replacement);
    metadata::TrailerView view;
    if (decision != transport::SendDecision::Replace ||
        !DecodeMessagePacket(replacement, &view, &g_title_event_message)) return 0;
    return 1;
}

void TestMissionTitleEventRetry()
{
    struct TitleBar { std::array<uint8_t, 0x108> prefix{}; void* battle = nullptr; void* mission = nullptr; };
    struct Event { std::array<uint8_t, 0x2C> prefix{}; ui::FakeEdit title; };
    ResetRoot();
    Check(Ready(91, 9001, {Record(9001, 5, 1, 1, "confirmed mission title"),
              Record(9001, kBattleDomainForTest | 5, 2, 1, "unrelated battle title")}),
          "shared titlebar fixture seeds both domains with the same native ID");
    uint32_t id = 5;
    TitleBar bar;
    bar.mission = &id;
    Event event;
    Put16(event.prefix.data() + 4, 0x0BDB);
    event.title.text = L"same mission draft after lost confirmation !ai3 lv4";
    ui::g_title_bar_cache = L"confirmed mission title";
    g_title_event_callback = &NativeTitleApplyFixture;
    Check(TitleEvent(&bar, nullptr, &event) == 1 && g_activeRoom == 5 &&
              g_title_event_message.room_id == 5 && g_state.HasPending(91),
          "mission titlebar uses subtype two and the Team identity instead of the colliding Battle ID");
    const auto originalRequest = g_title_event_message.request_id;
    g_state.AdvanceTime(91, GetTickCount64() + titles::kSetTitleTimeoutMilliseconds);
    Check(TitleEvent(&bar, nullptr, &event) == 1 &&
              g_title_event_message.request_id == originalRequest &&
              g_state.Pending(91)->status == titles::PendingStatus::Awaiting,
          "identical timed-out title restores the confirmed cache and sends the original mutation nonce");
    AckSnapshot(91, Record(9001, 5, 1, 2, Utf8Bytes(event.title.text)), originalRequest);
    Check(!g_state.HasPending(91) && g_state.Query(91, 5)->title == Utf8Bytes(event.title.text) &&
              g_state.Query(91, kBattleDomainForTest | 5)->title == "unrelated battle title",
          "replayed mission confirmation completes without changing another room domain");
}

void TestAutoCreateCapability()
{
    ResetRoot();
    auto native = NativePacket(0x53, 0x0640);
    native[6] = 2; native[7] = 10; native[0x15] = 12;
    std::memset(native.data() + 0x16, 0, 21);
    std::memcpy(native.data() + 0x16, "native title", 12);
    const auto original = native;
    std::vector<uint8_t> replacement;
    metadata::TrailerView view;
    metadata::Message hello;
    Check(BeforeSend(101, native.data(), native.size(), 0, &replacement) == transport::SendDecision::Replace &&
              DecodeMessagePacket(replacement, &view, &hello) && hello.kind == metadata::MessageKind::Hello &&
              std::equal(original.begin(), original.end(), view.native_data) && native == original,
          "mode-10 direct automatic create negotiates without a lobby visit or a changed native title");
    AckBatch(101, 5, 9876, 1, 1, "", hello.request_id);
    Check(g_state.IsReady(101) && g_state.Query(101, 5).has_value() && g_activeRoom == 5,
          "automatic create acknowledgement confirms capability and Team identity");
    const std::wstring title = L"full title in mode ten without entering another lobby !ai3";
    Check(Prepare(ui::Surface::PvpTitleEdit, title), "direct mode-10 room can edit a full title");
    auto rename = DraftPacket(0x089E, title);
    rename[6] = 2;
    Check(BeforeSend(101, rename.data(), rename.size(), 0, &replacement) == transport::SendDecision::Replace &&
              DecodeMessagePacket(replacement, &view, &hello) && hello.kind == metadata::MessageKind::SetTitle &&
              hello.room_id == 5,
          "direct mode-10 full rename uses the native mission subtype and confirmed identity");
    AckSnapshot(101, Record(9876, 5, 1, 2, Utf8Bytes(title)), hello.request_id);
    Check(BeforeSend(101, native.data(), native.size(), 0, &replacement) == transport::SendDecision::Replace &&
              DecodeMessagePacket(replacement, &view, &hello) && hello.kind == metadata::MessageKind::Hello,
          "automatic mission create rechecks capability when a native connection handle is reused");
    auto bare = NativePacket(ReceiveSize(kCreateAck), kCreateAck);
    Put32(bare.data() + 6, 6);
    OnReceive(101, bare.data(), bare.size(), nullptr, 0);
    Check(!g_state.IsReady(101) && !g_state.Query(101, 5),
          "bare auto-create ACK clears prior session metadata rather than displaying stale full titles");
}

void TestNegotiatingDraft()
{
    ResetRoot();
    Check(Ready(81, 1234), "refresh race fixture starts with confirmed capability");
    auto list = NativePacket(0x16, 0x0A08);
    std::vector<uint8_t> replacement;
    Check(BeforeSend(81, list.data(), list.size(), 0, &replacement) ==
              transport::SendDecision::Replace && g_state.IsNegotiating(81),
          "mission refresh tracks its outstanding capability offer");
    const std::wstring text = L"a full room title waiting for a refreshed list !ai3";
    Check(!Prepare(ui::Surface::PvpCreate, text) && !g_notices.empty() &&
              g_saved[static_cast<size_t>(ui::Surface::PvpCreate)].text == text &&
              g_drafts.empty() && !g_state.HasPending(81),
          "title submitted during refresh reports the wait before native UI closes and keeps text");
    metadata::TrailerView view;
    metadata::Message hello;
    Check(DecodeMessagePacket(replacement, &view, &hello), "refresh offer decodes");
    metadata::Message error;
    error.kind = metadata::MessageKind::Error;
    error.request_id = hello.request_id;
    error.server_epoch = 1234;
    error.error = metadata::RemoteError::InvalidRequest;
    std::vector<uint8_t> body;
    Check(metadata::EncodeMessageBody(error, &body), "mission list error body encodes");
    auto failedList = NativePacket(8, 0x0916);
    Check(ReceiveSize(0x0916, failedList.data(), failedList.size()) == 8,
          "native eight-byte mission list error is a supported carrier");
    OnReceive(81, failedList.data(), failedList.size(), body.data(), body.size());
    Check(!g_state.IsNegotiating(81) && !g_state.IsReady(81),
          "matching mission list rejection resolves negotiation without stale readiness");
    Check(Ready(81, 1234) && Prepare(ui::Surface::PvpCreate, text),
          "title can be retried once a later list confirms support");
    const auto sent = SendDraft(81, 0x0640, text);
    auto nativeError = NativePacket(8, 0x054E);
    Put16(nativeError.data() + 6, 0x101F);
    OnReceive(81, nativeError.data(), nativeError.size(), nullptr, 0);
    const auto pending = g_state.Pending(81);
    Check(sent.message.request_id && pending && pending->status == titles::PendingStatus::Failed &&
              pending->request.title == Utf8Bytes(text),
          "bare native create rejection fails the matching title without losing its text");
}

void RunTests()
{
    TestCapabilityAndLegacyFallback();
    TestCreateMissionAndPvpUpdates();
    TestProjectionBoundaries();
    TestQueueFailureAndAcknowledgement();
    TestSavedDraftAndDomains();
    TestTitleEventNoSendAndServerError();
    TestNegotiatingDraft();
    TestMissionTitleEventRetry();
    TestAutoCreateCapability();
    ResetRoot();
    const uint32_t room = kBattleDomainForTest | 5;
    const std::string full = "a descriptive title with its difficulty after byte twenty !ai3 hard";
    Check(Ready(111, 9002, {Record(9002, room, 1, 1, full)}),
          "AI title provider fixture confirms full metadata");
    auto joined = NativePacket(ReceiveSize(kPvpRoom), kPvpRoom);
    Put32(joined.data() + 6, 5);
    OnReceive(111, joined.data(), joined.size(), nullptr, 0);
    char copy[SDGO_ROOM_TITLE_MAX_UTF8_BYTES + 1] = {};
    InterlockedExchange(&g_currentTitleReady, 0);
    Check(SdgoRoomTitleCurrentV1(copy, sizeof(copy)) == 0,
          "uninitialized title provider never supplies data");
    InterlockedExchange(&g_currentTitleReady, 1);
    Check(SdgoRoomTitleCurrentV1(copy, sizeof(copy)) == full.size() && copy == full,
          "AI provider returns the confirmed current-room full title");
    std::memset(copy, 'x', sizeof(copy));
    Check(SdgoRoomTitleCurrentV1(copy, 8) == 0 && copy[0] == 'x' &&
              SdgoRoomTitleCurrentV1(nullptr, sizeof(copy)) == 0,
          "AI provider rejects short and null buffers without a partial title");
    Put32(joined.data() + 6, 6);
    OnReceive(111, joined.data(), joined.size(), nullptr, 0);
    Check(SdgoRoomTitleCurrentV1(copy, sizeof(copy)) == 0,
          "another room never inherits the previous room difficulty");
    Put32(joined.data() + 6, 5);
    OnReceive(111, joined.data(), joined.size(), nullptr, 0);
    auto left = NativePacket(2, 0x087A);
    OnReceive(111, left.data(), left.size(), nullptr, 0);
    Check(SdgoRoomTitleCurrentV1(copy, sizeof(copy)) == 0,
          "leaving the room stops the AI title provider");
}
}  // namespace

int main()
{
    RunTests();
    std::printf("%s: %d failure(s)\n",
                g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
