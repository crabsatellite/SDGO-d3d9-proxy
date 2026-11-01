#include "room_titles_server_logic.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace server = sdgo::room_titles_server;
namespace metadata = sdgo::room_metadata;

namespace {

int g_failures = 0;

void Check(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", description);
    if (!condition)
        ++g_failures;
}

server::NativeTitle Native(const char* text)
{
    server::NativeTitle result;
    const std::size_t length =
        text ? std::min<std::size_t>(std::strlen(text), result.bytes.size()) : 0;
    result.size = static_cast<std::uint8_t>(length);
    if (length)
        std::memcpy(result.bytes.data(), text, length);
    return result;
}

bool SameKey(const metadata::RoomKey& left,
             const metadata::RoomKey& right)
{
    return left.server_epoch == right.server_epoch &&
           left.room_id == right.room_id &&
           left.generation == right.generation;
}

void TestDomainCodec()
{
    Check(server::EncodeRoomId(server::RoomDomain::Team, 7) == 7 &&
              server::EncodeRoomId(server::RoomDomain::Battle, 7) ==
                  0x80000007u,
          "Team and Battle native ID collisions encode as distinct wire IDs");
    Check(server::DecodeRoomDomain(7) == server::RoomDomain::Team &&
              server::DecodeRoomDomain(0x80000007u) ==
                  server::RoomDomain::Battle &&
              server::DecodeNativeRoomId(0x80000007u) == 7,
          "wire IDs decode to their native domain and ID");
    Check(!server::IsValidNativeRoomId(0) &&
              server::IsValidNativeRoomId(400) &&
              !server::IsValidNativeRoomId(401) &&
              server::EncodeRoomId(server::RoomDomain::Battle,
                                   0x80000001u) == 0,
          "codec enforces the verified 400-entry native pools");
    Check(server::ResponseRequestId(100, 100, 1) == 1 &&
              server::ResponseRequestId(100, 200, 1) == 0,
          "only the mutation actor receives its nonzero request ID");
}

void TestBattleListFixture()
{
    std::vector<std::uint8_t> packet(0x5B5, 0);
    packet[0] = 0x17;
    packet[1] = 0x06;
    packet[0x0E] = 9; // unrelated constructor DWORD low byte
    packet[0x14] = 2; // authoritative row count
    const std::uint32_t first = 5;
    const std::uint32_t second = 6;
    std::memcpy(packet.data() + 0x15, &first, sizeof(first));
    std::memcpy(packet.data() + 0x15 + 0x90, &second, sizeof(second));
    std::uint32_t wireIds[10] = {};
    const std::size_t count =
        server::ParseBattleListWireIds(packet.data(), packet.size(),
                                       wireIds, 10);
    Check(count == 2 &&
              wireIds[0] == 0x80000005u &&
              wireIds[1] == 0x80000006u,
          "0x617 fixture reads count at +0x14 and Battle rows at +0x15");
}

void TestCapabilityAndDedup()
{
    server::ServerState state(100);
    const std::uint8_t requestA[] = {1, 2, 3, 4};
    const std::uint8_t requestB[] = {1, 2, 3, 5};

    Check(state.BeginRequest(11, 9, 0x640, requestA,
                             sizeof(requestA)).disposition ==
              server::RequestDisposition::Unauthorized,
          "extended mutation is rejected before negotiated capability");
    state.ConfirmCapability(11);
    Check(state.BeginRequest(11, 9, 0x640, requestA,
                             sizeof(requestA)).disposition ==
              server::RequestDisposition::Proceed,
          "first authenticated request proceeds");
    Check(state.BeginRequest(11, 9, 0x640, requestA,
                             sizeof(requestA)).disposition ==
              server::RequestDisposition::DuplicateInFlight,
          "concurrent exact duplicate is blocked before native dispatch");
    Check(state.BeginRequest(11, 9, 0x640, requestB,
                             sizeof(requestB)).disposition ==
              server::RequestDisposition::Conflict,
          "reused request ID with different bytes is rejected");

    server::CachedReply reply;
    reply.packet = {0x33, 0x0A, 0x01};
    reply.sender = 0x1234;
    reply.flags = 7;
    state.CompleteRequest(11, 9, 0x640, requestA, sizeof(requestA),
                          &reply);
    const server::RequestDecision replay =
        state.BeginRequest(11, 9, 0x640, requestA, sizeof(requestA));
    Check(replay.disposition == server::RequestDisposition::Replay &&
              replay.reply.packet == reply.packet &&
              replay.reply.sender == reply.sender &&
              replay.reply.flags == reply.flags,
          "completed exact duplicate replays the cached native success carrier");

    Check(state.BeginRequest(11, 10, 0x50F, requestA,
                             sizeof(requestA)).disposition ==
              server::RequestDisposition::Proceed,
          "new request ID has its own in-flight entry");
    state.AbortRequest(11, 10, 0x50F, requestA, sizeof(requestA));
    Check(state.BeginRequest(11, 10, 0x50F, requestA,
                             sizeof(requestA)).disposition ==
              server::RequestDisposition::Proceed,
          "native rejection removes an uncommitted request for retry");
}

void TestLegacyAndNativeTitleRules()
{
    server::ServerState state(200);
    const server::NativeTitle shortA = Native("same native prefix");
    const metadata::StateResult legacy =
        state.ObserveLegacyRoom(server::RoomDomain::Team, 3, 101, shortA);
    Check(legacy.ok() && legacy.snapshot.title.empty() &&
              legacy.snapshot.revision == 1,
          "legacy native room registers an identity-only snapshot");

    const std::string full = "same native prefix with full rules !ai count=3";
    const metadata::StateResult extended =
        state.SetExtendedTitle(legacy.snapshot.key, 101,
                               legacy.snapshot.revision, full, shortA);
    Check(extended.ok() && extended.snapshot.title == full,
          "legacy room can later receive an authenticated extended title");

    const metadata::StateResult unchanged =
        state.ObserveNativeTitle(legacy.snapshot.key.room_id, shortA);
    Check(unchanged.ok() && unchanged.snapshot.title == full &&
              unchanged.snapshot.revision == extended.snapshot.revision,
          "bare settings with unchanged native short bytes preserve full title");

    const server::NativeTitle shortB = Native("renamed native");
    const metadata::StateResult cleared =
        state.ObserveNativeTitle(legacy.snapshot.key.room_id, shortB);
    Check(cleared.ok() && cleared.snapshot.title.empty() &&
              cleared.snapshot.revision ==
                  extended.snapshot.revision + 1 &&
              SameKey(cleared.snapshot.key, extended.snapshot.key),
          "changed bare native title clears extension without changing generation");
    const metadata::StateResult idempotent =
        state.ObserveNativeTitle(legacy.snapshot.key.room_id, shortB);
    Check(idempotent.ok() &&
              idempotent.snapshot.revision == cleared.snapshot.revision,
          "re-observing the same cleared native title is idempotent");
}

void TestMissionAndPvpAliases()
{
    server::ServerState state(300);
    const server::NativeTitle sharedPrefix =
        Native("12345678901234567890");
    const std::string teamAFull =
        "12345678901234567890 TeamA private suffix !ai count=1";
    const std::string teamBFull =
        "12345678901234567890 TeamB authoritative suffix !ai count=4";

    const metadata::StateResult teamA =
        state.CreateRoom(server::RoomDomain::Team, 8, 1001,
                         sharedPrefix, &teamAFull, true);
    const metadata::StateResult teamB =
        state.CreateRoom(server::RoomDomain::Team, 9, 2002,
                         sharedPrefix, &teamBFull, true);
    Check(teamA.ok() && teamB.ok(), "two extended Team rooms are created");

    const metadata::StateResult mission =
        state.CloneTeamToBattle(8, 8, 1001,
                                Native("mission native fixed"));
    Check(mission.ok() &&
              mission.snapshot.key.room_id == 0x80000008u &&
              mission.snapshot.title == teamAFull,
          "mission conversion copies the sole Team full title across domains");
    const metadata::StateResult missionMismatch =
        state.CloneTeamToBattle(8, 12, 9999,
                                Native("mission native fixed"));
    Check(missionMismatch.ok() && missionMismatch.snapshot.title.empty(),
          "mission host mismatch also remains identity-only");

    const metadata::StateResult pvp =
        state.PromotePvpBattle(9, 9, 2002, 2002,
                               Native("pvp native fixed"));
    Check(pvp.ok() &&
              pvp.snapshot.key.room_id == 0x80000009u &&
              pvp.snapshot.title == teamBFull &&
              pvp.snapshot.title != teamAFull,
          "PvP conversion selects TeamB full title despite identical native prefixes");

    const metadata::StateResult mismatch =
        state.PromotePvpBattle(9, 10, 9999, 2002,
                               Native("pvp native fixed"));
    Check(mismatch.ok() && mismatch.snapshot.title.empty(),
          "PvP host mismatch falls back to identity-only native semantics");

    const metadata::StateResult legacyTeam =
        state.ObserveLegacyRoom(server::RoomDomain::Team, 11, 3003,
                                Native("legacy matching"));
    const metadata::StateResult legacyPvp =
        state.PromotePvpBattle(11, 11, 3003, 3003,
                               Native("pvp native fixed"));
    Check(legacyTeam.ok() && legacyPvp.ok() &&
              legacyPvp.snapshot.title.empty(),
          "ordinary legacy matchmaking does not invent an extended AI rule");

    metadata::RoomSnapshot teamAtEight;
    metadata::RoomSnapshot battleAtEight;
    Check(state.QueryRoom(8, &teamAtEight) &&
              state.QueryRoom(0x80000008u, &battleAtEight) &&
              teamAtEight.title == teamAFull &&
              battleAtEight.title == teamAFull &&
              teamAtEight.key.room_id != battleAtEight.key.room_id,
          "equal raw native IDs never merge Team and Battle state");
}

void TestMembershipProviderAndHost()
{
    server::ServerState state(400);
    const std::string teamFull =
        "Team title retained while the pilot is in Battle";
    const metadata::StateResult team =
        state.CreateRoom(server::RoomDomain::Team, 20, 5001,
                         Native("Team title retained"), &teamFull, true);
    const std::string full =
        "provider title longer than twenty bytes !ai count=2";
    const metadata::StateResult created =
        state.CreateRoom(server::RoomDomain::Battle, 20, 5001,
                         Native("provider title"), &full, true);
    Check(team.ok() && created.ok(),
          "pilot can remain in native Team while entering Battle");
    state.AddMember(created.snapshot.key.room_id, 5002);

    char exact[128] = {};
    const std::uint32_t bytes =
        state.CopyPilotTitle(5002, exact,
                             static_cast<std::uint32_t>(sizeof(exact)));
    Check(bytes == full.size() && std::string(exact) == full,
          "provider prioritizes active Battle title over retained Team title");
    const metadata::StateResult observedTeam =
        state.ObserveLegacyRoom(server::RoomDomain::Team, 20, 5001,
                                Native("Team title retained"));
    Check(observedTeam.ok() &&
              state.CopyPilotTitle(5001, exact,
                                   static_cast<std::uint32_t>(sizeof(exact))) ==
                  full.size() &&
              std::string(exact) == full,
          "later Team observation cannot steal provider focus from Battle");
    char tooSmall[8] = {'x', '\0'};
    Check(state.CopyPilotTitle(5002, tooSmall,
                               static_cast<std::uint32_t>(sizeof(tooSmall))) ==
              0 &&
              tooSmall[0] == 'x',
          "provider never truncates into insufficient caller storage");

    const metadata::StateResult transferred =
        state.TransferHost(created.snapshot.key.room_id, 5002);
    Check(transferred.ok() &&
              transferred.snapshot.owner_session == 5002 &&
              transferred.snapshot.revision ==
                  created.snapshot.revision + 1,
          "native host transfer preserves identity and advances revision");
    state.RemoveMember(created.snapshot.key.room_id, 5001);
    Check(state.QueryPilotWireId(5001) == team.snapshot.key.room_id &&
              state.QueryPilotWireId(5002) ==
                  created.snapshot.key.room_id,
          "Battle removal falls back to Team while preserving remaining Battle member");

    state.DestroyRoom(created.snapshot.key.room_id);
    Check(state.QueryPilotWireId(5001) == team.snapshot.key.room_id &&
              state.CopyPilotTitle(
                  5001, exact,
                  static_cast<std::uint32_t>(sizeof(exact))) ==
                  teamFull.size() &&
              std::string(exact) == teamFull &&
              state.QueryPilotWireId(5002) == 0 &&
              state.CopyPilotTitle(5002, exact,
                                   static_cast<std::uint32_t>(sizeof(exact))) ==
                  0,
          "Battle destruction clears Battle membership and falls back to live Team");
}

void TestDestroyReuseAndBatches()
{
    server::ServerState state(500);
    const std::string first = "first generation extended title";
    const std::string second = "second generation extended title";
    const metadata::StateResult source1 =
        state.CreateRoom(server::RoomDomain::Team, 30, 7001,
                         Native("generation"), &first, true);
    const metadata::StateResult alias1 =
        state.CloneTeamToBattle(30, 30, 7001,
                                Native("mission fixed"));
    Check(source1.ok() && alias1.ok(), "first source and alias are created");
    state.DestroyRoom(source1.snapshot.key.room_id);
    state.DestroyRoom(alias1.snapshot.key.room_id);

    const metadata::StateResult source2 =
        state.CreateRoom(server::RoomDomain::Team, 30, 7002,
                         Native("generation"), &second, true);
    const metadata::StateResult alias2 =
        state.CloneTeamToBattle(30, 30, 7002,
                                Native("mission fixed"));
    Check(source2.ok() && alias2.ok() &&
              source2.snapshot.key.generation !=
                  source1.snapshot.key.generation &&
              alias2.snapshot.key.generation !=
                  alias1.snapshot.key.generation &&
              alias2.snapshot.title == second,
          "destroy and native ID reuse cannot revive an old alias generation");

    const std::uint32_t ids[] = {
        source2.snapshot.key.room_id,
        alias2.snapshot.key.room_id
    };
    const metadata::SnapshotBatch batch =
        state.SnapshotBatchForWireIds(77, ids, 2);
    Check(batch.server_epoch == 500 && batch.request_id == 77 &&
              batch.rooms.size() == 2 &&
              batch.rooms[0].key.room_id == 30 &&
              batch.rooms[1].key.room_id == 0x8000001Eu &&
              batch.rooms[1].title == second,
          "Hello/list batch keeps Team and Battle identities domain-separated");
}

} // namespace

int main()
{
    TestDomainCodec();
    TestBattleListFixture();
    TestCapabilityAndDedup();
    TestLegacyAndNativeTitleRules();
    TestMissionAndPvpAliases();
    TestMembershipProviderAndHost();
    TestDestroyReuseAndBatches();
    std::printf("%s (%d failures)\n",
                g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
