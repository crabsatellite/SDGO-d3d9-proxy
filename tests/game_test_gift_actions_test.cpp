// OFFLINE-ONLY ABI and boundary tests for GiftActions.
//
// The fixture models the shipped x86 profile, mail-packet constructor and
// network object. It never starts GOnline, contacts a server, writes a
// database, or claims that a gift command was processed.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../mods/game_test_client/gift_actions.h"

namespace
{
int g_failures = 0;
uint8_t g_app[0x500] = {};
uint8_t g_profileStorage[0x6000] = {};
uint8_t* g_profile = g_profileStorage;
uint8_t g_network[0x60] = {};
uint8_t g_stream = 0;
uint8_t g_lastPacket[0x159] = {};
unsigned g_packetCtorCalls = 0;
unsigned g_networkCalls = 0;
bool g_corruptPacket = false;
int g_networkResult = 1;

void CopyBounded(char* destination, const char* source, size_t count)
{
    if (!destination || !source)
        return;
    size_t length = 0;
    while (length < count && source[length] != '\0')
        ++length;
    std::memcpy(destination, source, length);
}

void Check(bool condition, const char* message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

void ResetFixture()
{
    std::memset(g_app, 0, sizeof(g_app));
    std::memset(g_profileStorage, 0, sizeof(g_profileStorage));
    std::memset(g_network, 0, sizeof(g_network));
    std::memset(g_lastPacket, 0, sizeof(g_lastPacket));
    *reinterpret_cast<uint32_t*>(g_app + 0x4A0) = 6;
    *reinterpret_cast<uint32_t*>(g_profileStorage + 0x138) = 6228;
    std::memcpy(g_profileStorage + 0x161, "GiftGM", 7);
    *reinterpret_cast<void**>(g_network + 0x14) = &g_stream;
    g_profile = g_profileStorage;
    g_packetCtorCalls = 0;
    g_networkCalls = 0;
    g_corruptPacket = false;
    g_networkResult = 1;
}

uint8_t* __cdecl FakeGameApp()
{
    return g_app;
}

void* __fastcall FakeLoginPacketCtor(
    void* self,
    void*,
    const char*,
    const char*)
{
    return self;
}

char* __cdecl FakePasswordMd5Hex(const void*, char* output, int)
{
    if (output)
        std::memset(output, '0', 32);
    return output;
}

uint8_t* __cdecl FakeNetwork()
{
    return g_network;
}

int __fastcall FakeNetworkSend(
    void* network,
    void*,
    void* stream,
    const void* packet,
    int size,
    char flag)
{
    ++g_networkCalls;
    Check(
        network == g_network &&
            stream == &g_stream &&
            flag == 0,
        "GiftActions used the original network object, stream, and flag");
    Check(
        packet != nullptr &&
            size == static_cast<int>(sizeof(g_lastPacket)),
        "GiftActions submitted exactly one native 0x159-byte request");
    if (packet && size == static_cast<int>(sizeof(g_lastPacket)))
        std::memcpy(g_lastPacket, packet, sizeof(g_lastPacket));
    return g_networkResult;
}

int __fastcall FakeLoginDispatch(
    void*,
    void*,
    int,
    int,
    int,
    int,
    void*)
{
    return 0;
}

void* __fastcall FakeNativeWStringCtor(
    void* self,
    void*,
    const wchar_t*)
{
    return self;
}

void* __fastcall FakeNicknameDialogCtor(void* self, void*)
{
    return self;
}

void* __cdecl FakeEnqueueUiObject(void* object, int)
{
    return object;
}

void __fastcall FakeNicknameDialogDtor(void*, void*)
{
}

void __fastcall FakeNicknameDialogConfirm(void*, void*)
{
}

void* __fastcall FakeMailPacketCtor(
    void* self,
    void*,
    uint32_t kind,
    const char* sender,
    uint32_t recipientKind,
    const char* recipient,
    const char* body)
{
    ++g_packetCtorCalls;
    auto* packet = static_cast<uint8_t*>(self);
    std::memset(packet, 0, 0x159);
    *reinterpret_cast<uint16_t*>(packet) =
        g_corruptPacket ? 0xFFFF : 0x0891;
    *reinterpret_cast<uint32_t*>(packet + 0x02) = kind;
    *reinterpret_cast<uint32_t*>(packet + 0x17) = recipientKind;
    if (sender)
        CopyBounded(
            reinterpret_cast<char*>(packet + 0x06),
            sender,
            16);
    if (recipient)
        CopyBounded(
            reinterpret_cast<char*>(packet + 0x1B),
            recipient,
            16);
    if (body)
        CopyBounded(
            reinterpret_cast<char*>(packet + 0x2C),
            body,
            300);
    return self;
}

void* LookupSymbol(const char* name)
{
    if (std::strcmp(name, "game.app") == 0)
        return reinterpret_cast<void*>(&FakeGameApp);
    if (std::strcmp(name, "flow.login_packet_ctor") == 0)
        return reinterpret_cast<void*>(&FakeLoginPacketCtor);
    if (std::strcmp(name, "flow.password_md5_hex") == 0)
        return reinterpret_cast<void*>(&FakePasswordMd5Hex);
    if (std::strcmp(name, "flow.network") == 0)
        return reinterpret_cast<void*>(&FakeNetwork);
    if (std::strcmp(name, "flow.network_send") == 0)
        return reinterpret_cast<void*>(&FakeNetworkSend);
    if (std::strcmp(name, "flow.login_service_dispatch") == 0)
        return reinterpret_cast<void*>(&FakeLoginDispatch);
    if (std::strcmp(name, "flow.profile_pointer") == 0)
        return &g_profile;
    if (std::strcmp(name, "flow.native_wstring_ctor") == 0)
        return reinterpret_cast<void*>(&FakeNativeWStringCtor);
    if (std::strcmp(name, "flow.nickname_dialog_ctor") == 0)
        return reinterpret_cast<void*>(&FakeNicknameDialogCtor);
    if (std::strcmp(name, "flow.enqueue_ui_object") == 0)
        return reinterpret_cast<void*>(&FakeEnqueueUiObject);
    if (std::strcmp(name, "flow.nickname_dialog_dtor") == 0)
        return reinterpret_cast<void*>(&FakeNicknameDialogDtor);
    if (std::strcmp(name, "flow.nickname_dialog_confirm") == 0)
        return reinterpret_cast<void*>(&FakeNicknameDialogConfirm);
    if (std::strcmp(name, "flow.mail_send_packet_ctor") == 0)
        return reinterpret_cast<void*>(&FakeMailPacketCtor);
    return nullptr;
}

void* ApiSymbol(SdgoMod*, const char* name)
{
    return LookupSymbol(name);
}

int ApiHookCode(
    SdgoMod*,
    const char* name,
    void*,
    void** original)
{
    void* target = LookupSymbol(name);
    if (!target || !original)
        return 0;
    *original = target;
    return 1;
}

void TestNativeRequest(GiftActions& gift)
{
    ResetFixture();
    gift.Reset();
    const GiftActions::Result result =
        gift.SendUnitGift("GameTest9229", 3, 2);
    const GiftActions::Observation state = gift.State();

    Check(
        result == GiftActions::kRequestSent,
        "valid gift command reached the original request path");
    Check(
        g_packetCtorCalls == 1 && g_networkCalls == 1,
        "one native constructor call produced one transport call");
    Check(
        *reinterpret_cast<const uint16_t*>(g_lastPacket) == 0x0891 &&
            *reinterpret_cast<const uint32_t*>(g_lastPacket + 0x02) == 0 &&
            *reinterpret_cast<const uint32_t*>(g_lastPacket + 0x17) == 0,
        "request retained the shipped 0x0891 header and ordinary-mail kinds");
    Check(
        std::strcmp(
            reinterpret_cast<const char*>(g_lastPacket + 0x06),
            "GiftGM") == 0 &&
        std::strcmp(
            reinterpret_cast<const char*>(g_lastPacket + 0x1B),
            "GameTest9229") == 0 &&
        std::strcmp(
            reinterpret_cast<const char*>(g_lastPacket + 0x2C),
            "!give 3 2") == 0,
        "sender, recipient, and command occupy the original packet fields");
    Check(
        *reinterpret_cast<const uint32_t*>(g_network + 0x58) == 1 &&
            *reinterpret_cast<const uint32_t*>(g_network + 0x5C) == 0x159,
        "original network counters advanced exactly as NativeActions specifies");
    Check(
        state.result == GiftActions::kRequestSent &&
            state.native_app_state == 6 &&
            state.profile_pilot_id == 6228 &&
            state.page == 3 &&
            state.position == 2 &&
            state.packet_size == 0x159 &&
            state.opcode == 0x0891 &&
            state.request_sent == 1,
        "published evidence describes only the native mail request");
}

void TestInputAndStateBoundaries(GiftActions& gift)
{
    ResetFixture();
    gift.Reset();
    Check(
        gift.SendUnitGift(nullptr, 1, 1) ==
            GiftActions::kInvalidRecipient &&
            g_packetCtorCalls == 0 &&
            g_networkCalls == 0,
        "null recipient fails before packet construction");

    char overlong[18] = "12345678901234567";
    Check(
        gift.SendUnitGift(overlong, 1, 1) ==
            GiftActions::kInvalidRecipient &&
            g_packetCtorCalls == 0,
        "recipient longer than the native 16-byte field is rejected");

    Check(
        gift.SendUnitGift("target", 0, 1) ==
            GiftActions::kInvalidPage &&
            gift.SendUnitGift("target", 1, 0) ==
                GiftActions::kInvalidPosition &&
            gift.SendUnitGift("target", 1, 7) ==
                GiftActions::kInvalidPosition &&
            g_networkCalls == 0,
        "page and six-slot position bounds fail without transport");

    *reinterpret_cast<uint32_t*>(g_app + 0x4A0) = 7;
    Check(
        gift.SendUnitGift("target", 1, 1) ==
            GiftActions::kWaitingForLobby &&
            g_packetCtorCalls == 0,
        "mail command waits for the verified lobby application state");

    *reinterpret_cast<uint32_t*>(g_app + 0x4A0) = 6;
    g_profile = nullptr;
    Check(
        gift.SendUnitGift("target", 1, 1) ==
            GiftActions::kProfileUnavailable &&
            g_packetCtorCalls == 0,
        "missing authenticated profile fails closed");

    g_profile = g_profileStorage;
    std::memset(g_profileStorage + 0x161, 0, 16);
    Check(
        gift.SendUnitGift("target", 1, 1) ==
            GiftActions::kSenderNicknameInvalid &&
            g_packetCtorCalls == 0,
        "empty native sender nickname fails before construction");
}

void TestConstructionAndTransportFailures(GiftActions& gift)
{
    ResetFixture();
    gift.Reset();
    g_corruptPacket = true;
    Check(
        gift.SendUnitGift("target", 2, 6) ==
            GiftActions::kPacketConstructionFailed &&
            g_packetCtorCalls == 1 &&
            g_networkCalls == 0,
        "mismatched constructor output never reaches transport");

    ResetFixture();
    gift.Reset();
    g_networkResult = 0;
    Check(
        gift.SendUnitGift("target", 2, 6) ==
            GiftActions::kTransportRejected &&
            g_packetCtorCalls == 1 &&
            g_networkCalls == 1 &&
            gift.State().request_sent == 0,
        "native network rejection is not published as a sent request");
}
} // namespace

int main()
{
    std::puts(
        "OFFLINE ONLY: GiftActions native-mail ABI/boundary regression; no game, server, or database");

    SdgoApi api = {};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Symbol = &ApiSymbol;
    api.HookCode = &ApiHookCode;

    NativeActions::Config config = {};
    config.username = "offline";
    config.password = "offline";
    config.room_title = "offline-gift";
    config.room_mode = 6;
    config.map_id = 0;

    NativeActions native;
    const bool nativeBound = native.Bind(
        &api,
        reinterpret_cast<SdgoMod*>(1),
        config);
    Check(
        nativeBound,
        "NativeActions bound to the offline native-network fixture");

    GiftActions gift;
    const bool giftBound = gift.Bind(
        &api,
        reinterpret_cast<SdgoMod*>(1),
        &native);
    Check(
        giftBound,
        "GiftActions required the profile and original mail constructor symbols");

    if (nativeBound && giftBound) {
        TestNativeRequest(gift);
        TestInputAndStateBoundaries(gift);
        TestConstructionAndTransportFailures(gift);
        ResetFixture();gift.Reset();
        Check(gift.SendMail("System","!givegb 9229 100000")==GiftActions::kRequestSent &&
            g_networkCalls==1 && strcmp(reinterpret_cast<const char*>(g_lastPacket+0x2C),"!givegb 9229 100000")==0 &&
            strcmp(reinterpret_cast<const char*>(g_lastPacket+0x06),"GiftGM")==0,
            "authored normal preparation mail retains the native authenticated sender and exact body");
        ResetFixture();
        Check(gift.SendMail("System","")==GiftActions::kInvalidBody && !g_packetCtorCalls,
            "empty mail body cannot reach the original constructor");
        char tooLong[302];memset(tooLong,'a',301);tooLong[301]=0;
        Check(gift.SendMail("System",tooLong)==GiftActions::kInvalidBody && !g_packetCtorCalls,
            "oversized mail cannot reach the original constructor");
    }

    if (g_failures) {
        std::fprintf(
            stderr,
            "OFFLINE GIFT ABI FAILURES: %d\n",
            g_failures);
        return 1;
    }
    std::puts("OFFLINE GIFT ABI PASS");
    return 0;
}
