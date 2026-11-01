#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../mods/room_metadata/common/room_metadata_common.h"
#include "../mods/room_titles_server/room_titles_server.cpp"

namespace {

namespace metadata = sdgo::room_metadata;
namespace server = sdgo::room_titles_server;

enum class NativeScenario {
    TeamList,
    BattleList,
    CreateSuccess,
    NativeError,
    RenameSuccess,
    NoSend
};

int g_failures = 0;
int g_native_dispatches = 0;
NativeScenario g_scenario = NativeScenario::NoSend;
std::vector<std::uint8_t> g_native_input;
std::vector<std::uint8_t> g_sent;
DWORD g_sent_destination = 0;
DWORD g_sent_flags = 0;
void* g_sent_self = nullptr;

void Check(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", description);
    if (!condition)
        ++g_failures;
}

void Write16(std::uint8_t* output, std::uint16_t value)
{
    std::memcpy(output, &value, sizeof(value));
}

void Write32(std::uint8_t* output, std::uint32_t value)
{
    std::memcpy(output, &value, sizeof(value));
}

server::NativeTitle Native(const char* text)
{
    server::NativeTitle result;
    const std::size_t size =
        std::min<std::size_t>(std::strlen(text), result.bytes.size());
    result.size = static_cast<std::uint8_t>(size);
    std::memcpy(result.bytes.data(), text, size);
    return result;
}

int __fastcall CaptureSend(void* self,
                           void*,
                           DWORD destination,
                           const void* packet,
                           int size,
                           DWORD flags)
{
    g_sent_self = self;
    g_sent_destination = destination;
    g_sent_flags = flags;
    g_sent.assign(static_cast<const std::uint8_t*>(packet),
                  static_cast<const std::uint8_t*>(packet) + size);
    return 1;
}

int __cdecl NativeDispatch(DWORD,
                           DWORD,
                           DWORD connection,
                           void* packet,
                           int size)
{
    ++g_native_dispatches;
    g_native_input.assign(static_cast<const std::uint8_t*>(packet),
                          static_cast<const std::uint8_t*>(packet) + size);
    std::uint32_t pilot = 0;
    if (size >= 6)
        std::memcpy(&pilot,
                    static_cast<const std::uint8_t*>(packet) + 2,
                    sizeof(pilot));

    std::vector<std::uint8_t> response;
    switch (g_scenario) {
    case NativeScenario::TeamList:
        response.assign(0x13, 0);
        Write16(response.data(), 0x0824);
        break;
    case NativeScenario::BattleList:
        response.assign(0x5B5, 0);
        Write16(response.data(), 0x0617);
        break;
    case NativeScenario::CreateSuccess:
        g_state->ObserveLegacyRoom(server::RoomDomain::Team, 5, pilot, Native("native title"));
        response.assign(0x21FC, 0);
        Write16(response.data(), 0x0A33);
        Write32(response.data() + 6, 5);
        break;
    case NativeScenario::NativeError:
        response.assign(8, 0);
        Write16(response.data(), 0x0916);
        Write16(response.data() + 6, 0x101F);
        break;
    case NativeScenario::RenameSuccess:
        response.assign(28, 0);
        Write16(response.data(), 0x06BA);
        break;
    case NativeScenario::NoSend:
        return 1;
    }
    Write32(response.data() + 2, pilot);
    ForwardSend(reinterpret_cast<void*>(0x12345000), connection,
                response.data(), static_cast<int>(response.size()),
                0xABCDEF01);
    return 1;
}

void Reset(std::uint64_t epoch)
{
    g_state = std::make_unique<server::ServerState>(epoch);
    g_dispatch = &NativeDispatch;
    g_send = reinterpret_cast<SendFn>(&CaptureSend);
    g_native_dispatches = 0;
    g_scenario = NativeScenario::NoSend;
    g_native_input.clear();
    g_sent.clear();
    g_sent_destination = 0;
    g_sent_flags = 0;
    g_sent_self = nullptr;
}

std::vector<std::uint8_t> HelloPacket(std::uint16_t opcode,
                                      std::uint32_t pilot,
                                      std::uint32_t requestId)
{
    std::vector<std::uint8_t> native(opcode == kCreateTeam ? 0x53 : 0x16, 0);
    Write16(native.data(), opcode);
    Write32(native.data() + 2, pilot);
    if (opcode == kCreateTeam) {
        native[6] = 2; native[7] = 10;
        native[0x15] = 12;
        std::memcpy(native.data() + 0x16, "native title", 12);
    }
    metadata::Message hello;
    hello.kind = metadata::MessageKind::Hello;
    hello.request_id = requestId;
    std::vector<std::uint8_t> encoded;
    Check(metadata::EncodePacket(native.data(), native.size(),
                                 hello, &encoded),
          "Hello fixture encodes");
    return encoded;
}

void TestHello(std::uint16_t requestOpcode,
               NativeScenario scenario,
               std::uint16_t responseOpcode,
               const char* description)
{
    Reset(1000 + requestOpcode);
    constexpr std::uint32_t pilot = 0x10203040;
    constexpr DWORD route = 0x50001000;
    const std::vector<std::uint8_t> request =
        HelloPacket(requestOpcode, pilot, 77);
    g_scenario = scenario;
    Check(Dispatch(0, 0, route,
                   const_cast<std::uint8_t*>(request.data()),
                   static_cast<int>(request.size())) == 1 &&
              g_native_dispatches == 1 &&
              g_native_input.size() == 0x16 &&
              Word(g_native_input.data()) == requestOpcode,
          description);
    metadata::TrailerView trailer;
    metadata::SnapshotBatch batch;
    Check(!g_sent.empty() &&
              Word(g_sent.data()) == responseOpcode &&
              g_sent_destination == route &&
              metadata::DecodeSnapshotBatchPacket(
                  g_sent.data(), g_sent.size(), &trailer, &batch) &&
              batch.server_epoch == g_state->server_epoch() &&
              batch.request_id == 77 &&
              batch.rooms.empty(),
          "native list reply carries one empty authenticated snapshot batch");
}

void TestNativeError()
{
    Reset(2000);
    constexpr std::uint32_t pilot = 0x11223344;
    constexpr DWORD route = 0x50002000;
    const std::vector<std::uint8_t> request =
        HelloPacket(kListTeam, pilot, 88);
    g_scenario = NativeScenario::NativeError;
    Dispatch(0, 0, route,
             const_cast<std::uint8_t*>(request.data()),
             static_cast<int>(request.size()));

    metadata::TrailerView trailer;
    metadata::Message error;
    Check(metadata::DecodePacket(g_sent.data(), g_sent.size(),
                                 &trailer, &error) &&
              trailer.native_size == 8 &&
              Word(trailer.native_data) == 0x0916 &&
              error.kind == metadata::MessageKind::Error &&
              error.error == metadata::RemoteError::InvalidRequest &&
              error.request_id == 88,
          "native 8-byte rejection immediately completes Hello with RTMD Error");
}

void TestNativeErrorWhitelist()
{
    struct Pair {
        std::uint16_t request;
        std::uint16_t response;
    };
    const Pair pairs[] = {
        {kCreateTeam, 0x054E},
        {kListBattle, 0x0709},
        {kListTeam, 0x0916},
        {kBattleSettings, 0x09F4},
        {kRenameRoom, 0x07AC},
        {kTeamSettings, 0x08FB}
    };
    constexpr std::uint32_t pilot = 0x55667788;
    std::uint8_t packet[8] = {};
    Write32(packet + 2, pilot);
    Write16(packet + 6, 1);
    bool accepted = true;
    for (const Pair& pair : pairs) {
        DispatchContext context;
        context.extended = true;
        context.pilot = pilot;
        context.opcode = pair.request;
        Write16(packet, pair.response);
        accepted = accepted &&
                   NativeFailureCarrier(context, packet, sizeof(packet));
        Write16(packet, static_cast<std::uint16_t>(pair.response + 1));
        accepted = accepted &&
                   !NativeFailureCarrier(context, packet, sizeof(packet));
    }
    DispatchContext unrelated;
    unrelated.extended = true;
    unrelated.pilot = pilot;
    unrelated.opcode = kListTeam;
    Write16(packet, 0x1234);
    accepted = accepted &&
               !NativeFailureCarrier(unrelated, packet, sizeof(packet));
    Check(accepted,
          "only six verified request/error opcode pairs accept an 8-byte RTMD Error carrier");
}

void TestLegacyIsolationAndOversizedPassThrough()
{
    Reset(3000);
    constexpr std::uint32_t capablePilot = 100;
    constexpr std::uint32_t legacyPilot = 200;
    constexpr DWORD sharedRoute = 0x50003000;
    Check(g_state->BeginCapabilityRound(capablePilot, sharedRoute),
          "capable pilot is registered on the shared Agent route");

    std::vector<std::uint8_t> notice(28, 0);
    Write16(notice.data(), 0x06BA);
    Write32(notice.data() + 2, legacyPilot);
    ForwardSend(reinterpret_cast<void*>(0x11110000), sharedRoute,
                notice.data(), static_cast<int>(notice.size()), 9);
    Check(g_sent == notice,
          "legacy pilot on the same Agent route receives only the native packet");

    g_sent.clear();
    std::vector<std::uint8_t> oversized(0x16 + 7, 0x5A);
    Write16(oversized.data(), kListBattle);
    Write32(oversized.data() + 2, capablePilot);
    g_scenario = NativeScenario::NoSend;
    Dispatch(0, 0, sharedRoute, oversized.data(),
             static_cast<int>(oversized.size()));
    Check(g_native_dispatches == 1 &&
              g_native_input == oversized,
          "known-op oversized packet without RTMD is preserved for another Mod");

    Check(g_state->ForgetCapabilityIfDetached(capablePilot),
          "final detach removes the negotiated capability");
    g_sent.clear();
    Write32(notice.data() + 2, capablePilot);
    ForwardSend(reinterpret_cast<void*>(0x11110000), sharedRoute,
                notice.data(), static_cast<int>(notice.size()), 9);
    Check(g_sent == notice,
          "same-pilot legacy reconnect cannot inherit an old RTMD session");
}

std::vector<std::uint8_t> RenamePacket(
    std::uint32_t pilot,
    const metadata::RoomSnapshot& room,
    std::uint32_t requestId,
    const std::string& fullTitle)
{
    std::vector<std::uint8_t> nativePacket(0x1D, 0);
    Write16(nativePacket.data(), kRenameRoom);
    Write32(nativePacket.data() + 2, pilot);
    nativePacket[6] = 2;
    const server::NativeTitle nativeTitle =
        Native("12345678901234567890");
    nativePacket[7] = nativeTitle.size;
    std::memcpy(nativePacket.data() + 8, nativeTitle.bytes.data(),
                nativeTitle.size);

    metadata::Message request;
    request.kind = metadata::MessageKind::SetTitle;
    request.server_epoch = room.key.server_epoch;
    request.room_id = room.key.room_id;
    request.generation = room.key.generation;
    request.revision = room.revision;
    request.request_id = requestId;
    request.title = fullTitle;
    std::vector<std::uint8_t> encoded;
    Check(metadata::EncodePacket(nativePacket.data(), nativePacket.size(),
                                 request, &encoded),
          "suffix-only rename fixture encodes");
    return encoded;
}

void TestSuffixOnlyRenameAndReplay()
{
    Reset(4000);
    constexpr std::uint32_t pilot = 333;
    constexpr DWORD route = 0x50004000;
    const std::string first =
        "12345678901234567890 first rule suffix !ai count=1";
    const std::string second =
        "12345678901234567890 second rule suffix !ai count=4";
    const metadata::StateResult created =
        g_state->CreateRoom(server::RoomDomain::Team, 7, pilot,
                            Native("12345678901234567890"),
                            &first, true);
    Check(created.ok() &&
              g_state->BeginCapabilityRound(pilot, route),
          "extended Team room is ready for native-wrapper rename");

    const std::vector<std::uint8_t> request =
        RenamePacket(pilot, created.snapshot, 99, second);
    g_scenario = NativeScenario::RenameSuccess;
    Dispatch(0, 0, route,
             const_cast<std::uint8_t*>(request.data()),
             static_cast<int>(request.size()));

    metadata::RoomSnapshot updated;
    metadata::TrailerView trailer;
    metadata::Message response;
    Check(g_state->QueryRoom(created.snapshot.key.room_id, &updated) &&
              updated.title == second &&
              updated.revision == created.snapshot.revision + 1 &&
              metadata::DecodePacket(g_sent.data(), g_sent.size(),
                                     &trailer, &response) &&
              response.kind == metadata::MessageKind::Snapshot &&
              response.request_id == 99 &&
              response.title == second,
          "two full titles sharing one 20-byte native prefix update by full suffix");

    const int nativeBeforeReplay = g_native_dispatches;
    g_sent.clear();
    Dispatch(0, 0, route,
             const_cast<std::uint8_t*>(request.data()),
             static_cast<int>(request.size()));
    if (g_native_dispatches != nativeBeforeReplay ||
        g_sent.empty() ||
        g_sent_destination != route ||
        g_sent_self != reinterpret_cast<void*>(0x12345000)) {
        std::printf("diag replay native=%d expected=%d sent=%zu dest=%08lX self=%p\n",
                    g_native_dispatches, nativeBeforeReplay, g_sent.size(),
                    static_cast<unsigned long>(g_sent_destination),
                    g_sent_self);
    }
    Check(g_native_dispatches == nativeBeforeReplay &&
              !g_sent.empty() &&
              g_sent_destination == route &&
              g_sent_self == reinterpret_cast<void*>(0x12345000),
          "exact retry replays the cached carrier on its original native route");
}

void TestAutoCreateHello()
{
    Reset(778);
    g_scenario = NativeScenario::CreateSuccess;
    auto packet = HelloPacket(kCreateTeam, 1001, 1);
    Dispatch(0, 0, 123, packet.data(), static_cast<int>(packet.size()));
    metadata::TrailerView view;
    metadata::SnapshotBatch batch;
    Check(g_native_dispatches == 1 && g_native_input.size() == 0x53 &&
              g_native_input[7] == 10 && g_native_input[0x15] == 12 &&
              metadata::DecodeSnapshotBatchPacket(g_sent.data(), g_sent.size(), &view, &batch) &&
              view.native_size == 0x21FC && batch.request_id == 1 && batch.server_epoch == 778 &&
              batch.rooms.size() == 1 && batch.rooms[0].key.room_id == 5 && batch.rooms[0].title.empty(),
          "mode-10 auto-create bootstraps capability and identity without replacing the generated native title");

    Reset(779);
    Check(g_state->BeginCapabilityRound(1001, 123), "prior unjoined login capability fixture");
    g_scenario = NativeScenario::TeamList;
    std::vector<std::uint8_t> legacy(0x16, 0);
    Write16(legacy.data(), kListTeam); Write32(legacy.data() + 2, 1001);
    Dispatch(0, 0, 123, legacy.data(), static_cast<int>(legacy.size()));
    Check(!g_state->HasCapability(1001) && g_sent.size() == 0x13,
          "legacy lobby entry on the same pilot and Agent route clears stale unjoined capability");
}

} // namespace

int main()
{
    TestHello(kListTeam, NativeScenario::TeamList, 0x0824,
              "mission Hello strips RTMD before native 0xA08 dispatch");
    TestHello(kListBattle, NativeScenario::BattleList, 0x0617,
              "battle Hello strips RTMD before native 0x7FB dispatch");
    TestNativeError();
    TestNativeErrorWhitelist();
    TestLegacyIsolationAndOversizedPassThrough();
    TestSuffixOnlyRenameAndReplay();
    TestAutoCreateHello();
    std::printf("%s (%d failures)\n",
                g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
