#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace pvp_ai::special_budget {

constexpr std::uint16_t kRequestOpcode = 0x04BE;
constexpr std::uint16_t kNotificationOpcode = 0x07BF;
constexpr std::uint16_t kAbsoluteSpCommand = 0x0018;
constexpr std::uint32_t kNpcHandleBit = 0x80000000u;
constexpr std::int32_t kFullSp = 30000;
constexpr std::int32_t kAcceptedCost = -10000;
constexpr std::int32_t kCompletionCost = -20000;

#pragma pack(push, 1)
// Packet received by BattleServer ReqSpecialAttack (0x004C3820). The
// connection layer supplies senderKey at +0x02 before this handler runs.
struct ServerRequestPacket {
    std::uint16_t opcode = kRequestOpcode;
    std::uint32_t senderKey = 0;
    std::uint32_t attackerHandle = 0;
    std::uint32_t weaponId = 0;
    std::uint32_t extra = 0;
};

// PT_NTF_SPECIAL_ATTACK (0x07BF), produced by BattleServer 0x004C3C92 and
// consumed by GOnline 0x004C3270.
struct NotificationPacket {
    std::uint16_t opcode = kNotificationOpcode;
    std::uint32_t reserved = 0;
    std::uint32_t attackerHandle = 0;
    std::uint32_t weaponId = 0;
    std::uint32_t extra = 0;
    std::uint32_t absoluteSp = 0;
};

// Native CUnit object command 0x18. GOnline 0x0068853E reads absoluteSp,
// clamps it to [0, 30000], and performs the native skill/UI refresh.
struct AbsoluteSpMessage {
    std::uint16_t command = kAbsoluteSpCommand;
    std::uint16_t reserved02 = 0;
    std::uint32_t reserved04 = 0;
    std::uint32_t reserved08 = 0;
    std::uint32_t reserved0C = 0;
    std::uint32_t absoluteSp = 0;
};
#pragma pack(pop)

static_assert(sizeof(ServerRequestPacket) == 0x12);
static_assert(offsetof(ServerRequestPacket, senderKey) == 0x02);
static_assert(offsetof(ServerRequestPacket, attackerHandle) == 0x06);
static_assert(offsetof(ServerRequestPacket, weaponId) == 0x0A);
static_assert(offsetof(ServerRequestPacket, extra) == 0x0E);
static_assert(sizeof(NotificationPacket) == 0x16);
static_assert(offsetof(NotificationPacket, attackerHandle) == 0x06);
static_assert(offsetof(NotificationPacket, weaponId) == 0x0A);
static_assert(offsetof(NotificationPacket, extra) == 0x0E);
static_assert(offsetof(NotificationPacket, absoluteSp) == 0x12);
static_assert(sizeof(AbsoluteSpMessage) == 0x14);
static_assert(offsetof(AbsoluteSpMessage, absoluteSp) == 0x10);

constexpr bool IsNpcHandle(std::uint32_t handle) {
    return (handle & kNpcHandleBit) != 0;
}

constexpr std::uint32_t ClampSp(std::int64_t value) {
    return value <= 0 ? 0u
         : value >= kFullSp ? static_cast<std::uint32_t>(kFullSp)
                            : static_cast<std::uint32_t>(value);
}

constexpr AbsoluteSpMessage MakeAbsoluteSpMessage(std::uint32_t serverSp) {
    AbsoluteSpMessage message{};
    message.absoluteSp = ClampSp(serverSp);
    return message;
}

struct BotKey {
    std::uintptr_t room = 0;
    std::uint32_t generation = 0;
    std::uint32_t pilotId = 0;
};

constexpr bool SameBot(const BotKey& a, const BotKey& b) {
    return a.room == b.room && a.generation == b.generation &&
           a.pilotId == b.pilotId;
}

struct Request {
    BotKey bot{};
    // Integration-owned stable identity for one AI intent. Re-entrant hooks
    // and retransmission handling must reuse it; zero is never accepted.
    std::uint64_t receiptId = 0;
    std::uint32_t weaponId = 0;
    std::uint32_t extra = 0;
};

enum class ReserveResult : std::uint8_t {
    Ready = 0,
    Invalid,
    NotNpc,
    InsufficientSp,
    Busy,
    Replay,
    Full,
};

enum class Phase : std::uint8_t {
    Idle = 0,
    Reserved,
    Accepted,
};

struct Charge {
    bool apply = false;
    std::int32_t delta = 0;
    BotKey bot{};
    std::uint64_t receiptId = 0;
};

struct StateView {
    bool active = false;
    Phase phase = Phase::Idle;
    BotKey bot{};
    std::uint64_t receiptId = 0;
    std::uint32_t weaponId = 0;
    std::uint32_t extra = 0;
};

// This controller prevents the mod's added NPC costs from being applied twice.
// It deliberately does not replace the native 0x004C3820 admission checks:
// reserve before the original handler, resolve with its exact return value,
// then apply only the returned native CalcUnitSp delta.
//
// There is no nonce in the native 0x04BE/0x07BF packets. receiptId therefore
// comes from the authoritative AI intent/session ledger; packet bytes alone
// cannot provide cryptographic replay protection.
template <std::size_t MaxBots = 480, std::size_t History = 8>
class Controller {
public:
    ReserveResult Reserve(const Request& request, std::uint32_t nativeSp) {
        if (!Valid(request))
            return ReserveResult::Invalid;
        if (!IsNpcHandle(request.bot.pilotId))
            return ReserveResult::NotNpc;
        if (nativeSp < static_cast<std::uint32_t>(kFullSp))
            return ReserveResult::InsufficientSp;

        State* state = Find(request.bot);
        if (state) {
            if (state->phase != Phase::Idle) {
                return state->receiptId == request.receiptId
                           ? ReserveResult::Replay
                           : ReserveResult::Busy;
            }
            if (Seen(*state, request.receiptId))
                return ReserveResult::Replay;
        } else {
            state = FindFree();
            if (!state)
                return ReserveResult::Full;
            *state = {};
            state->active = true;
            state->bot = request.bot;
        }

        state->phase = Phase::Reserved;
        state->receiptId = request.receiptId;
        state->weaponId = request.weaponId;
        state->extra = request.extra;
        return ReserveResult::Ready;
    }

    Charge ResolveNative(const Request& request, int nativeResult) {
        State* state = Find(request.bot);
        if (!Matches(state, request, Phase::Reserved))
            return {};
        if (nativeResult != 0) {
            Remember(*state, request.receiptId);
            ClearPending(*state);
            return {};
        }
        state->phase = Phase::Accepted;
        Remember(*state, request.receiptId);
        return {true, kAcceptedCost, request.bot, request.receiptId};
    }

    Charge Complete(const BotKey& bot, std::uint64_t receiptId) {
        return Settle(bot, receiptId, kCompletionCost);
    }

    // The exact native interruption policy is supplied by the integration.
    // Pass zero when interruption only retains the already paid entry cost.
    Charge Interrupt(const BotKey& bot, std::uint64_t receiptId,
                     std::int32_t nativeInterruptDelta) {
        return Settle(bot, receiptId, nativeInterruptDelta);
    }

    bool Reset(const BotKey& bot) {
        State* state = Find(bot);
        if (!state)
            return false;
        *state = {};
        return true;
    }

    bool Inspect(const BotKey& bot, StateView* view) const {
        if (!view)
            return false;
        const State* state = Find(bot);
        if (!state)
            return false;
        view->active = state->active;
        view->phase = state->phase;
        view->bot = state->bot;
        view->receiptId = state->receiptId;
        view->weaponId = state->weaponId;
        view->extra = state->extra;
        return true;
    }

private:
    struct State {
        bool active = false;
        Phase phase = Phase::Idle;
        BotKey bot{};
        std::uint64_t receiptId = 0;
        std::uint32_t weaponId = 0;
        std::uint32_t extra = 0;
        std::array<std::uint64_t, History> history{};
        std::size_t historyNext = 0;
    };

    static bool Valid(const Request& request) {
        return request.bot.room != 0 && request.bot.generation != 0 &&
               request.bot.pilotId != 0 && request.receiptId != 0;
    }

    static bool Matches(const State* state, const Request& request,
                        Phase phase) {
        return state && state->phase == phase &&
               state->receiptId == request.receiptId &&
               state->weaponId == request.weaponId &&
               state->extra == request.extra;
    }

    static bool Seen(const State& state, std::uint64_t receiptId) {
        for (std::uint64_t seen : state.history)
            if (seen == receiptId)
                return true;
        return false;
    }

    static void Remember(State& state, std::uint64_t receiptId) {
        if constexpr (History != 0) {
            if (Seen(state, receiptId))
                return;
            state.history[state.historyNext] = receiptId;
            state.historyNext = (state.historyNext + 1) % History;
        }
    }

    static void ClearPending(State& state) {
        state.phase = Phase::Idle;
        state.receiptId = 0;
        state.weaponId = 0;
        state.extra = 0;
    }

    Charge Settle(const BotKey& bot, std::uint64_t receiptId,
                  std::int32_t delta) {
        State* state = Find(bot);
        if (!state || state->phase != Phase::Accepted ||
            state->receiptId != receiptId) {
            return {};
        }
        Charge charge{delta != 0, delta, bot, receiptId};
        ClearPending(*state);
        return charge;
    }

    State* Find(const BotKey& bot) {
        for (State& state : states_)
            if (state.active && SameBot(state.bot, bot))
                return &state;
        return nullptr;
    }

    const State* Find(const BotKey& bot) const {
        for (const State& state : states_)
            if (state.active && SameBot(state.bot, bot))
                return &state;
        return nullptr;
    }

    State* FindFree() {
        for (State& state : states_)
            if (!state.active)
                return &state;
        return nullptr;
    }

    std::array<State, MaxBots> states_{};
};

}  // namespace pvp_ai::special_budget
