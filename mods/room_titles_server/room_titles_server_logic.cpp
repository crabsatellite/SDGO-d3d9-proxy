#include "room_titles_server_logic.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace sdgo {
namespace room_titles_server {

namespace {

constexpr std::size_t kMaximumRememberedRequests = 4096;

metadata::SnapshotRecord ToRecord(const metadata::RoomSnapshot& snapshot)
{
    metadata::SnapshotRecord record;
    record.key = snapshot.key;
    record.revision = snapshot.revision;
    record.title = snapshot.title;
    return record;
}

} // namespace

bool operator==(const NativeTitle& left, const NativeTitle& right)
{
    return left.size == right.size &&
           std::equal(left.bytes.begin(), left.bytes.begin() + left.size,
                      right.bytes.begin());
}

bool operator!=(const NativeTitle& left, const NativeTitle& right)
{
    return !(left == right);
}

std::size_t ParseBattleListWireIds(const std::uint8_t* packet,
                                   std::size_t packetSize,
                                   std::uint32_t* wireIds,
                                   std::size_t capacity)
{
    if (!packet || (!wireIds && capacity) || packetSize < 0x15)
        return 0;
    const std::size_t count =
        std::min<std::size_t>(packet[0x14], 10);
    const std::size_t available =
        std::min(count, (packetSize - 0x15) / 0x90);
    const std::size_t outputCount = std::min(available, capacity);
    for (std::size_t index = 0; index < outputCount; ++index) {
        std::uint32_t nativeId = 0;
        std::memcpy(&nativeId, packet + 0x15 + index * 0x90,
                    sizeof(nativeId));
        wireIds[index] =
            EncodeRoomId(RoomDomain::Battle, nativeId);
    }
    return outputCount;
}

ServerState::ReadLock::ReadLock(SRWLOCK* value) : lock(value)
{
    AcquireSRWLockShared(lock);
}

ServerState::ReadLock::~ReadLock()
{
    ReleaseSRWLockShared(lock);
}

ServerState::WriteLock::WriteLock(SRWLOCK* value) : lock(value)
{
    AcquireSRWLockExclusive(lock);
}

ServerState::WriteLock::~WriteLock()
{
    ReleaseSRWLockExclusive(lock);
}

std::size_t ServerState::RequestKeyHash::operator()(const RequestKey& key) const
{
    const std::uint64_t combined =
        (static_cast<std::uint64_t>(key.pilot) << 32) | key.request_id;
    return static_cast<std::size_t>(combined ^ (combined >> 33));
}

ServerState::ServerState(std::uint64_t serverEpoch) : store_(serverEpoch)
{
}

std::uint64_t ServerState::server_epoch() const
{
    return store_.server_epoch();
}

void ServerState::ConfirmCapability(std::uint32_t pilot)
{
    if (!pilot)
        return;
    WriteLock lock(&lock_);
    for (auto request = requests_.begin(); request != requests_.end();) {
        if (request->first.pilot == pilot)
            request = requests_.erase(request);
        else
            ++request;
    }
    capabilities_.insert(pilot);
}

bool ServerState::HasCapability(std::uint32_t pilot) const
{
    ReadLock lock(&lock_);
    return capabilities_.find(pilot) != capabilities_.end();
}

bool ServerState::SameRequest(const RequestEntry& entry,
                              std::uint16_t opcode,
                              const std::uint8_t* body,
                              std::size_t bodySize) const
{
    return entry.opcode == opcode && entry.body.size() == bodySize &&
           (bodySize == 0 || std::memcmp(entry.body.data(), body, bodySize) == 0);
}

RequestDecision ServerState::BeginRequest(std::uint32_t pilot,
                                          std::uint32_t requestId,
                                          std::uint16_t opcode,
                                          const std::uint8_t* body,
                                          std::size_t bodySize)
{
    RequestDecision decision;
    if (!pilot || !requestId || (!body && bodySize)) {
        decision.disposition = RequestDisposition::Invalid;
        return decision;
    }

    WriteLock lock(&lock_);
    if (capabilities_.find(pilot) == capabilities_.end()) {
        decision.disposition = RequestDisposition::Unauthorized;
        return decision;
    }

    const RequestKey key{pilot, requestId};
    const auto found = requests_.find(key);
    if (found != requests_.end()) {
        if (!SameRequest(found->second, opcode, body, bodySize)) {
            decision.disposition = RequestDisposition::Conflict;
        } else if (!found->second.complete) {
            decision.disposition = RequestDisposition::DuplicateInFlight;
        } else if (!found->second.reply.packet.empty()) {
            decision.disposition = RequestDisposition::Replay;
            decision.reply = found->second.reply;
        } else {
            decision.disposition = RequestDisposition::DuplicateInFlight;
        }
        return decision;
    }

    PruneRequests(kMaximumRememberedRequests - 1);
    if (requests_.size() >= kMaximumRememberedRequests) {
        decision.disposition = RequestDisposition::Invalid;
        return decision;
    }
    RequestEntry entry;
    entry.opcode = opcode;
    if (bodySize)
        entry.body.assign(body, body + bodySize);
    requests_.emplace(key, std::move(entry));
    decision.disposition = RequestDisposition::Proceed;
    return decision;
}

void ServerState::CompleteRequest(std::uint32_t pilot,
                                  std::uint32_t requestId,
                                  std::uint16_t opcode,
                                  const std::uint8_t* body,
                                  std::size_t bodySize,
                                  const CachedReply* reply)
{
    if (!pilot || !requestId || (!body && bodySize))
        return;
    WriteLock lock(&lock_);
    const auto found = requests_.find(RequestKey{pilot, requestId});
    if (found == requests_.end() ||
        !SameRequest(found->second, opcode, body, bodySize))
        return;
    if (!found->second.complete) {
        found->second.complete = true;
        request_order_.push_back(RequestKey{pilot, requestId});
    }
    if (reply)
        found->second.reply = *reply;
    PruneRequests(kMaximumRememberedRequests);
}

void ServerState::AbortRequest(std::uint32_t pilot,
                               std::uint32_t requestId,
                               std::uint16_t opcode,
                               const std::uint8_t* body,
                               std::size_t bodySize)
{
    if (!pilot || !requestId || (!body && bodySize))
        return;
    WriteLock lock(&lock_);
    const RequestKey key{pilot, requestId};
    const auto found = requests_.find(key);
    if (found != requests_.end() && !found->second.complete &&
        SameRequest(found->second, opcode, body, bodySize))
        requests_.erase(found);
}

void ServerState::PruneRequests(std::size_t maximumSize)
{
    while (requests_.size() > maximumSize && !request_order_.empty()) {
        const RequestKey key = request_order_.front();
        request_order_.pop_front();
        const auto found = requests_.find(key);
        if (found != requests_.end() && found->second.complete)
            requests_.erase(found);
    }
}

metadata::StateResult ServerState::MissingResult() const
{
    metadata::StateResult result;
    result.error = metadata::StateError::NotFound;
    return result;
}

bool ServerState::IsCurrentKeyLocked(
    const metadata::RoomKey& key) const
{
    const auto found = rooms_.find(key.room_id);
    return found != rooms_.end() &&
           found->second.snapshot.key.server_epoch ==
               key.server_epoch &&
           found->second.snapshot.key.generation == key.generation;
}

void ServerState::CacheCreatedRoom(const metadata::StateResult& result,
                                   const NativeTitle& nativeTitle,
                                   std::uint32_t ownerPilot)
{
    Record record;
    record.snapshot = result.snapshot;
    record.native_title = nativeTitle;
    const auto inserted =
        rooms_.emplace(result.snapshot.key.room_id, std::move(record));
    if (!inserted.second)
        throw std::bad_alloc();
    AddMemberLocked(result.snapshot.key.room_id, ownerPilot);
}

void ServerState::RollBackCreatedRoom(const metadata::RoomKey& key,
                                      std::uint32_t ownerPilot)
{
    const auto found = rooms_.find(key.room_id);
    if (found != rooms_.end())
        DestroyRoomLocked(key.room_id);
    else
        store_.DestroyRoom(key);
    const auto pilot = pilot_rooms_.find(ownerPilot);
    if (pilot != pilot_rooms_.end() &&
        !pilot->second.team && !pilot->second.battle)
        pilot_rooms_.erase(pilot);
}

metadata::StateResult ServerState::CreateRoom(RoomDomain domain,
                                              std::uint32_t nativeId,
                                              std::uint32_t ownerPilot,
                                              const NativeTitle& nativeTitle,
                                              const std::string* fullTitle,
                                              bool replaceExisting)
{
    const std::uint32_t wireId = EncodeRoomId(domain, nativeId);
    if (!wireId || !ownerPilot) {
        metadata::StateResult result;
        result.error = metadata::StateError::InvalidArgument;
        return result;
    }
    WriteLock lock(&lock_);
    if (replaceExisting && rooms_.find(wireId) != rooms_.end())
        DestroyRoomLocked(wireId);
    metadata::StateResult result =
        fullTitle && !fullTitle->empty() ?
        store_.CreateRoom(wireId, ownerPilot, *fullTitle) :
        store_.CreateLegacyRoom(wireId, ownerPilot);
    if (!result.ok())
        return result;
    try {
        CacheCreatedRoom(result, nativeTitle, ownerPilot);
    } catch (...) {
        RollBackCreatedRoom(result.snapshot.key, ownerPilot);
        throw;
    }
    return result;
}

metadata::StateResult ServerState::CloneTeamToBattle(
    std::uint32_t teamNativeId,
    std::uint32_t battleNativeId,
    std::uint32_t battleOwnerPilot,
    const NativeTitle& battleNativeTitle)
{
    const std::uint32_t sourceWire =
        EncodeRoomId(RoomDomain::Team, teamNativeId);
    const std::uint32_t targetWire =
        EncodeRoomId(RoomDomain::Battle, battleNativeId);
    if (!sourceWire || !targetWire || !battleOwnerPilot) {
        metadata::StateResult result;
        result.error = metadata::StateError::InvalidArgument;
        return result;
    }
    WriteLock lock(&lock_);
    const auto source = rooms_.find(sourceWire);
    if (rooms_.find(targetWire) != rooms_.end())
        DestroyRoomLocked(targetWire);
    metadata::StateResult result =
        source != rooms_.end() &&
        source->second.snapshot.owner_session == battleOwnerPilot &&
        !source->second.snapshot.title.empty() ?
        store_.CreateRoom(targetWire, battleOwnerPilot,
                          source->second.snapshot.title) :
        store_.CreateLegacyRoom(targetWire, battleOwnerPilot);
    if (!result.ok())
        return result;
    try {
        CacheCreatedRoom(result, battleNativeTitle, battleOwnerPilot);
    } catch (...) {
        RollBackCreatedRoom(result.snapshot.key, battleOwnerPilot);
        throw;
    }
    return result;
}

metadata::StateResult ServerState::PromotePvpBattle(
    std::uint32_t teamBNativeId,
    std::uint32_t battleNativeId,
    std::uint32_t battleOwnerPilot,
    std::uint32_t teamBOwnerPilot,
    const NativeTitle& battleNativeTitle)
{
    const std::uint32_t sourceWire =
        EncodeRoomId(RoomDomain::Team, teamBNativeId);
    const std::uint32_t targetWire =
        EncodeRoomId(RoomDomain::Battle, battleNativeId);
    if (!sourceWire || !targetWire || !battleOwnerPilot ||
        !teamBOwnerPilot) {
        metadata::StateResult result;
        result.error = metadata::StateError::InvalidArgument;
        return result;
    }
    WriteLock lock(&lock_);
    const auto source = rooms_.find(sourceWire);
    if (rooms_.find(targetWire) != rooms_.end())
        DestroyRoomLocked(targetWire);
    const bool inherit =
        battleOwnerPilot == teamBOwnerPilot &&
        source != rooms_.end() &&
        source->second.snapshot.owner_session == teamBOwnerPilot &&
        !source->second.snapshot.title.empty();
    metadata::StateResult result =
        inherit ?
        store_.CreateRoom(targetWire, battleOwnerPilot,
                          source->second.snapshot.title) :
        store_.CreateLegacyRoom(targetWire, battleOwnerPilot);
    if (!result.ok())
        return result;
    try {
        CacheCreatedRoom(result, battleNativeTitle, battleOwnerPilot);
    } catch (...) {
        RollBackCreatedRoom(result.snapshot.key, battleOwnerPilot);
        throw;
    }
    return result;
}

metadata::StateResult ServerState::ObserveLegacyRoom(
    RoomDomain domain,
    std::uint32_t nativeId,
    std::uint32_t ownerPilot,
    const NativeTitle& nativeTitle)
{
    const std::uint32_t wireId = EncodeRoomId(domain, nativeId);
    if (!wireId || !ownerPilot) {
        metadata::StateResult result;
        result.error = metadata::StateError::InvalidArgument;
        return result;
    }
    WriteLock lock(&lock_);
    const auto found = rooms_.find(wireId);
    if (found == rooms_.end()) {
        metadata::StateResult created =
            store_.CreateLegacyRoom(wireId, ownerPilot);
        if (!created.ok())
            return created;
        try {
            CacheCreatedRoom(created, nativeTitle, ownerPilot);
        } catch (...) {
            RollBackCreatedRoom(created.snapshot.key, ownerPilot);
            throw;
        }
        return created;
    }
    metadata::StateResult result;
    result.snapshot = found->second.snapshot;
    AddMemberLocked(wireId, ownerPilot);
    if (found->second.snapshot.owner_session != ownerPilot) {
        result = store_.TransferHost(found->second.snapshot.key,
                                     found->second.snapshot.owner_session,
                                     ownerPilot,
                                     found->second.snapshot.revision);
        if (!result.ok())
            return result;
        found->second.snapshot.owner_session =
            result.snapshot.owner_session;
        found->second.snapshot.revision = result.snapshot.revision;
    }
    if (found->second.native_title != nativeTitle) {
        result = store_.ClearNativeTitle(result.snapshot.key);
        if (!result.ok())
            return result;
        found->second.snapshot.title.clear();
        found->second.snapshot.revision = result.snapshot.revision;
        found->second.native_title = nativeTitle;
    }
    return result;
}

metadata::StateResult ServerState::SetExtendedTitle(
    const metadata::RoomKey& key,
    std::uint32_t ownerPilot,
    std::uint32_t expectedRevision,
    const std::string& title,
    const NativeTitle& nativeTitle)
{
    WriteLock lock(&lock_);
    const auto found = rooms_.find(key.room_id);
    if (found == rooms_.end())
        return MissingResult();
    std::string preparedTitle(title);
    metadata::StateResult result =
        store_.UpdateTitle(key, ownerPilot, expectedRevision, title);
    if (!result.ok())
        return result;
    found->second.snapshot.key = result.snapshot.key;
    found->second.snapshot.revision = result.snapshot.revision;
    found->second.snapshot.owner_session =
        result.snapshot.owner_session;
    found->second.snapshot.title.swap(preparedTitle);
    found->second.native_title = nativeTitle;
    return result;
}

metadata::StateResult ServerState::ObserveNativeTitle(
    std::uint32_t wireId,
    const NativeTitle& nativeTitle)
{
    WriteLock lock(&lock_);
    const auto found = rooms_.find(wireId);
    if (found == rooms_.end())
        return MissingResult();
    if (found->second.native_title == nativeTitle)
        return metadata::StateResult{metadata::StateError::None,
                                     found->second.snapshot};
    metadata::StateResult result =
        store_.ClearNativeTitle(found->second.snapshot.key);
    if (!result.ok())
        return result;
    found->second.snapshot.title.clear();
    found->second.snapshot.revision = result.snapshot.revision;
    found->second.native_title = nativeTitle;
    return result;
}

metadata::StateResult ServerState::TransferHost(
    std::uint32_t wireId,
    std::uint32_t newOwnerPilot)
{
    if (!newOwnerPilot) {
        metadata::StateResult result;
        result.error = metadata::StateError::InvalidArgument;
        return result;
    }
    WriteLock lock(&lock_);
    const auto found = rooms_.find(wireId);
    if (found == rooms_.end())
        return MissingResult();
    if (found->second.snapshot.owner_session == newOwnerPilot)
        return metadata::StateResult{metadata::StateError::None,
                                     found->second.snapshot};
    metadata::StateResult result =
        store_.TransferHost(found->second.snapshot.key,
                            found->second.snapshot.owner_session,
                            newOwnerPilot,
                            found->second.snapshot.revision);
    if (!result.ok())
        return result;
    found->second.snapshot.owner_session =
        result.snapshot.owner_session;
    found->second.snapshot.revision = result.snapshot.revision;
    return result;
}

metadata::StateResult ServerState::TransferHost(
    const metadata::RoomKey& expectedKey,
    std::uint32_t newOwnerPilot)
{
    if (!newOwnerPilot) {
        metadata::StateResult result;
        result.error = metadata::StateError::InvalidArgument;
        return result;
    }
    WriteLock lock(&lock_);
    if (!IsCurrentKeyLocked(expectedKey))
        return MissingResult();
    const auto found = rooms_.find(expectedKey.room_id);
    if (found->second.snapshot.owner_session == newOwnerPilot)
        return metadata::StateResult{metadata::StateError::None,
                                     found->second.snapshot};
    metadata::StateResult result =
        store_.TransferHost(expectedKey,
                            found->second.snapshot.owner_session,
                            newOwnerPilot,
                            found->second.snapshot.revision);
    if (!result.ok())
        return result;
    found->second.snapshot.owner_session =
        result.snapshot.owner_session;
    found->second.snapshot.revision = result.snapshot.revision;
    return result;
}

void ServerState::AddMember(std::uint32_t wireId, std::uint32_t pilot)
{
    if (!wireId || !pilot)
        return;
    WriteLock lock(&lock_);
    AddMemberLocked(wireId, pilot);
}

void ServerState::AddMember(const metadata::RoomKey& expectedKey,
                            std::uint32_t pilot)
{
    if (!pilot)
        return;
    WriteLock lock(&lock_);
    if (IsCurrentKeyLocked(expectedKey))
        AddMemberLocked(expectedKey.room_id, pilot);
}

void ServerState::AddMemberLocked(std::uint32_t wireId, std::uint32_t pilot)
{
    const auto room = rooms_.find(wireId);
    if (room == rooms_.end())
        return;
    const bool alreadyMember =
        room->second.members.find(pilot) != room->second.members.end();
    if (!alreadyMember)
        room->second.members.insert(pilot);
    PilotRooms* membership = nullptr;
    try {
        membership = &pilot_rooms_[pilot];
    } catch (...) {
        if (!alreadyMember)
            room->second.members.erase(pilot);
        throw;
    }
    std::uint32_t& slot =
        DecodeRoomDomain(wireId) == RoomDomain::Battle ?
        membership->battle : membership->team;
    if (slot && slot != wireId) {
        const auto oldRoom = rooms_.find(slot);
        if (oldRoom != rooms_.end())
            oldRoom->second.members.erase(pilot);
    }
    slot = wireId;
}

void ServerState::RemoveMemberLocked(std::uint32_t wireId,
                                     std::uint32_t pilot)
{
    const auto room = rooms_.find(wireId);
    if (room != rooms_.end())
        room->second.members.erase(pilot);
    const auto found = pilot_rooms_.find(pilot);
    if (found == pilot_rooms_.end())
        return;
    std::uint32_t& slot =
        DecodeRoomDomain(wireId) == RoomDomain::Battle ?
        found->second.battle : found->second.team;
    if (slot == wireId)
        slot = 0;
    if (!found->second.team && !found->second.battle)
        pilot_rooms_.erase(found);
}

void ServerState::RemoveMember(std::uint32_t wireId, std::uint32_t pilot)
{
    WriteLock lock(&lock_);
    RemoveMemberLocked(wireId, pilot);
}

void ServerState::RemoveMember(const metadata::RoomKey& expectedKey,
                               std::uint32_t pilot)
{
    WriteLock lock(&lock_);
    if (IsCurrentKeyLocked(expectedKey))
        RemoveMemberLocked(expectedKey.room_id, pilot);
}

void ServerState::ReplaceMembers(std::uint32_t wireId,
                                 const std::uint32_t* pilots,
                                 std::size_t count)
{
    if (!pilots && count)
        return;
    WriteLock lock(&lock_);
    const auto room = rooms_.find(wireId);
    if (room == rooms_.end())
        return;
    for (const std::uint32_t existing : room->second.members) {
        const auto mapped = pilot_rooms_.find(existing);
        if (mapped == pilot_rooms_.end())
            continue;
        std::uint32_t& slot =
            DecodeRoomDomain(wireId) == RoomDomain::Battle ?
            mapped->second.battle : mapped->second.team;
        if (slot == wireId)
            slot = 0;
        if (!mapped->second.team && !mapped->second.battle)
            pilot_rooms_.erase(mapped);
    }
    room->second.members.clear();
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint32_t pilot = pilots[index];
        if (!pilot)
            continue;
        AddMemberLocked(wireId, pilot);
    }
}

void ServerState::ReplaceMembers(
    const metadata::RoomKey& expectedKey,
    const std::uint32_t* pilots,
    std::size_t count)
{
    if (!pilots && count)
        return;
    WriteLock lock(&lock_);
    if (!IsCurrentKeyLocked(expectedKey))
        return;
    const auto room = rooms_.find(expectedKey.room_id);
    for (const std::uint32_t existing : room->second.members) {
        const auto mapped = pilot_rooms_.find(existing);
        if (mapped == pilot_rooms_.end())
            continue;
        std::uint32_t& slot =
            DecodeRoomDomain(expectedKey.room_id) ==
                    RoomDomain::Battle ?
            mapped->second.battle : mapped->second.team;
        if (slot == expectedKey.room_id)
            slot = 0;
        if (!mapped->second.team && !mapped->second.battle)
            pilot_rooms_.erase(mapped);
    }
    room->second.members.clear();
    for (std::size_t index = 0; index < count; ++index) {
        if (pilots[index])
            AddMemberLocked(expectedKey.room_id, pilots[index]);
    }
}

void ServerState::DestroyRoomLocked(std::uint32_t wireId)
{
    const auto found = rooms_.find(wireId);
    if (found == rooms_.end())
        return;
    for (const std::uint32_t pilot : found->second.members) {
        const auto mapped = pilot_rooms_.find(pilot);
        if (mapped == pilot_rooms_.end())
            continue;
        std::uint32_t& slot =
            DecodeRoomDomain(wireId) == RoomDomain::Battle ?
            mapped->second.battle : mapped->second.team;
        if (slot == wireId)
            slot = 0;
        if (!mapped->second.team && !mapped->second.battle)
            pilot_rooms_.erase(mapped);
    }
    store_.DestroyRoom(found->second.snapshot.key);
    rooms_.erase(found);
}

void ServerState::DestroyRoom(std::uint32_t wireId)
{
    WriteLock lock(&lock_);
    DestroyRoomLocked(wireId);
}

void ServerState::DestroyRoom(const metadata::RoomKey& expectedKey)
{
    WriteLock lock(&lock_);
    if (IsCurrentKeyLocked(expectedKey))
        DestroyRoomLocked(expectedKey.room_id);
}

bool ServerState::QueryRoom(std::uint32_t wireId,
                            metadata::RoomSnapshot* snapshot) const
{
    if (!snapshot)
        return false;
    ReadLock lock(&lock_);
    const auto found = rooms_.find(wireId);
    if (found == rooms_.end())
        return false;
    *snapshot = found->second.snapshot;
    return true;
}

bool ServerState::QueryRoomKey(std::uint32_t wireId,
                               metadata::RoomKey* key) const
{
    if (!key)
        return false;
    ReadLock lock(&lock_);
    const auto found = rooms_.find(wireId);
    if (found == rooms_.end())
        return false;
    *key = found->second.snapshot.key;
    return true;
}

bool ServerState::QueryPilot(std::uint32_t pilot,
                             metadata::RoomSnapshot* snapshot) const
{
    if (!snapshot)
        return false;
    ReadLock lock(&lock_);
    const auto mapped = pilot_rooms_.find(pilot);
    if (mapped == pilot_rooms_.end())
        return false;
    const std::uint32_t wireId =
        mapped->second.battle ? mapped->second.battle :
        mapped->second.team;
    const auto found = rooms_.find(wireId);
    if (found == rooms_.end())
        return false;
    *snapshot = found->second.snapshot;
    return true;
}

std::uint32_t ServerState::CopyPilotTitle(std::uint32_t pilot,
                                          char* utf8,
                                          std::uint32_t capacity) const
{
    if (!utf8 || capacity == 0)
        return 0;
    ReadLock lock(&lock_);
    const auto mapped = pilot_rooms_.find(pilot);
    if (mapped == pilot_rooms_.end())
        return 0;
    const std::uint32_t wireId =
        mapped->second.battle ? mapped->second.battle :
        mapped->second.team;
    const auto found = rooms_.find(wireId);
    if (found == rooms_.end() || found->second.snapshot.title.empty())
        return 0;
    const std::size_t bytes = found->second.snapshot.title.size();
    if (bytes + 1 > capacity)
        return 0;
    std::memcpy(utf8, found->second.snapshot.title.data(), bytes);
    utf8[bytes] = '\0';
    return static_cast<std::uint32_t>(bytes);
}

std::uint32_t ServerState::QueryPilotWireId(std::uint32_t pilot) const
{
    ReadLock lock(&lock_);
    const auto found = pilot_rooms_.find(pilot);
    if (found == pilot_rooms_.end())
        return 0;
    return found->second.battle ? found->second.battle :
           found->second.team;
}

std::uint32_t ServerState::QueryPilotWireId(
    std::uint32_t pilot,
    RoomDomain domain) const
{
    ReadLock lock(&lock_);
    const auto found = pilot_rooms_.find(pilot);
    if (found == pilot_rooms_.end())
        return 0;
    return domain == RoomDomain::Battle ?
           found->second.battle : found->second.team;
}

bool ServerState::QueryNativeTitle(std::uint32_t wireId,
                                   NativeTitle* nativeTitle) const
{
    if (!nativeTitle)
        return false;
    ReadLock lock(&lock_);
    const auto found = rooms_.find(wireId);
    if (found == rooms_.end())
        return false;
    *nativeTitle = found->second.native_title;
    return true;
}

metadata::SnapshotBatch ServerState::SnapshotBatchForWireIds(
    std::uint32_t requestId,
    const std::uint32_t* wireIds,
    std::size_t count) const
{
    metadata::SnapshotBatch batch;
    batch.server_epoch = store_.server_epoch();
    batch.request_id = requestId;
    if (!wireIds && count)
        return batch;
    ReadLock lock(&lock_);
    const std::size_t bounded =
        std::min(count, metadata::kMaximumSnapshotBatchEntries);
    batch.rooms.reserve(bounded);
    for (std::size_t index = 0; index < bounded; ++index) {
        const auto found = rooms_.find(wireIds[index]);
        if (found != rooms_.end())
            batch.rooms.push_back(ToRecord(found->second.snapshot));
    }
    return batch;
}

} // namespace room_titles_server
} // namespace sdgo
