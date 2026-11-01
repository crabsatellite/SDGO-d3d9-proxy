#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../room_metadata/common/room_metadata_common.h"
#include "room_domain.h"

namespace sdgo {
namespace room_titles_server {

namespace metadata = room_metadata;

struct NativeTitle {
    std::array<std::uint8_t, 20> bytes{};
    std::uint8_t size = 0;
};

bool operator==(const NativeTitle& left, const NativeTitle& right);
bool operator!=(const NativeTitle& left, const NativeTitle& right);

std::size_t ParseBattleListWireIds(const std::uint8_t* packet,
                                   std::size_t packetSize,
                                   std::uint32_t* wireIds,
                                   std::size_t capacity);

struct CachedReply {
    std::vector<std::uint8_t> packet;
    std::uintptr_t sender = 0;
    std::uint32_t flags = 0;
};

enum class RequestDisposition {
    Proceed,
    Replay,
    DuplicateInFlight,
    Conflict,
    Unauthorized,
    Invalid
};

struct RequestDecision {
    RequestDisposition disposition = RequestDisposition::Invalid;
    CachedReply reply;
};

class ServerState {
public:
    explicit ServerState(std::uint64_t serverEpoch);

    std::uint64_t server_epoch() const;

    void ConfirmCapability(std::uint32_t pilot);
    bool HasCapability(std::uint32_t pilot) const;

    RequestDecision BeginRequest(std::uint32_t pilot,
                                 std::uint32_t requestId,
                                 std::uint16_t opcode,
                                 const std::uint8_t* body,
                                 std::size_t bodySize);
    void CompleteRequest(std::uint32_t pilot,
                         std::uint32_t requestId,
                         std::uint16_t opcode,
                         const std::uint8_t* body,
                         std::size_t bodySize,
                         const CachedReply* reply);
    void AbortRequest(std::uint32_t pilot,
                      std::uint32_t requestId,
                      std::uint16_t opcode,
                      const std::uint8_t* body,
                      std::size_t bodySize);

    metadata::StateResult CreateRoom(RoomDomain domain,
                                     std::uint32_t nativeId,
                                     std::uint32_t ownerPilot,
                                     const NativeTitle& nativeTitle,
                                     const std::string* fullTitle,
                                     bool replaceExisting);
    metadata::StateResult CloneTeamToBattle(std::uint32_t teamNativeId,
                                           std::uint32_t battleNativeId,
                                           std::uint32_t battleOwnerPilot,
                                           const NativeTitle& battleNativeTitle);
    metadata::StateResult PromotePvpBattle(std::uint32_t teamBNativeId,
                                          std::uint32_t battleNativeId,
                                          std::uint32_t battleOwnerPilot,
                                          std::uint32_t teamBOwnerPilot,
                                          const NativeTitle& battleNativeTitle);
    metadata::StateResult ObserveLegacyRoom(RoomDomain domain,
                                            std::uint32_t nativeId,
                                            std::uint32_t ownerPilot,
                                            const NativeTitle& nativeTitle);
    metadata::StateResult SetExtendedTitle(const metadata::RoomKey& key,
                                           std::uint32_t ownerPilot,
                                           std::uint32_t expectedRevision,
                                           const std::string& title,
                                           const NativeTitle& nativeTitle);
    metadata::StateResult ObserveNativeTitle(std::uint32_t wireId,
                                             const NativeTitle& nativeTitle);
    metadata::StateResult TransferHost(std::uint32_t wireId,
                                       std::uint32_t newOwnerPilot);
    metadata::StateResult TransferHost(const metadata::RoomKey& expectedKey,
                                       std::uint32_t newOwnerPilot);

    void AddMember(std::uint32_t wireId, std::uint32_t pilot);
    void AddMember(const metadata::RoomKey& expectedKey,
                   std::uint32_t pilot);
    void RemoveMember(std::uint32_t wireId, std::uint32_t pilot);
    void RemoveMember(const metadata::RoomKey& expectedKey,
                      std::uint32_t pilot);
    void ReplaceMembers(std::uint32_t wireId,
                        const std::uint32_t* pilots,
                        std::size_t count);
    void ReplaceMembers(const metadata::RoomKey& expectedKey,
                        const std::uint32_t* pilots,
                        std::size_t count);
    void DestroyRoom(std::uint32_t wireId);
    void DestroyRoom(const metadata::RoomKey& expectedKey);

    bool QueryRoom(std::uint32_t wireId,
                   metadata::RoomSnapshot* snapshot) const;
    bool QueryRoomKey(std::uint32_t wireId,
                      metadata::RoomKey* key) const;
    bool QueryPilot(std::uint32_t pilot,
                    metadata::RoomSnapshot* snapshot) const;
    std::uint32_t CopyPilotTitle(std::uint32_t pilot,
                                 char* utf8,
                                 std::uint32_t capacity) const;
    std::uint32_t QueryPilotWireId(std::uint32_t pilot) const;
    std::uint32_t QueryPilotWireId(std::uint32_t pilot,
                                   RoomDomain domain) const;
    bool QueryNativeTitle(std::uint32_t wireId,
                          NativeTitle* nativeTitle) const;
    metadata::SnapshotBatch SnapshotBatchForWireIds(
        std::uint32_t requestId,
        const std::uint32_t* wireIds,
        std::size_t count) const;

private:
    struct Record {
        metadata::RoomSnapshot snapshot;
        NativeTitle native_title;
        std::unordered_set<std::uint32_t> members;
    };

    struct RequestKey {
        std::uint32_t pilot = 0;
        std::uint32_t request_id = 0;

        bool operator==(const RequestKey& other) const
        {
            return pilot == other.pilot && request_id == other.request_id;
        }
    };

    struct RequestKeyHash {
        std::size_t operator()(const RequestKey& key) const;
    };

    struct RequestEntry {
        std::uint16_t opcode = 0;
        std::vector<std::uint8_t> body;
        bool complete = false;
        CachedReply reply;
    };

    struct PilotRooms {
        std::uint32_t team = 0;
        std::uint32_t battle = 0;
    };

    struct ReadLock {
        explicit ReadLock(SRWLOCK* lock);
        ~ReadLock();
        SRWLOCK* lock;
    };

    struct WriteLock {
        explicit WriteLock(SRWLOCK* lock);
        ~WriteLock();
        SRWLOCK* lock;
    };

    bool SameRequest(const RequestEntry& entry,
                     std::uint16_t opcode,
                     const std::uint8_t* body,
                     std::size_t bodySize) const;
    void PruneRequests(std::size_t maximumSize);
    metadata::StateResult MissingResult() const;
    void CacheCreatedRoom(const metadata::StateResult& result,
                          const NativeTitle& nativeTitle,
                          std::uint32_t ownerPilot);
    void RollBackCreatedRoom(const metadata::RoomKey& key,
                             std::uint32_t ownerPilot);
    bool IsCurrentKeyLocked(const metadata::RoomKey& key) const;
    void AddMemberLocked(std::uint32_t wireId, std::uint32_t pilot);
    void RemoveMemberLocked(std::uint32_t wireId, std::uint32_t pilot);
    void DestroyRoomLocked(std::uint32_t wireId);

    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    metadata::RoomStore store_;
    std::unordered_map<std::uint32_t, Record> rooms_;
    std::unordered_map<std::uint32_t, PilotRooms> pilot_rooms_;
    std::unordered_set<std::uint32_t> capabilities_;
    std::unordered_map<RequestKey, RequestEntry, RequestKeyHash> requests_;
    std::deque<RequestKey> request_order_;
};

} // namespace room_titles_server
} // namespace sdgo
