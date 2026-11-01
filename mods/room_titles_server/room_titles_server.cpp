// BattleServer integration for portable extended room titles. Native packets,
// room allocation and membership remain authoritative; this module only binds
// validated UTF-8 metadata to those native lifecycles.
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "sdgo/room_titles.h"
#include "sdgo/sdgo_api.h"
#include "room_titles_server_logic.h"

namespace {

namespace metadata = sdgo::room_metadata;
namespace server = sdgo::room_titles_server;

using DispatchFn = int(__cdecl*)(DWORD, DWORD, DWORD, void*, int);
using SendFn = int(__thiscall*)(void*, DWORD, const void*, int, DWORD);
using CreateCommitFn = int(__thiscall*)(void*, void*);
using RoomCreateFn = int(__thiscall*)(void*, void*);
using RoomCreateOneTeamFn = int(__thiscall*)(void*, void*, const void*);
using RoomCreateTwoTeamsFn =
    int(__thiscall*)(void*, void*, const void*, const void*);
using TeamSetHostFn = int(__thiscall*)(void*, DWORD, unsigned char,
                                      const void*, const void*, DWORD, DWORD);
using BattleSetHostFn = unsigned char(__thiscall*)(
    void*, DWORD, unsigned char, const void*, const void*, DWORD, DWORD);
using AddPilotFn = int(__thiscall*)(void*, const void*);
using RemovePilotFn = int(__thiscall*)(void*, DWORD, int*, int*);
using DestroyMembersFn = int(__thiscall*)(void*, void*, DWORD);
using DeleteRoomFn = int(__thiscall*)(void*, DWORD);
using DeletePilotFn = int(__thiscall*)(void*, DWORD);

DispatchFn g_dispatch = nullptr;
SendFn g_send = nullptr;
CreateCommitFn g_teamCreateCommit = nullptr;
RoomCreateFn g_battleCreate = nullptr;
RoomCreateOneTeamFn g_battleCreateMission = nullptr;
RoomCreateTwoTeamsFn g_battleCreateTeams = nullptr;
RoomCreateTwoTeamsFn g_battleCreateClan = nullptr;
TeamSetHostFn g_teamSetHost = nullptr;
BattleSetHostFn g_battleSetHost = nullptr;
AddPilotFn g_teamAddPilot = nullptr;
AddPilotFn g_battleAddPilot = nullptr;
RemovePilotFn g_teamRemovePilot = nullptr;
RemovePilotFn g_teamRemoveObserver = nullptr;
RemovePilotFn g_battleRemovePilot = nullptr;
RemovePilotFn g_battleRemoveObserver = nullptr;
DestroyMembersFn g_teamDestroyMembers = nullptr;
DestroyMembersFn g_battleDestroyMembers = nullptr;
DeleteRoomFn g_teamDeleteRoom = nullptr;
DeleteRoomFn g_battleDeleteRoom = nullptr;
DeletePilotFn g_teamDeletePilot = nullptr;
DeletePilotFn g_battleDeletePilot = nullptr;

void** g_teamPool = nullptr;
void** g_battlePool = nullptr;
std::unique_ptr<server::ServerState> g_state;
bool g_active = false;

constexpr std::uint16_t kCreateTeam = 0x0640;
constexpr std::uint16_t kListBattle = 0x07FB;
constexpr std::uint16_t kBattleSettings = 0x050F;
constexpr std::uint16_t kRenameRoom = 0x089E;
constexpr std::uint16_t kTeamSettings = 0x09ED;
constexpr std::uint16_t kJoinBattle = 0x0734;
constexpr std::uint16_t kBattleInfo = 0x052C;
constexpr std::uint16_t kMissionConversion = 0x0956;
constexpr std::uint16_t kPvpConversion = 0x056E;

constexpr std::size_t kTeamStride = 0x2218;
constexpr std::size_t kBattleStride = 0x32880;
constexpr std::size_t kTeamTitleLength = 0x53;
constexpr std::size_t kTeamTitleText = 0x54;
constexpr std::size_t kBattleTitleLength = 0x79;
constexpr std::size_t kBattleTitleText = 0x7A;
constexpr std::size_t kTeamHostPilot = 0x561;
constexpr std::size_t kBattleHostPilot = 0x4CE;
constexpr std::size_t kTeamMemberPilot = 0x9EF;
constexpr std::size_t kBattleMemberPilot = 0x95C;
constexpr std::size_t kPilotStride = 0x48E;
constexpr std::size_t kTeamMemberSlots = 6;
constexpr std::size_t kBattleMemberSlots = 12;

enum class ContextKind {
    None,
    Hello,
    Create,
    Update,
    NativeTitle,
    Join,
    Info,
    Conversion
};

struct DispatchContext {
    ContextKind kind = ContextKind::None;
    std::uint16_t opcode = 0;
    std::uint32_t pilot = 0;
    std::uint32_t wire_id = 0;
    const void* native_packet = nullptr;
    std::size_t native_size = 0;
    metadata::Message message;
    std::vector<std::uint8_t> request_body;
    server::NativeTitle native_title;
    bool has_native_title = false;
    bool extended = false;
    bool mutation = false;
    bool committed = false;
    bool request_completed = false;
};

struct SendGate {
    DispatchContext* context = nullptr;
    const void* packet = nullptr;
    DWORD destination = 0;
    std::uint16_t opcode = 0;
};

thread_local DispatchContext* g_context = nullptr;
thread_local SendGate g_sendGate;

std::uint16_t Word(const void* pointer)
{
    const auto* bytes = static_cast<const std::uint8_t*>(pointer);
    return static_cast<std::uint16_t>(
        bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8));
}

bool SafeDword(const void* pointer, std::uint32_t* output)
{
    if (!pointer || !output)
        return false;
    __try {
        std::memcpy(output, pointer, sizeof(*output));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeNativeTitle(const void* room,
                     std::size_t lengthOffset,
                     std::size_t textOffset,
                     server::NativeTitle* output)
{
    if (!room || !output)
        return false;
    __try {
        const auto* bytes = static_cast<const std::uint8_t*>(room);
        const std::uint8_t length = bytes[lengthOffset];
        if (length > output->bytes.size())
            return false;
        output->bytes.fill(0);
        output->size = length;
        if (length)
            std::memcpy(output->bytes.data(), bytes + textOffset, length);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeRequestTitle(const void* packet,
                      std::size_t packetSize,
                      std::size_t lengthOffset,
                      std::size_t textOffset,
                      server::NativeTitle* output)
{
    if (!packet || !output || lengthOffset >= packetSize ||
        textOffset > packetSize)
        return false;
    const auto* bytes = static_cast<const std::uint8_t*>(packet);
    const std::uint8_t length = bytes[lengthOffset];
    if (length > output->bytes.size() || textOffset + length > packetSize)
        return false;
    output->bytes.fill(0);
    output->size = length;
    if (length)
        std::memcpy(output->bytes.data(), bytes + textOffset, length);
    return true;
}

std::uint32_t SafeRoomId(const void* room)
{
    std::uint32_t value = 0;
    SafeDword(room, &value);
    return value;
}

std::uint32_t SafePilot(const void* room, std::size_t offset)
{
    std::uint32_t value = 0;
    const auto* bytes = static_cast<const std::uint8_t*>(room);
    SafeDword(bytes ? bytes + offset : nullptr, &value);
    return value;
}

void* SafePoolBase(void** slot)
{
    if (!slot)
        return nullptr;
    __try {
        return *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* NativeRoom(server::RoomDomain domain, std::uint32_t nativeId)
{
    if (!server::IsValidNativeRoomId(nativeId))
        return nullptr;
    void* base = SafePoolBase(domain == server::RoomDomain::Team ?
                              g_teamPool : g_battlePool);
    if (!base)
        return nullptr;
    const std::size_t stride =
        domain == server::RoomDomain::Team ? kTeamStride : kBattleStride;
    return static_cast<std::uint8_t*>(base) +
           static_cast<std::size_t>(nativeId - 1) * stride;
}

bool ReadRoomIdentity(server::RoomDomain domain,
                      const void* room,
                      std::uint32_t* nativeId,
                      std::uint32_t* owner,
                      server::NativeTitle* title)
{
    if (!room || !nativeId || !owner || !title)
        return false;
    *nativeId = SafeRoomId(room);
    if (!server::IsValidNativeRoomId(*nativeId))
        return false;
    const bool team = domain == server::RoomDomain::Team;
    *owner = SafePilot(room, team ? kTeamHostPilot : kBattleHostPilot);
    return *owner != 0 &&
           SafeNativeTitle(room,
                           team ? kTeamTitleLength : kBattleTitleLength,
                           team ? kTeamTitleText : kBattleTitleText,
                           title);
}

void SyncMembers(server::RoomDomain domain, const void* room)
{
    if (!g_state || !room)
        return;
    const std::uint32_t nativeId = SafeRoomId(room);
    const std::uint32_t wireId = server::EncodeRoomId(domain, nativeId);
    if (!wireId)
        return;
    std::array<std::uint32_t, kBattleMemberSlots + 1> pilots{};
    std::size_t count = 0;
    const bool team = domain == server::RoomDomain::Team;
    const std::uint32_t host =
        SafePilot(room, team ? kTeamHostPilot : kBattleHostPilot);
    if (host)
        pilots[count++] = host;
    const std::size_t slots =
        team ? kTeamMemberSlots : kBattleMemberSlots;
    const std::size_t first =
        team ? kTeamMemberPilot : kBattleMemberPilot;
    for (std::size_t index = 0; index < slots; ++index) {
        const std::uint32_t pilot =
            SafePilot(room, first + index * kPilotStride);
        if (!pilot ||
            std::find(pilots.begin(), pilots.begin() + count, pilot) !=
                pilots.begin() + count)
            continue;
        pilots[count++] = pilot;
    }
    g_state->ReplaceMembers(wireId, pilots.data(), count);
}

metadata::StateResult EnsureLegacyRoom(server::RoomDomain domain,
                                       const void* room)
{
    metadata::StateResult result;
    result.error = metadata::StateError::NotFound;
    if (!g_state)
        return result;
    std::uint32_t nativeId = 0;
    std::uint32_t owner = 0;
    server::NativeTitle title;
    if (!ReadRoomIdentity(domain, room, &nativeId, &owner, &title))
        return result;
    result = g_state->ObserveLegacyRoom(domain, nativeId, owner, title);
    if (result.ok())
        SyncMembers(domain, room);
    return result;
}

bool RoomForWire(std::uint32_t wireId, void** room)
{
    if (!room || !server::IsValidWireRoomId(wireId))
        return false;
    *room = NativeRoom(server::DecodeRoomDomain(wireId),
                       server::DecodeNativeRoomId(wireId));
    return *room != nullptr;
}

std::uint64_t MakeEpoch()
{
    std::uint64_t epoch = 0;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&epoch),
                        sizeof(epoch), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
        epoch == 0) {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        epoch = static_cast<std::uint64_t>(counter.QuadPart) ^
                (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32) ^
                GetTickCount64();
        if (epoch == 0)
            epoch = 1;
    }
    return epoch;
}

std::size_t ExactRequestBytes(std::uint16_t opcode)
{
    switch (opcode) {
    case kCreateTeam: return 0x53;
    case kListBattle: return 0x16;
    case kBattleSettings: return 0x37;
    case kRenameRoom: return 0x1D;
    case kTeamSettings: return 0x3E;
    case kJoinBattle: return 0x32;
    case kBattleInfo: return 0x0B;
    default: return 0;
    }
}

bool RequestTitle(std::uint16_t opcode,
                  const void* packet,
                  std::size_t size,
                  server::NativeTitle* title)
{
    switch (opcode) {
    case kCreateTeam:
        return SafeRequestTitle(packet, size, 0x15, 0x16, title);
    case kBattleSettings:
        return SafeRequestTitle(packet, size, 0x0B, 0x0C, title);
    case kTeamSettings:
        return SafeRequestTitle(packet, size, 0x12, 0x13, title);
    case kRenameRoom:
        return SafeRequestTitle(packet, size, 7, 8, title);
    default:
        return false;
    }
}

server::RoomDomain RequestDomain(std::uint16_t opcode,
                                 const void* packet)
{
    if (opcode == kBattleSettings)
        return server::RoomDomain::Battle;
    if (opcode == kRenameRoom) {
        const auto* bytes = static_cast<const std::uint8_t*>(packet);
        if (bytes && bytes[6] == 1)
            return server::RoomDomain::Battle;
    }
    return server::RoomDomain::Team;
}

bool IsTitleRequest(std::uint16_t opcode, const void* packet)
{
    if (opcode == kBattleSettings || opcode == kTeamSettings)
        return true;
    if (opcode != kRenameRoom || !packet)
        return false;
    const std::uint8_t type =
        static_cast<const std::uint8_t*>(packet)[6];
    return type == 1 || type == 2;
}

bool SameKey(const metadata::RoomKey& left,
             const metadata::Message& right)
{
    return left.server_epoch == right.server_epoch &&
           left.room_id == right.room_id &&
           left.generation == right.generation;
}

int DispatchScoped(DispatchFn original,
                   DWORD argument0,
                   DWORD argument1,
                   DWORD connection,
                   void* packet,
                   int size,
                   DispatchContext* context)
{
    DispatchContext* previous = g_context;
    g_context = context;
    __try {
        return original(argument0, argument1, connection, packet, size);
    } __finally {
        g_context = previous;
    }
}

int SendScoped(SendFn original,
               DispatchContext* context,
               void* self,
               DWORD destination,
               const void* packet,
               int size,
               DWORD flags,
               std::uint16_t opcode)
{
    const SendGate previous = g_sendGate;
    g_sendGate.context = context;
    g_sendGate.packet = packet;
    g_sendGate.destination = destination;
    g_sendGate.opcode = opcode;
    __try {
        return original(self, destination, packet, size, flags);
    } __finally {
        g_sendGate = previous;
    }
}

metadata::Message SnapshotMessage(const metadata::RoomSnapshot& snapshot,
                                  std::uint32_t requestId)
{
    metadata::Message message;
    message.kind = metadata::MessageKind::Snapshot;
    message.server_epoch = snapshot.key.server_epoch;
    message.room_id = snapshot.key.room_id;
    message.generation = snapshot.key.generation;
    message.revision = snapshot.revision;
    message.request_id = requestId;
    message.title = snapshot.title;
    return message;
}

bool ObserveRoomByWire(std::uint32_t wireId,
                       metadata::RoomSnapshot* snapshot)
{
    if (!g_state || !snapshot)
        return false;
    if (g_state->QueryRoom(wireId, snapshot))
        return true;
    void* room = nullptr;
    if (!RoomForWire(wireId, &room))
        return false;
    const metadata::StateResult observed =
        EnsureLegacyRoom(server::DecodeRoomDomain(wireId), room);
    if (!observed.ok())
        return false;
    *snapshot = observed.snapshot;
    return true;
}

bool CommitContext(DispatchContext* context,
                   const void* packet,
                   int size,
                   metadata::RoomSnapshot* snapshot)
{
    if (!context || !packet || size < 2 || !snapshot || !g_state)
        return false;
    if (context->committed)
        return context->wire_id &&
               g_state->QueryRoom(context->wire_id, snapshot);

    const std::uint16_t response = Word(packet);
    if (context->kind == ContextKind::Hello && response == 0x0617) {
        const auto* bytes = static_cast<const std::uint8_t*>(packet);
        if (size < 0x15)
            return false;
        std::array<std::uint32_t, 10> wireIds{};
        const std::size_t count =
            server::ParseBattleListWireIds(
                bytes, static_cast<std::size_t>(size),
                wireIds.data(), wireIds.size());
        for (std::size_t index = 0; index < count; ++index) {
            metadata::RoomSnapshot ignored;
            ObserveRoomByWire(wireIds[index], &ignored);
        }
        context->committed = true;
        return false;
    }

    if (context->kind == ContextKind::Create && response == 0x0A33) {
        std::uint32_t nativeId = 0;
        if (size < 10 ||
            !SafeDword(static_cast<const std::uint8_t*>(packet) + 6,
                       &nativeId))
            return false;
        void* room = NativeRoom(server::RoomDomain::Team, nativeId);
        std::uint32_t observedId = 0;
        std::uint32_t owner = 0;
        server::NativeTitle nativeTitle;
        if (!ReadRoomIdentity(server::RoomDomain::Team, room, &observedId,
                              &owner, &nativeTitle) ||
            observedId != nativeId || owner != context->pilot)
            return false;
        const std::string* full =
            context->extended ? &context->message.title : nullptr;
        metadata::StateResult created;
        if (!context->extended &&
            g_state->QueryRoom(
                server::EncodeRoomId(server::RoomDomain::Team, nativeId),
                &created.snapshot)) {
            created.error = metadata::StateError::None;
        } else {
            created =
                g_state->CreateRoom(server::RoomDomain::Team, nativeId,
                                    owner, nativeTitle, full, true);
        }
        if (!created.ok())
            return false;
        context->wire_id = created.snapshot.key.room_id;
        context->committed = true;
        SyncMembers(server::RoomDomain::Team, room);
        *snapshot = created.snapshot;
        return true;
    }

    if ((context->kind == ContextKind::Update ||
         context->kind == ContextKind::NativeTitle) &&
        ((context->opcode == kBattleSettings && response == 0x0902) ||
         (context->opcode == kTeamSettings &&
          (response == 0x0809 || response == 0x0717)) ||
         (context->opcode == kRenameRoom && response == 0x06BA))) {
        metadata::StateResult updated;
        if (context->extended) {
            updated = g_state->SetExtendedTitle(
                metadata::RoomKey{context->message.server_epoch,
                                  context->message.room_id,
                                  context->message.generation},
                context->pilot, context->message.revision,
                context->message.title, context->native_title);
        } else {
            updated = g_state->ObserveNativeTitle(context->wire_id,
                                                  context->native_title);
        }
        if (!updated.ok())
            return false;
        context->committed = true;
        *snapshot = updated.snapshot;
        return true;
    }

    if (context->kind == ContextKind::Join && response == 0x0550) {
        std::uint32_t nativeId = 0;
        if (size < 10 ||
            !SafeDword(static_cast<const std::uint8_t*>(packet) + 6,
                       &nativeId))
            return false;
        context->wire_id =
            server::EncodeRoomId(server::RoomDomain::Battle, nativeId);
        if (!ObserveRoomByWire(context->wire_id, snapshot))
            return false;
        g_state->AddMember(context->wire_id, context->pilot);
        context->committed = true;
        return true;
    }

    if (context->kind == ContextKind::Info && response == 0x091F) {
        if (!context->wire_id)
            context->wire_id = g_state->QueryPilotWireId(context->pilot);
        if (!ObserveRoomByWire(context->wire_id, snapshot))
            return false;
        context->committed = true;
        return true;
    }

    if (context->kind == ContextKind::Conversion &&
        (response == 0x0680 || response == 0x086F)) {
        std::uint32_t nativeId = 0;
        if (size < 10 ||
            !SafeDword(static_cast<const std::uint8_t*>(packet) + 6,
                       &nativeId))
            return false;
        context->wire_id =
            server::EncodeRoomId(server::RoomDomain::Battle, nativeId);
        if (!ObserveRoomByWire(context->wire_id, snapshot))
            return false;
        context->committed = true;
        return true;
    }
    return false;
}

bool BuildHelloBatch(DispatchContext* context,
                     const void* packet,
                     int size,
                     std::vector<std::uint8_t>* extended)
{
    if (!context || !packet || !extended || size < 0x15 || !g_state)
        return false;
    const auto* bytes = static_cast<const std::uint8_t*>(packet);
    std::array<std::uint32_t, 10> wireIds{};
    const std::size_t count =
        server::ParseBattleListWireIds(
            bytes, static_cast<std::size_t>(size),
            wireIds.data(), wireIds.size());
    const metadata::SnapshotBatch batch =
        g_state->SnapshotBatchForWireIds(context->message.request_id,
                                         wireIds.data(), count);
    return metadata::EncodeSnapshotBatchPacket(
        bytes, static_cast<std::size_t>(size), batch, extended);
}

bool BuildContextResponse(DispatchContext* context,
                          const void* packet,
                          int size,
                          bool actorRecipient,
                          std::vector<std::uint8_t>* extended)
{
    if (!context || !packet || !extended || size < 2 || !g_state)
        return false;
    metadata::RoomSnapshot snapshot;
    const bool hasSnapshot = CommitContext(context, packet, size, &snapshot);
    if (context->kind == ContextKind::Hello &&
        Word(packet) == 0x0617 && actorRecipient)
        return BuildHelloBatch(context, packet, size, extended);
    if (!hasSnapshot)
        return false;
    const std::uint32_t requestId =
        context->extended ?
        server::ResponseRequestId(
            context->pilot,
            actorRecipient ? context->pilot : 0,
            context->message.request_id) : 0;
    return metadata::EncodePacket(
        static_cast<const std::uint8_t*>(packet),
        static_cast<std::size_t>(size),
        SnapshotMessage(snapshot, requestId), extended);
}

std::uint32_t PacketPilot(DWORD destination,
                          const void* packet,
                          int size)
{
    std::uint32_t pilot = destination;
    if (packet && size >= 6) {
        std::uint32_t embedded = 0;
        if (SafeDword(static_cast<const std::uint8_t*>(packet) + 2,
                      &embedded) &&
            embedded)
            pilot = embedded;
    }
    return pilot;
}

std::uint32_t EmbeddedPilot(const void* packet, int size)
{
    std::uint32_t pilot = 0;
    if (packet && size >= 6)
        SafeDword(static_cast<const std::uint8_t*>(packet) + 2, &pilot);
    return pilot;
}

bool GenericSnapshot(const void* packet,
                     int size,
                     DWORD destination,
                     metadata::RoomSnapshot* snapshot)
{
    if (!packet || size < 2 || !snapshot || !g_state)
        return false;
    const std::uint16_t opcode = Word(packet);
    std::uint32_t wireId = 0;
    if ((opcode == 0x0550 || opcode == 0x0680 || opcode == 0x086F) &&
        size >= 10) {
        std::uint32_t nativeId = 0;
        SafeDword(static_cast<const std::uint8_t*>(packet) + 6, &nativeId);
        wireId =
            server::EncodeRoomId(server::RoomDomain::Battle, nativeId);
    } else {
        switch (opcode) {
        case 0x0A35:
        case 0x0788:
        case 0x0793:
        case 0x0902:
        case 0x091F:
            wireId = g_state->QueryPilotWireId(
                PacketPilot(destination, packet, size),
                server::RoomDomain::Battle);
            break;
        case 0x0717:
            wireId = g_state->QueryPilotWireId(
                PacketPilot(destination, packet, size),
                server::RoomDomain::Team);
            break;
        case 0x06BA:
        {
            const std::uint32_t pilot =
                PacketPilot(destination, packet, size);
            // Rename notifications share one opcode. Prefer the request's
            // explicit domain; unsolicited packets fall back to the room in
            // which the pilot is currently active.
            if (g_context && g_context->opcode == kRenameRoom)
                wireId = g_state->QueryPilotWireId(
                    pilot, RequestDomain(kRenameRoom,
                                         g_context->native_packet));
            else
                wireId = g_state->QueryPilotWireId(pilot);
            break;
        }
        default:
            return false;
        }
    }
    return ObserveRoomByWire(wireId, snapshot);
}

int ForwardSend(void* self,
                DWORD destination,
                const void* packet,
                int size,
                DWORD flags)
{
    if (!packet || size < 2)
        return g_send(self, destination, packet, size, flags);
    const std::uint16_t opcode = Word(packet);
    DispatchContext* context = g_context;
    if (g_sendGate.context == context && g_sendGate.packet == packet &&
        g_sendGate.destination == destination &&
        g_sendGate.opcode == opcode)
        return 0;

    try {
        std::vector<std::uint8_t> extended;
        bool recognized = false;
        const bool actorRecipient =
            context && EmbeddedPilot(packet, size) == context->pilot;
        if (context)
            recognized =
                BuildContextResponse(context, packet, size,
                                     actorRecipient, &extended);
        if (!recognized) {
            metadata::RoomSnapshot snapshot;
            if (GenericSnapshot(packet, size, destination, &snapshot)) {
                recognized = metadata::EncodePacket(
                    static_cast<const std::uint8_t*>(packet),
                    static_cast<std::size_t>(size),
                    SnapshotMessage(snapshot, 0), &extended);
            }
        }
        if (!recognized)
            return g_send(self, destination, packet, size, flags);
        if (extended.empty())
            return 0;
        const int result =
            SendScoped(g_send, context, self, destination, extended.data(),
                       static_cast<int>(extended.size()), flags, opcode);
        if (result > 0 && context) {
            if (context->kind == ContextKind::Hello)
                g_state->ConfirmCapability(context->pilot);
            if (context->mutation) {
                server::CachedReply reply;
                server::CachedReply* replyPointer = nullptr;
                if (actorRecipient) {
                    reply.packet = extended;
                    reply.sender =
                        reinterpret_cast<std::uintptr_t>(self);
                    reply.flags = flags;
                    replyPointer = &reply;
                }
                g_state->CompleteRequest(
                    context->pilot, context->message.request_id,
                    context->opcode, context->request_body.data(),
                    context->request_body.size(), replyPointer);
                context->request_completed = true;
            }
        }
        return result;
    } catch (const std::bad_alloc&) {
        // A recognized extension is atomic. Never leak the bare success packet.
        if (context)
            return 0;
        return g_send(self, destination, packet, size, flags);
    }
}

int __fastcall Send(void* self,
                    void*,
                    DWORD destination,
                    const void* packet,
                    int size,
                    DWORD flags)
{
    return ForwardSend(self, destination, packet, size, flags);
}

bool PrepareContext(DispatchContext* context,
                    std::uint16_t opcode,
                    std::uint32_t pilot,
                    const void* nativePacket,
                    std::size_t nativeSize,
                    const metadata::Message* message,
                    const std::uint8_t* body,
                    std::size_t bodySize)
{
    if (!context || !nativePacket || nativeSize < 2 || !pilot || !g_state)
        return false;
    context->opcode = opcode;
    context->pilot = pilot;
    context->native_packet = nativePacket;
    context->native_size = nativeSize;
    context->extended = message != nullptr;
    if (message)
        context->message = *message;
    if (bodySize)
        context->request_body.assign(body, body + bodySize);

    if (opcode == kListBattle) {
        if (!message || message->kind != metadata::MessageKind::Hello)
            return false;
        context->kind = ContextKind::Hello;
        return true;
    }
    if (opcode == kCreateTeam) {
        context->kind = ContextKind::Create;
        context->mutation = message != nullptr;
        context->has_native_title =
            RequestTitle(opcode, nativePacket, nativeSize,
                         &context->native_title);
        if (!context->has_native_title)
            return false;
        if (message &&
            (message->kind != metadata::MessageKind::SetTitle ||
             message->server_epoch || message->room_id ||
             message->generation || message->revision))
            return false;
        return true;
    }
    if (IsTitleRequest(opcode, nativePacket)) {
        context->kind = message ? ContextKind::Update :
                                  ContextKind::NativeTitle;
        context->mutation = message != nullptr;
        context->has_native_title =
            RequestTitle(opcode, nativePacket, nativeSize,
                         &context->native_title);
        if (!context->has_native_title)
            return false;
        const server::RoomDomain domain =
            RequestDomain(opcode, nativePacket);
        context->wire_id =
            g_state->QueryPilotWireId(pilot, domain);
        if (!context->wire_id)
            return message == nullptr;
        if (message) {
            metadata::RoomSnapshot current;
            if (message->kind != metadata::MessageKind::SetTitle ||
                message->room_id != context->wire_id ||
                !g_state->QueryRoom(context->wire_id, &current) ||
                !SameKey(current.key, *message) ||
                current.revision != message->revision ||
                current.owner_session != pilot)
                return false;
        }
        return true;
    }
    if (message)
        return false;
    if (opcode == kJoinBattle) {
        context->kind = ContextKind::Join;
        return true;
    }
    if (opcode == kBattleInfo) {
        context->kind = ContextKind::Info;
        return true;
    }
    if (opcode == kMissionConversion || opcode == kPvpConversion) {
        context->kind = ContextKind::Conversion;
        return true;
    }
    return false;
}

int Replay(const server::CachedReply& reply,
           std::uint32_t pilot)
{
    if (!g_send || reply.packet.empty() || !reply.sender)
        return 1;
    g_send(reinterpret_cast<void*>(reply.sender), pilot,
           reply.packet.data(), static_cast<int>(reply.packet.size()),
           reply.flags);
    return 1;
}

int __cdecl Dispatch(DWORD argument0,
                     DWORD argument1,
                     DWORD connection,
                     void* packet,
                     int size)
{
    if (!packet || size < 2 || !g_state)
        return g_dispatch(argument0, argument1, connection, packet, size);
    const std::uint16_t opcode = Word(packet);
    const std::size_t expected = ExactRequestBytes(opcode);
    const bool passiveVariable =
        opcode == kMissionConversion || opcode == kPvpConversion;
    if (!expected && !passiveVariable)
        return g_dispatch(argument0, argument1, connection, packet, size);

    try {
        std::uint32_t pilot = 0;
        if (size < 6 ||
            !SafeDword(static_cast<const std::uint8_t*>(packet) + 2,
                       &pilot) ||
            !pilot)
            return passiveVariable ?
                   g_dispatch(argument0, argument1, connection, packet, size) :
                   1;

        metadata::Message message;
        const metadata::Message* messagePointer = nullptr;
        metadata::TrailerView trailer;
        std::vector<std::uint8_t> native;
        const std::uint8_t* body = nullptr;
        std::size_t bodySize = 0;
        void* nativePacket = packet;
        std::size_t nativeSize = static_cast<std::size_t>(size);

        if (expected && static_cast<std::size_t>(size) != expected) {
            const metadata::TrailerParseResult parsed =
                metadata::ParseTrailer(
                    static_cast<const std::uint8_t*>(packet),
                    static_cast<std::size_t>(size));
            if (parsed.status != metadata::TrailerStatus::Valid ||
                parsed.view.native_size != expected ||
                !metadata::DecodeMessageBody(parsed.view.body_data,
                                             parsed.view.body_size,
                                             &message))
                return 1;
            native.assign(parsed.view.native_data,
                          parsed.view.native_data + parsed.view.native_size);
            nativePacket = native.data();
            nativeSize = native.size();
            body = parsed.view.body_data;
            bodySize = parsed.view.body_size;
            messagePointer = &message;
        } else if (expected && static_cast<std::size_t>(size) < expected) {
            return 1;
        }

        DispatchContext context;
        if (!PrepareContext(&context, opcode, pilot, nativePacket,
                            nativeSize, messagePointer, body, bodySize)) {
            if (messagePointer)
                return 1;
            return g_dispatch(argument0, argument1, connection, packet, size);
        }

        if (context.mutation) {
            const server::RequestDecision decision =
                g_state->BeginRequest(
                    pilot, message.request_id, opcode, body, bodySize);
            switch (decision.disposition) {
            case server::RequestDisposition::Proceed:
                break;
            case server::RequestDisposition::Replay:
                return Replay(decision.reply, pilot);
            default:
                return 1;
            }
        }

        DispatchScoped(g_dispatch, argument0, argument1, connection,
                       nativePacket, static_cast<int>(nativeSize), &context);
        if (context.mutation && !context.request_completed) {
            if (context.committed) {
                g_state->CompleteRequest(
                    pilot, message.request_id, opcode,
                    context.request_body.data(),
                    context.request_body.size(), nullptr);
            } else {
                g_state->AbortRequest(
                    pilot, message.request_id, opcode,
                    context.request_body.data(),
                    context.request_body.size());
            }
        }
        // The native dispatcher returns 1 for handled and rejected cases.
        return 1;
    } catch (const std::bad_alloc&) {
        // Extended packets fail closed; exact native packets keep native behavior.
        return static_cast<std::size_t>(size) == expected ?
               g_dispatch(argument0, argument1, connection, packet, size) : 1;
    }
}

void ObserveCreatedRoom(server::RoomDomain domain,
                        void* room,
                        bool replaceExisting)
{
    if (!g_state || !room)
        return;
    if (domain == server::RoomDomain::Team && g_context &&
        g_context->kind == ContextKind::Create &&
        g_context->extended)
        return;
    std::uint32_t nativeId = 0;
    std::uint32_t owner = 0;
    server::NativeTitle title;
    if (!ReadRoomIdentity(domain, room, &nativeId, &owner, &title))
        return;
    const metadata::StateResult created =
        g_state->CreateRoom(domain, nativeId, owner, title, nullptr,
                            replaceExisting);
    if (created.ok())
        SyncMembers(domain, room);
}

int __fastcall TeamCreateCommit(void* self, void*, void* room)
{
    const int result = g_teamCreateCommit(self, room);
    try {
        if (result > 0)
            ObserveCreatedRoom(server::RoomDomain::Team, room, true);
    } catch (const std::bad_alloc&) {
    }
    return result;
}

int __fastcall BattleCreate(void* self, void*, void* room)
{
    const int result = g_battleCreate(self, room);
    try {
        if (result > 0)
            ObserveCreatedRoom(server::RoomDomain::Battle, room, true);
    } catch (const std::bad_alloc&) {
    }
    return result;
}

void ObserveSourceTeam(const void* team)
{
    if (team)
        EnsureLegacyRoom(server::RoomDomain::Team, team);
}

void CloneMissionRoom(void* room, const void* team)
{
    if (!g_state || !room || !team)
        return;
    ObserveSourceTeam(team);
    const std::uint32_t teamId = SafeRoomId(team);
    std::uint32_t battleId = 0;
    std::uint32_t owner = 0;
    server::NativeTitle title;
    if (!ReadRoomIdentity(server::RoomDomain::Battle, room, &battleId,
                          &owner, &title))
        return;
    const metadata::StateResult cloned =
        g_state->CloneTeamToBattle(teamId, battleId, owner, title);
    if (cloned.ok())
        SyncMembers(server::RoomDomain::Battle, room);
}

int __fastcall BattleCreateMission(void* self,
                                   void*,
                                   void* room,
                                   const void* team)
{
    const int result = g_battleCreateMission(self, room, team);
    try {
        if (result > 0)
            CloneMissionRoom(room, team);
    } catch (const std::bad_alloc&) {
    }
    return result;
}

void ObservePvpRoom(void* room, const void* teamA, const void* teamB)
{
    ObserveSourceTeam(teamA);
    ObserveSourceTeam(teamB);
    if (!g_state || !room || !teamB)
        return;
    std::uint32_t battleId = 0;
    std::uint32_t battleOwner = 0;
    server::NativeTitle battleTitle;
    const std::uint32_t teamBId = SafeRoomId(teamB);
    const std::uint32_t teamBOwner =
        SafePilot(teamB, kTeamHostPilot);
    if (!ReadRoomIdentity(server::RoomDomain::Battle, room, &battleId,
                          &battleOwner, &battleTitle) ||
        !server::IsValidNativeRoomId(teamBId) || !teamBOwner)
        return;
    const metadata::StateResult cloned =
        g_state->PromotePvpBattle(teamBId, battleId, battleOwner,
                                  teamBOwner, battleTitle);
    if (cloned.ok())
        SyncMembers(server::RoomDomain::Battle, room);
}

int __fastcall BattleCreateTeams(void* self,
                                 void*,
                                 void* room,
                                 const void* teamA,
                                 const void* teamB)
{
    const int result = g_battleCreateTeams(self, room, teamA, teamB);
    try {
        if (result > 0)
            ObservePvpRoom(room, teamA, teamB);
    } catch (const std::bad_alloc&) {
    }
    return result;
}

int __fastcall BattleCreateClan(void* self,
                                void*,
                                void* room,
                                const void* teamA,
                                const void* teamB)
{
    const int result = g_battleCreateClan(self, room, teamA, teamB);
    try {
        if (result > 0)
            ObservePvpRoom(room, teamA, teamB);
    } catch (const std::bad_alloc&) {
    }
    return result;
}

bool CaptureKey(std::uint32_t wireId, metadata::RoomKey* key)
{
    if (!g_state || !wireId || !key)
        return false;
    return g_state->QueryRoomKey(wireId, key);
}

void AfterHost(server::RoomDomain domain, void* room, int result,
               const metadata::RoomKey* expectedKey)
{
    if (!g_state || !room || result <= 0 || !expectedKey)
        return;
    const std::uint32_t owner =
        SafePilot(room, domain == server::RoomDomain::Team ?
                  kTeamHostPilot : kBattleHostPilot);
    if (owner)
        g_state->TransferHost(*expectedKey, owner);
}

int __fastcall TeamSetHost(void* self, void*, DWORD pilot,
                           unsigned char length, const void* name,
                           const void* tail, DWORD a5, DWORD a6)
{
    metadata::RoomKey expectedKey;
    const bool hadKey = CaptureKey(
        server::EncodeRoomId(server::RoomDomain::Team,
                             SafeRoomId(self)),
        &expectedKey);
    const int result =
        g_teamSetHost(self, pilot, length, name, tail, a5, a6);
    try {
        AfterHost(server::RoomDomain::Team, self, result,
                  hadKey ? &expectedKey : nullptr);
    } catch (const std::bad_alloc&) {
    }
    return result;
}

unsigned char __fastcall BattleSetHost(
    void* self, void*, DWORD pilot, unsigned char length,
    const void* name, const void* tail, DWORD a5, DWORD a6)
{
    metadata::RoomKey expectedKey;
    const bool hadKey = CaptureKey(
        server::EncodeRoomId(server::RoomDomain::Battle,
                             SafeRoomId(self)),
        &expectedKey);
    const unsigned char result =
        g_battleSetHost(self, pilot, length, name, tail, a5, a6);
    try {
        AfterHost(server::RoomDomain::Battle, self, result,
                  hadKey ? &expectedKey : nullptr);
    } catch (const std::bad_alloc&) {
    }
    return result;
}

void AfterAdd(const metadata::RoomKey* expectedKey,
              const void* roomPilot, int result)
{
    if (!g_state || !expectedKey || !roomPilot || result <= 0)
        return;
    const std::uint32_t pilot = SafePilot(roomPilot, 0x429);
    if (pilot)
        g_state->AddMember(*expectedKey, pilot);
}

int __fastcall TeamAddPilot(void* self, void*, const void* pilot)
{
    metadata::RoomKey expectedKey;
    const bool hadKey = CaptureKey(
        server::EncodeRoomId(server::RoomDomain::Team,
                             SafeRoomId(self)),
        &expectedKey);
    const int result = g_teamAddPilot(self, pilot);
    try {
        AfterAdd(hadKey ? &expectedKey : nullptr, pilot, result);
    } catch (const std::bad_alloc&) {
    }
    return result;
}

int __fastcall BattleAddPilot(void* self, void*, const void* pilot)
{
    metadata::RoomKey expectedKey;
    const bool hadKey = CaptureKey(
        server::EncodeRoomId(server::RoomDomain::Battle,
                             SafeRoomId(self)),
        &expectedKey);
    const int result = g_battleAddPilot(self, pilot);
    try {
        AfterAdd(hadKey ? &expectedKey : nullptr, pilot, result);
    } catch (const std::bad_alloc&) {
    }
    return result;
}

void AfterRemove(server::RoomDomain domain, void* room,
                 const metadata::RoomKey* expectedKey,
                 std::uint32_t pilot, int result)
{
    if (!g_state || !expectedKey || !pilot || result == 0)
        return;
    g_state->RemoveMember(*expectedKey, pilot);
    const std::uint32_t currentId = SafeRoomId(room);
    if (result < 0 || currentId == 0) {
        g_state->DestroyRoom(*expectedKey);
        return;
    }
    const std::uint32_t owner =
        SafePilot(room, domain == server::RoomDomain::Team ?
                  kTeamHostPilot : kBattleHostPilot);
    if (owner)
        g_state->TransferHost(*expectedKey, owner);
}

int RemovePilot(RemovePilotFn original, server::RoomDomain domain,
                void* room, DWORD pilot, int* slot, int* hostSlot)
{
    metadata::RoomKey expectedKey;
    const bool hadKey = CaptureKey(
        server::EncodeRoomId(domain, SafeRoomId(room)),
        &expectedKey);
    const int result = original(room, pilot, slot, hostSlot);
    try {
        AfterRemove(domain, room,
                    hadKey ? &expectedKey : nullptr, pilot, result);
    } catch (const std::bad_alloc&) {
    }
    return result;
}

int __fastcall TeamRemovePilot(void* self, void*, DWORD pilot,
                               int* slot, int* hostSlot)
{
    return RemovePilot(g_teamRemovePilot, server::RoomDomain::Team,
                       self, pilot, slot, hostSlot);
}

int __fastcall TeamRemoveObserver(void* self, void*, DWORD pilot,
                                  int* slot, int* hostSlot)
{
    return RemovePilot(g_teamRemoveObserver, server::RoomDomain::Team,
                       self, pilot, slot, hostSlot);
}

int __fastcall BattleRemovePilot(void* self, void*, DWORD pilot,
                                 int* slot, int* hostSlot)
{
    return RemovePilot(g_battleRemovePilot, server::RoomDomain::Battle,
                       self, pilot, slot, hostSlot);
}

int __fastcall BattleRemoveObserver(void* self, void*, DWORD pilot,
                                    int* slot, int* hostSlot)
{
    return RemovePilot(g_battleRemoveObserver,
                       server::RoomDomain::Battle,
                       self, pilot, slot, hostSlot);
}

int DestroyMembers(DestroyMembersFn original,
                   server::RoomDomain domain,
                   void* room, void* proc, DWORD argument)
{
    metadata::RoomKey expectedKey;
    const bool hadKey = CaptureKey(
        server::EncodeRoomId(domain, SafeRoomId(room)),
        &expectedKey);
    const int result = original(room, proc, argument);
    try {
        if (hadKey && g_state)
            g_state->ReplaceMembers(expectedKey, nullptr, 0);
    } catch (const std::bad_alloc&) {
    }
    return result;
}

int __fastcall TeamDestroyMembers(void* self, void*,
                                  void* proc, DWORD argument)
{
    return DestroyMembers(g_teamDestroyMembers,
                          server::RoomDomain::Team,
                          self, proc, argument);
}

int __fastcall BattleDestroyMembers(void* self, void*,
                                    void* proc, DWORD argument)
{
    return DestroyMembers(g_battleDestroyMembers,
                          server::RoomDomain::Battle,
                          self, proc, argument);
}

int DeleteRoom(DeleteRoomFn original, server::RoomDomain domain,
               void* proc, DWORD nativeId)
{
    metadata::RoomKey expectedKey;
    const bool hadKey = CaptureKey(
        server::EncodeRoomId(domain, nativeId), &expectedKey);
    const int result = original(proc, nativeId);
    try {
        if (result > 0 && g_state && hadKey)
            g_state->DestroyRoom(expectedKey);
    } catch (const std::bad_alloc&) {
    }
    return result;
}

int __fastcall TeamDeleteRoom(void* self, void*, DWORD roomId)
{
    return DeleteRoom(g_teamDeleteRoom, server::RoomDomain::Team,
                      self, roomId);
}

int __fastcall BattleDeleteRoom(void* self, void*, DWORD roomId)
{
    return DeleteRoom(g_battleDeleteRoom,
                      server::RoomDomain::Battle, self, roomId);
}

int __fastcall TeamDeletePilot(void* self, void*, DWORD pilot)
{
    const std::uint32_t wireId =
        g_state ? g_state->QueryPilotWireId(
                      pilot, server::RoomDomain::Team) : 0;
    metadata::RoomKey expectedKey;
    const bool hadKey = CaptureKey(wireId, &expectedKey);
    const int result = g_teamDeletePilot(self, pilot);
    try {
        if (result > 0 && hadKey)
            g_state->RemoveMember(expectedKey, pilot);
    } catch (const std::bad_alloc&) {
    }
    return result;
}

int __fastcall BattleDeletePilot(void* self, void*, DWORD pilot)
{
    const std::uint32_t wireId =
        g_state ? g_state->QueryPilotWireId(
                      pilot, server::RoomDomain::Battle) : 0;
    metadata::RoomKey expectedKey;
    const bool hadKey = CaptureKey(wireId, &expectedKey);
    const int result = g_battleDeletePilot(self, pilot);
    try {
        if (result > 0 && hadKey)
            g_state->RemoveMember(expectedKey, pilot);
    } catch (const std::bad_alloc&) {
    }
    return result;
}

std::uint32_t __cdecl QueryForPilot(std::uint32_t pilot,
                                    char* utf8,
                                    std::uint32_t capacity)
{
    if (!g_active || !g_state)
        return 0;
    return g_state->CopyPilotTitle(pilot, utf8, capacity);
}

const SdgoRoomTitleApi g_titleApi = {
    sizeof(SdgoRoomTitleApi),
    SDGO_ROOM_TITLE_API_VERSION,
    &QueryForPilot
};

template <typename T>
bool Bind(const SdgoApi* api, SdgoMod* mod, const char* name, T* output)
{
    *output = reinterpret_cast<T>(api->Symbol(mod, name));
    return *output != nullptr;
}

template <typename T>
bool Hook(const SdgoApi* api, SdgoMod* mod, const char* name,
          void* detour, T* original)
{
    return api->HookCode(mod, name, detour,
                         reinterpret_cast<void**>(original)) != 0;
}

} // namespace

extern "C" __declspec(dllexport)
const SdgoRoomTitleApi* __cdecl SdgoRoomTitleApiV1()
{
    return g_active ? &g_titleApi : nullptr;
}

SDGO_MOD_ENTRY
{
    try {
        g_state = std::make_unique<server::ServerState>(MakeEpoch());
    } catch (const std::bad_alloc&) {
        return 0;
    }

    if (!Bind(api, mod, "room_titles.team_pool", &g_teamPool) ||
        !Bind(api, mod, "room_titles.battle_pool", &g_battlePool) ||
        !Hook(api, mod, "room_titles.battle_dispatch",
              reinterpret_cast<void*>(&Dispatch), &g_dispatch) ||
        !Hook(api, mod, "room_titles.battle_send_agent",
              reinterpret_cast<void*>(&Send), &g_send) ||
        !Hook(api, mod, "room_titles.team_create_commit",
              reinterpret_cast<void*>(&TeamCreateCommit),
              &g_teamCreateCommit) ||
        !Hook(api, mod, "room_titles.battle_create",
              reinterpret_cast<void*>(&BattleCreate), &g_battleCreate) ||
        !Hook(api, mod, "room_titles.battle_create_mission",
              reinterpret_cast<void*>(&BattleCreateMission),
              &g_battleCreateMission) ||
        !Hook(api, mod, "room_titles.battle_create_teams",
              reinterpret_cast<void*>(&BattleCreateTeams),
              &g_battleCreateTeams) ||
        !Hook(api, mod, "room_titles.battle_create_clan",
              reinterpret_cast<void*>(&BattleCreateClan),
              &g_battleCreateClan) ||
        !Hook(api, mod, "room_titles.team_set_host",
              reinterpret_cast<void*>(&TeamSetHost), &g_teamSetHost) ||
        !Hook(api, mod, "room_titles.battle_set_host",
              reinterpret_cast<void*>(&BattleSetHost),
              &g_battleSetHost) ||
        !Hook(api, mod, "room_titles.team_add_pilot",
              reinterpret_cast<void*>(&TeamAddPilot), &g_teamAddPilot) ||
        !Hook(api, mod, "room_titles.battle_add_pilot",
              reinterpret_cast<void*>(&BattleAddPilot),
              &g_battleAddPilot) ||
        !Hook(api, mod, "room_titles.team_remove_pilot",
              reinterpret_cast<void*>(&TeamRemovePilot),
              &g_teamRemovePilot) ||
        !Hook(api, mod, "room_titles.team_remove_observer",
              reinterpret_cast<void*>(&TeamRemoveObserver),
              &g_teamRemoveObserver) ||
        !Hook(api, mod, "room_titles.battle_remove_pilot",
              reinterpret_cast<void*>(&BattleRemovePilot),
              &g_battleRemovePilot) ||
        !Hook(api, mod, "room_titles.battle_remove_observer",
              reinterpret_cast<void*>(&BattleRemoveObserver),
              &g_battleRemoveObserver) ||
        !Hook(api, mod, "room_titles.team_destroy_members",
              reinterpret_cast<void*>(&TeamDestroyMembers),
              &g_teamDestroyMembers) ||
        !Hook(api, mod, "room_titles.battle_destroy_members",
              reinterpret_cast<void*>(&BattleDestroyMembers),
              &g_battleDestroyMembers) ||
        !Hook(api, mod, "room_titles.team_delete_room",
              reinterpret_cast<void*>(&TeamDeleteRoom),
              &g_teamDeleteRoom) ||
        !Hook(api, mod, "room_titles.battle_delete_room",
              reinterpret_cast<void*>(&BattleDeleteRoom),
              &g_battleDeleteRoom) ||
        !Hook(api, mod, "room_titles.team_delete_pilot",
              reinterpret_cast<void*>(&TeamDeletePilot),
              &g_teamDeletePilot) ||
        !Hook(api, mod, "room_titles.battle_delete_pilot",
              reinterpret_cast<void*>(&BattleDeletePilot),
              &g_battleDeletePilot))
        return 0;

    g_active = true;
    api->Log(mod, SDGO_LOG_INFO,
             "ready: authoritative Team/Battle room-title metadata");
    return 1;
}
