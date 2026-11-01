#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../mods/pvp_ai/special_budget.h"

using namespace pvp_ai::special_budget;

namespace {

int failures = 0;

void Check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition)
        ++failures;
}

BotKey Key(std::uint32_t generation = 1,
           std::uint32_t pilotId = 0x80001001u) {
    return {0x12345000u, generation, pilotId};
}

Request Req(std::uint64_t receiptId, std::uint32_t generation = 1) {
    return {Key(generation), receiptId, 43005, 77};
}

}  // namespace

int main() {
    Check(sizeof(ServerRequestPacket) == 0x12 &&
              offsetof(ServerRequestPacket, attackerHandle) == 0x06 &&
              offsetof(ServerRequestPacket, extra) == 0x0E,
          "server request preserves the 0x004C3820 packet ABI");
    Check(sizeof(NotificationPacket) == 0x16 &&
              offsetof(NotificationPacket, absoluteSp) == 0x12,
          "7BF notification carries the authoritative server SP at +0x12");
    const AbsoluteSpMessage message = MakeAbsoluteSpMessage(40000);
    Check(sizeof(message) == 0x14 && message.command == 0x18 &&
              message.reserved04 == 0 && message.reserved08 == 0 &&
              message.reserved0C == 0 && message.absoluteSp == 30000,
          "command18 is a zero-initialized native absolute-SP update");

    Controller<2> controller;
    const Request first = Req(1001);
    Check(controller.Reserve(first, 29999) ==
              ReserveResult::InsufficientSp,
          "NPC cannot reserve a special below the player 30000-SP gate");
    Request human = first;
    human.bot.pilotId = 1001;
    Check(controller.Reserve(human, 30000) == ReserveResult::NotNpc,
          "server supplement never changes the native human path");
    Check(controller.Reserve(first, 30000) == ReserveResult::Ready,
          "full-SP NPC request is reserved before native validation");
    Check(controller.Reserve(first, 30000) == ReserveResult::Replay,
          "same in-flight intent cannot enter native validation twice");

    const Charge rejected = controller.ResolveNative(first, 0x13B2);
    Check(!rejected.apply,
          "native rejection produces no supplemental SP charge");
    Check(controller.Reserve(first, 30000) == ReserveResult::Replay,
          "a resolved rejected receipt cannot be replayed");

    const Request accepted = Req(1002);
    Check(controller.Reserve(accepted, 30000) == ReserveResult::Ready,
          "new intent may retry after a native rejection");
    const Charge entry = controller.ResolveNative(accepted, 0);
    Check(entry.apply && entry.delta == -10000 &&
              SameBot(entry.bot, accepted.bot),
          "only native success produces the one-time entry charge");
    Check(!controller.ResolveNative(accepted, 0).apply,
          "duplicate native success callback cannot charge entry twice");

    StateView view{};
    Check(controller.Inspect(accepted.bot, &view) &&
              view.phase == Phase::Accepted &&
              view.receiptId == accepted.receiptId,
          "accepted intent remains pending until one terminal event");
    const Charge completion =
        controller.Complete(accepted.bot, accepted.receiptId);
    Check(completion.apply && completion.delta == -20000,
          "one authenticated completion pays the native player completion cost");
    Check(!controller.Complete(accepted.bot, accepted.receiptId).apply,
          "duplicate completion cannot charge twice");

    const Request interrupted = Req(1003);
    Check(controller.Reserve(interrupted, 30000) == ReserveResult::Ready &&
              controller.ResolveNative(interrupted, 0).apply,
          "second special reaches accepted state");
    const Charge interrupt =
        controller.Interrupt(interrupted.bot, interrupted.receiptId, 0);
    Check(!interrupt.apply,
          "zero-delta interrupt retains only the already paid entry cost");
    Check(!controller.Complete(interrupted.bot, interrupted.receiptId).apply,
          "an interrupted intent cannot later replay completion");

    const Request stale = Req(1004, 2);
    Check(controller.Reserve(stale, 30000) == ReserveResult::Ready,
          "new generation has an independent resource state");
    Check(!controller.ResolveNative(Req(1004, 1), 0).apply,
          "stale generation cannot resolve a newer reservation");
    Check(controller.Reset(stale.bot),
          "exact match cleanup removes the pending generation");

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
