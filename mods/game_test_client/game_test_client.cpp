#include "quiet_sockets.h"
#include "quiet_audio.h"
#include "quiet_graphics.h"
#include "../game_test_shared/runner.h"
#include "native_actions.h"
#include "channel_actions.h"
#include "battleship_actions.h"
#include "battleship_deck_actions.h"
#include "hangar_navigation.h"
#include "gift_actions.h"
#include "room_actions.h"
#include "tutorial_actions.h"
#include "onboarding_actions.h"
#include "onboarding_gate.h"
#include "sdgo/pvp_ai_client_observer.h"
#include <cwchar>

namespace {
static_assert(static_cast<uint32_t>(TutorialActions::kLeftGame) == sdgo_game_test::kTutorialLeftGame, "tutorial observation contract changed");
static_assert(static_cast<uint32_t>(OnboardingActions::kStarterAccepted) == sdgo_game_test::kStarterAccepted, "starter observation contract changed");
static_assert(static_cast<uint32_t>(OnboardingActions::kInitialLoadingAccepted) == sdgo_game_test::kInitialLoadingAccepted, "initial-loading observation contract changed");
using AppMethod = void(__thiscall*)(uint8_t*);
using FindWindowFn = HWND(WINAPI*)(LPCWSTR, LPCWSTR);
using MessageBoxWFn=int(WINAPI*)(HWND,LPCWSTR,LPCWSTR,UINT);
using MessageBoxAFn=int(WINAPI*)(HWND,LPCSTR,LPCSTR,UINT);
AppMethod originalUpdate;
FindWindowFn originalFindWindow;
MessageBoxWFn originalMessageBoxW;
MessageBoxAFn originalMessageBoxA;
DWORD mainThread;
NativeActions actions;
ChannelActions channels;
BattleShipActions battleships;
BattleShipDeckActions decks;
HangarNavigation hangarNavigation;
GiftActions gifts;
RoomActions rooms;
TutorialActions tutorial;
OnboardingActions onboarding;
SdgoPvpAiClientGetSnapshotV1Fn getSnapshot;
SdgoPvpAiClientGetLuaStatusV1Fn getLuaStatus;
char accountName[32] = {}, accountPassword[128] = {}, roomTitle[21] = {};
unsigned loginAttempts = 0, retryCount = 1;
uint32_t loginReplySeen = 0;
bool expectFirstFailure = false;
bool passwordIsMd5 = false;
bool importedExistingAccount = false;
bool bootstrapOnly = false;
bool verifyDirectNetwork = false;
bool verifyQuietRuntime = false;
bool quietLoopback = false;
QuietSockets quietSockets;
QuietAudio quietAudio;
QuietGraphics quietGraphics;
void PublishQuietSocketReceipt();
bool battleshipOnly = false, giftRequestsOnly = false, deckOnly = false;
uint64_t deckUnitUids[4]{};
char giftRecipient[17]{};
uint32_t giftSenderPilot = 0, giftRecipientPilot = 0, giftCount = 0, giftSent = 0;
uint32_t giftPages[4]{}, giftPositions[4]{};
uint64_t rejectedAt = 0;
bool nicknameRequested = false, nicknameAccepted = false;
uint32_t nicknameReplySeen = 0;
bool roomRequested = false, startRequested = false, leaveRequested = false;
uint32_t startSequence = 0;
uint32_t activeBattleStartSequence = 0;
uint32_t endSequence = 0, leaveSequence = 0;
uint32_t authenticatedPilot = 0;
uint32_t roomCreationSequence = 0;
uint32_t roomReplySequence = 0;
bool endObserved = false;
std::vector<uint16_t> requestedModes;
size_t modeIndex = 0;
unsigned expectedAi = 3;
uint16_t RequestedMode() { return requestedModes.at(modeIndex); }
struct MotionSample {
    uint32_t pilot, object, npc;
    float x, z;
    bool moved;
};
std::vector<MotionSample> motionSamples;

bool LoadPlan() {
    const std::wstring job = sdgo_test::directory + L"\\run.ini";
    wchar_t networkPolicy[24]{};
    GetPrivateProfileStringW(L"run",L"network_policy",L"legacy",networkPolicy,24,job.c_str());
    verifyDirectNetwork = !wcscmp(networkPolicy,L"direct");
    verifyQuietRuntime=GetPrivateProfileIntW(L"run",L"quiet",0,job.c_str())!=0;
    quietLoopback=verifyQuietRuntime && GetPrivateProfileIntW(L"run",L"quiet_loopback",0,job.c_str())==1;
    bootstrapOnly = GetPrivateProfileIntW(L"run", L"bootstrap_only", 0, job.c_str()) != 0;
    battleshipOnly = GetPrivateProfileIntW(L"run", L"battleship_only", 0, job.c_str()) != 0;
    giftRequestsOnly = GetPrivateProfileIntW(L"run", L"gift_requests_only", 0, job.c_str()) != 0;
    deckOnly = GetPrivateProfileIntW(L"run", L"deck_only", 0, job.c_str()) != 0;
    if (static_cast<unsigned>(bootstrapOnly) + battleshipOnly + giftRequestsOnly + deckOnly > 1) return false;
    wchar_t relative[512] = {}, nonce[16] = {}, user[32] = {}, password[128] = {};
    GetPrivateProfileStringW(L"run", L"account_path", L"private/account.ini", relative, 512, job.c_str());
    std::wstring accountPath = relative;
    if (accountPath.find(L':') == std::wstring::npos && accountPath[0] != L'\\')
        accountPath = sdgo_test::directory + L"\\" + accountPath;
    GetPrivateProfileStringW(L"account", L"username", L"", user, 32, accountPath.c_str());
    GetPrivateProfileStringW(L"account", L"password", L"", password, 128, accountPath.c_str());
    wchar_t passwordFormat[16] = {};
    GetPrivateProfileStringW(L"account", L"password_format", L"plain", passwordFormat, 16, accountPath.c_str());
    wchar_t registrationState[32] = {};
    GetPrivateProfileStringW(L"registration", L"state", L"", registrationState, 32, accountPath.c_str());
    importedExistingAccount = !wcscmp(registrationState, L"existing_account");
    passwordIsMd5 = !wcscmp(passwordFormat, L"md5");
    if (!passwordIsMd5 && wcscmp(passwordFormat, L"plain")) return false;
    if (passwordIsMd5) {
        if (!importedExistingAccount) return false;
    }
    if (!user[0] || !password[0] || wcslen(user) > 14 || wcslen(password) > 64) return false;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, user, -1, accountName, sizeof(accountName), nullptr, nullptr) ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, password, -1, accountPassword, sizeof(accountPassword), nullptr, nullptr)) return false;
    SecureZeroMemory(password, sizeof(password));
    if (deckOnly) {
        for (unsigned i=0; i<4; ++i) {
            wchar_t key[32]{}, value[32]{};
            swprintf_s(key,L"unit_uid%u",i);
            GetPrivateProfileStringW(L"deck",key,L"",value,32,accountPath.c_str());
            if (!value[0] || wcsspn(value,L"0123456789") != wcslen(value)) return false;
            wchar_t* end=nullptr;
            deckUnitUids[i]=_wcstoui64(value,&end,10);
            if (!deckUnitUids[i] || !end || *end) return false;
            for (unsigned j=0; j<i; ++j) if (deckUnitUids[j]==deckUnitUids[i]) return false;
        }
    }
    if (giftRequestsOnly) {
        if (!importedExistingAccount) return false;
        wchar_t recipient[32]{};
        GetPrivateProfileStringW(L"gift", L"recipient_nickname", L"", recipient, 32, accountPath.c_str());
        if (!recipient[0] || !WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, recipient, -1,
                giftRecipient, sizeof(giftRecipient), nullptr, nullptr)) return false;
        giftSenderPilot = GetPrivateProfileIntW(L"gift", L"sender_pilot_id", 0, accountPath.c_str());
        giftRecipientPilot = GetPrivateProfileIntW(L"gift", L"recipient_pilot_id", 0, accountPath.c_str());
        giftCount = GetPrivateProfileIntW(L"gift", L"count", 0, accountPath.c_str());
        if (!giftSenderPilot || !giftRecipientPilot || giftCount < 1 || giftCount > 4) return false;
        for (uint32_t i = 0; i < giftCount; ++i) {
            wchar_t key[32]{};
            swprintf_s(key, L"page%u", i);
            giftPages[i] = GetPrivateProfileIntW(L"gift", key, 0, accountPath.c_str());
            swprintf_s(key, L"position%u", i);
            giftPositions[i] = GetPrivateProfileIntW(L"gift", key, 0, accountPath.c_str());
            if (!giftPages[i] || giftPositions[i] < 1 || giftPositions[i] > 6) return false;
        }
    }
    GetPrivateProfileStringW(L"run", L"title_nonce", L"gt", nonce, 16, job.c_str());
    char narrowNonce[16] = {};
    if (!WideCharToMultiByte(CP_UTF8, 0, nonce, -1, narrowNonce, sizeof(narrowNonce), nullptr, nullptr)) return false;
    if (strlen(narrowNonce) > 8) return false;
    expectedAi = GetPrivateProfileIntW(L"run", L"expected_ai", 3, job.c_str());
    if (expectedAi < 1 || expectedAi > 7) return false;
    sprintf_s(roomTitle, "%s !ai e%u a%u", narrowNonce, (expectedAi + 1) / 2, expectedAi / 2);
    retryCount = static_cast<unsigned>(GetPrivateProfileIntW(L"run", L"retry_count", 1, job.c_str()));
    if (retryCount < 1 || retryCount > 2) return false;
    expectFirstFailure = GetPrivateProfileIntW(L"run", L"expect_first_fail", 0, job.c_str()) != 0;
    if (importedExistingAccount && (retryCount != 1 || expectFirstFailure)) return false;
    if (expectFirstFailure && retryCount != 2) return false;
    wchar_t modes[128] = {};
    GetPrivateProfileStringW(L"run", L"mode_sequence", L"1", modes, 128, job.c_str());
    for (const wchar_t* p = modes; *p;) {
        if (*p < L'0' || *p > L'9') return false;
        const uint16_t mode = static_cast<uint16_t>(*p++ - L'0');
        if (mode != 1 && mode != 3 && mode != 6 && mode != 7 && mode != 9) return false;
        if (std::find(requestedModes.begin(), requestedModes.end(), mode) != requestedModes.end()) return false;
        requestedModes.push_back(mode);
        if (!*p) break;
        if (*p++ != L',' || !*p) return false;
    }
    if (requestedModes.empty()) return false;
    return true;
}

int __cdecl Bootstrap(const SdgoGameTestContextV1* context) {
    const uint32_t state = actions.AppState();
    if (state == UINT32_MAX || state < 4) return SDGO_GAME_TEST_WAIT;
    if (verifyQuietRuntime) {
        const auto file=sdgo_test::directory+L"\\quiet-verified.ini";
        wchar_t id[64]{};
        GetPrivateProfileStringW(L"run",L"id",L"",id,64,file.c_str());
        const std::wstring expected(sdgo_test::runId.begin(),sdgo_test::runId.end());
        if (expected!=id || GetPrivateProfileIntW(L"quiet",L"pid",0,file.c_str())!=GetCurrentProcessId() ||
            GetPrivateProfileIntW(L"quiet",L"verified",0,file.c_str())!=1 ||
            GetPrivateProfileIntW(L"quiet",L"audio_muted",0,file.c_str())!=1 ||
            GetPrivateProfileIntW(L"quiet",L"firewall_verified",0,file.c_str())!=1) return SDGO_GAME_TEST_WAIT;
        context->Assert(1,"the owned client has an isolated background desktop, muted audio and a verified listener policy before login");
    }
    if (verifyDirectNetwork) {
        const auto file=sdgo_test::directory+L"\\network-verified.ini";
        wchar_t id[64]{};
        GetPrivateProfileStringW(L"run",L"id",L"",id,64,file.c_str());
        const std::wstring expected(sdgo_test::runId.begin(),sdgo_test::runId.end());
        wchar_t expectedEndpoint[160]{}, actualIP[32]{};
        const auto planFile=sdgo_test::directory+L"\\run.ini";
        GetPrivateProfileStringW(L"run",L"endpoint",L"",expectedEndpoint,160,planFile.c_str());
        GetPrivateProfileStringW(L"network",L"ipv4",L"",actualIP,32,file.c_str());
        const auto actualPort=GetPrivateProfileIntW(L"network",L"port",0,file.c_str());
        const std::wstring actualEndpoint=std::wstring(actualIP)+L":"+std::to_wstring(actualPort);
        if (expected!=id || GetPrivateProfileIntW(L"network",L"pid",0,file.c_str())!=GetCurrentProcessId() ||
            GetPrivateProfileIntW(L"network",L"verified",0,file.c_str())!=1 || actualEndpoint!=expectedEndpoint)
            return SDGO_GAME_TEST_WAIT;
        context->Assert(1,"the owned process established its actual TCP login peer at the explicit test endpoint before authentication");
    }
    context->Assert(GetCurrentThreadId() == mainThread, "test polling runs on the observed original app-update thread");
    context->Observe("the original client update reached the login stage without supplied game data or an application-state write");
    return SDGO_GAME_TEST_PASS;
}
int __cdecl Login(const SdgoGameTestContextV1* context) {
    const auto auth = actions.LoginState();
    if (auth.reply_sequence != loginReplySeen) {
        loginReplySeen = auth.reply_sequence;
        char message[128];
        sprintf_s(message, "real login reply opcode=%04X status=%u attempt=%u service_state=%u pilot=%u", auth.reply_opcode, auth.reply_status, loginAttempts, auth.native_login_state, auth.profile_pilot_id);
        context->Observe(message);
        if (auth.result == NativeActions::kAuthAccepted) {
            if (expectFirstFailure && loginAttempts < 2) {
                context->Assert(0, "backend accepted the first attempt although this plan requires a registration retry");
                return SDGO_GAME_TEST_FAIL;
            }
            context->Assert(1, "the original login service accepted a real reply from the backend");
            return SDGO_GAME_TEST_PASS;
        }
        if (auth.result == NativeActions::kAuthRejected) {
            if (loginAttempts >= retryCount) {
                context->Assert(0, "real backend rejected every configured login attempt");
                return SDGO_GAME_TEST_FAIL;
            }
            rejectedAt = GetTickCount64();
            context->Observe("real first-attempt rejection observed; registration retry remains in the normal client request path");
        }
    }
    if (!loginAttempts || (rejectedAt && GetTickCount64() - rejectedAt >= 1000)) {
        rejectedAt = 0;
        ++loginAttempts;
        if (!actions.Login()) {
            context->Assert(0, "original client login request could not be sent");
            return SDGO_GAME_TEST_FAIL;
        }
        context->Observe(loginAttempts == 1 ? "sent native login request, attempt 1" : "sent native login request, registration retry");
    }
    return SDGO_GAME_TEST_WAIT;
}
bool Snapshot(SdgoPvpAiClientSnapshotV1& snapshot) {
    snapshot = {}; snapshot.size = sizeof(snapshot); snapshot.version = SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
    return getSnapshot && getSnapshot(&snapshot) != 0;
}
int __cdecl Room(const SdgoGameTestContextV1* context) {
    if (!roomRequested) {
        const auto state = actions.OnboardingState();
        if (!state.profile_pilot_id || actions.AppState() == 4 || actions.AppState() == 25) return SDGO_GAME_TEST_WAIT;
        {
            const uint32_t requiredGroup = RequestedMode() == 6 || RequestedMode() == 7 ? 2 : 1;
            const ChannelActions::Result result = channels.EnsureVsGroup(requiredGroup);
            const auto channel = channels.State();
            static uint32_t lastResult = UINT32_MAX;
            static uint32_t lastReplySequence = UINT32_MAX;
            static uint32_t lastTransportSequence = UINT32_MAX;
            if (channel.result != lastResult ||
                channel.reply_sequence != lastReplySequence ||
                channel.request_transport_sequence != lastTransportSequence) {
                char message[256];
                sprintf_s(
                    message,
                    "native VS channel precondition result=%s selected=%u/%u target=%u/%u page=%u/%u request=%u reply=%04X/%04X/%u consumed=%u",
                    ChannelActions::ResultName(result),
                    channel.selected_group,
                    channel.selected_channel,
                    channel.target_group,
                    channel.target_channel,
                    channel.current_page,
                    channel.target_page,
                    channel.request_transport_sequence,
                    channel.reply_opcode,
                    channel.reply_error,
                    channel.reply_channel,
                    channel.reply_consumed);
                context->Observe(message);
                lastResult = channel.result;
                lastReplySequence = channel.reply_sequence;
                lastTransportSequence =
                    channel.request_transport_sequence;
            }
            if (result == ChannelActions::kWaitingForLobby ||
                result == ChannelActions::kSelectorOpened ||
                result == ChannelActions::kPaging ||
                result == ChannelActions::kNativeRequestPending ||
                result == ChannelActions::kRequestSent)
                return SDGO_GAME_TEST_WAIT;
            if (result != ChannelActions::kAlreadySelected &&
                result != ChannelActions::kAccepted) {
                char failure[128];
                sprintf_s(
                    failure,
                    "native VS channel precondition failed: %s",
                    ChannelActions::ResultName(result));
                context->Assert(0, failure);
                return SDGO_GAME_TEST_FAIL;
            }
            context->Assert(
                channel.selected_group == requiredGroup &&
                    channel.selected_channel != 0 &&
                    channel.selected_channel == channel.target_channel,
                "the original selector reply placed the real profile in the server-owned VS group required by this mode");
        }
        if (!rooms.SetRoomMode(RequestedMode())) { context->Assert(0, "invalid PvP mode in test plan"); return SDGO_GAME_TEST_FAIL; }
        SdgoPvpAiClientSnapshotV1 before{};
        if (!Snapshot(before)) return SDGO_GAME_TEST_WAIT;
        roomReplySequence = before.create_reply_sequence;
        rooms.ResetObservations();
        roomCreationSequence = rooms.FreshRoomState().sequence;
        roomRequested = true;
        char selection[128];
        sprintf_s(selection, "native create plan mode=%u channel=%u group=%u", RequestedMode(), state.channel_id, state.channel_group);
        context->Observe(selection);
        if (!rooms.Create()) { context->Assert(0, "original create-room request could not be sent"); return SDGO_GAME_TEST_FAIL; }
        context->Observe("sent the native PvP create-room request with the test nonce and AI title command");
    }
    const auto room = rooms.CreateState();
    SdgoPvpAiClientSnapshotV1 replies{};
    if (Snapshot(replies) && replies.create_reply_sequence > roomReplySequence) {
        static uint32_t lastReply = UINT32_MAX;
        if (replies.create_reply_sequence != lastReply) {
            char message[128];
            sprintf_s(message, "real create reply opcode=%04X error=%04X room=%u sequence=%u", replies.create_reply_opcode, replies.create_reply_error, replies.create_reply_room_id, replies.create_reply_sequence);
            context->Observe(message); lastReply = replies.create_reply_sequence;
        }
        if (replies.create_reply_opcode == 0x0A0A || (replies.create_reply_opcode == 0x0918 && !replies.create_reply_room_id)) {
            context->Assert(0, "the real backend rejected the native create-room request");
            return SDGO_GAME_TEST_FAIL;
        }
    }
    const auto nativeRoom = rooms.NativeRoomState();
    const auto freshRoom = rooms.FreshRoomState();
    static uint32_t lastFreshSequence = UINT32_MAX, lastFreshModel = UINT32_MAX, lastFreshId = UINT32_MAX, lastFreshMode = UINT32_MAX;
    if (freshRoom.sequence != lastFreshSequence || freshRoom.model_present != lastFreshModel || freshRoom.room_id != lastFreshId || freshRoom.room_mode != lastFreshMode) {
        char message[144];
        sprintf_s(message, "fresh factory sequence=%u before=%u model=%u id=%u mode=%u success=%u", freshRoom.sequence, roomCreationSequence, freshRoom.model_present, freshRoom.room_id, freshRoom.room_mode, freshRoom.original_factory_succeeded);
        context->Observe(message);
        lastFreshSequence = freshRoom.sequence; lastFreshModel = freshRoom.model_present; lastFreshId = freshRoom.room_id; lastFreshMode = freshRoom.room_mode;
    }
    static uint32_t lastRoomReply = UINT32_MAX, lastRoomResult = UINT32_MAX, lastRoomApp = UINT32_MAX, lastNativeId = UINT32_MAX;
    static bool lastMenu = false;
    if (room.reply_sequence != lastRoomReply || room.result != lastRoomResult || rooms.HasObservedWaitMenu() != lastMenu || nativeRoom.native_app_state != lastRoomApp || nativeRoom.room_id != lastNativeId) {
        char message[160];
        sprintf_s(message, "observed room seq=%u op=%04X result=%u id=%u app=%u model=%u host=%u players=%u menu=%u", room.reply_sequence, room.reply_opcode, room.result, room.room_id, nativeRoom.native_app_state, nativeRoom.model_present, nativeRoom.local_is_host, nativeRoom.player_count, nativeRoom.wait_menu_observed);
        context->Observe(message); lastRoomReply = room.reply_sequence; lastRoomResult = room.result; lastMenu = rooms.HasObservedWaitMenu();
        lastRoomApp = nativeRoom.native_app_state; lastNativeId = nativeRoom.room_id;
    }
    if (room.result == RoomActions::kCreateRejected) { context->Assert(0, "the real backend rejected room creation"); return SDGO_GAME_TEST_FAIL; }
    if ((actions.AppState() != 18 && actions.AppState() != 20) || !nativeRoom.model_present || !nativeRoom.local_is_host ||
        freshRoom.sequence <= roomCreationSequence || !freshRoom.original_factory_succeeded || !freshRoom.model_present ||
        freshRoom.room_mode != RequestedMode() || nativeRoom.room_mode != RequestedMode() || freshRoom.room_id != nativeRoom.room_id)
        return SDGO_GAME_TEST_WAIT;
    context->Assert(freshRoom.room_id == nativeRoom.room_id && freshRoom.room_mode == RequestedMode(), "a new native room factory completion belongs to this create request and room model");
    context->Assert(nativeRoom.room_id != 0, "the server-created native room model supplied a room id");
    context->Assert(actions.OnboardingState().profile_pilot_id == authenticatedPilot, "the waiting room retains this run's authenticated pilot");
    context->Assert(nativeRoom.model_present && nativeRoom.local_is_host && nativeRoom.player_count >= 1 && nativeRoom.player_count <= 12,
                    "the normal native room model contains the authenticated host");
    context->Assert(nativeRoom.room_mode == RequestedMode(), "the real waiting room has the requested PvP mode");
    context->Assert(1, "the original client entered its native waiting room after the real server reply");
    return SDGO_GAME_TEST_PASS;
}
int __cdecl Start(const SdgoGameTestContextV1* context) {
    SdgoPvpAiClientSnapshotV1 snapshot{};
    if (!startRequested) {
        Snapshot(snapshot); startSequence = snapshot.received_battle_start_count;
        endSequence = rooms.BattleEndState().sequence;
        if (!rooms.StartBattleNativeHost()) return SDGO_GAME_TEST_WAIT;
        startRequested = true; context->Observe("called the original room-host Start business routine");
    }
    if (!Snapshot(snapshot) || snapshot.native_app_state != 22 || snapshot.received_battle_start_count <= startSequence) return SDGO_GAME_TEST_WAIT;
    activeBattleStartSequence = snapshot.received_battle_start_count;
    context->Assert(snapshot.room_mode == RequestedMode(), "the native game mode matches the requested real room mode");
    context->Assert(snapshot.received_battle_start_count > startSequence, "a genuine server battle-start packet preceded native GAME_MAIN");
    return SDGO_GAME_TEST_PASS;
}
uint32_t UnitDataForNpc(const SdgoGameTestContextV1* context, uint32_t npcId) {
    using GridData = void*(__cdecl*)();
    using GridFind = int(__thiscall*)(void*, const uint8_t**, uint32_t, int, char);
    const auto data = reinterpret_cast<GridData>(context->api->Symbol(sdgo_test::mod, "grid.data"));
    const auto find = reinterpret_cast<GridFind>(context->api->Symbol(sdgo_test::mod, "grid.find"));
    const uint8_t* record = nullptr;
    if (!data || !find || !find(data(), &record, npcId, 0x29, 0) || !record) return 0;
    return *reinterpret_cast<const uint32_t*>(record + 0x69);
}
int __cdecl AiSpawn(const SdgoGameTestContextV1* context) {
    SdgoPvpAiClientSnapshotV1 snapshot{};
    if (!context->tick) motionSamples.clear();
    if (!Snapshot(snapshot) || snapshot.native_app_state != 22) return SDGO_GAME_TEST_WAIT;
    bool ready = snapshot.bot_count == expectedAi;
    unsigned allies = 0, enemies = 0, moved = 0;
    for (unsigned i = 0; i < snapshot.bot_count && i < 12; ++i) {
        const auto& bot = snapshot.bots[i];
        ready = ready && bot.valid_registered_object && bot.ai_enabled;
        if (bot.team == snapshot.local_team) ++allies;
        else if (bot.team == 3 - snapshot.local_team) ++enemies;
        if (!bot.valid_registered_object || !bot.ai_enabled || !bot.local_control || bot.remote_copy || !bot.master_npc || !bot.position_valid) continue;
        auto sample = std::find_if(motionSamples.begin(), motionSamples.end(), [&](const auto& value) { return value.pilot == bot.pilot_id; });
        if (sample == motionSamples.end()) motionSamples.push_back({bot.pilot_id, bot.object_id, bot.npc_id, bot.position_x, bot.position_z, false});
        else if (sample->object != bot.object_id || sample->npc != bot.npc_id)
            *sample = {bot.pilot_id, bot.object_id, bot.npc_id, bot.position_x, bot.position_z, false};
        else {
            const float dx = bot.position_x - sample->x, dz = bot.position_z - sample->z;
            if (dx * dx + dz * dz > 1.0f) sample->moved = true;
        }
        sample = std::find_if(motionSamples.begin(), motionSamples.end(), [&](const auto& value) { return value.pilot == bot.pilot_id; });
        if (sample != motionSamples.end() && sample->moved) ++moved;
    }
    if (context->elapsed_ms < 4000 || context->tick < 120 || ((!ready || !moved) && context->elapsed_ms < 30000)) return SDGO_GAME_TEST_WAIT;
    char observation[160];
    sprintf_s(observation, "real AI mode=%u bots=%u allies=%u enemies=%u moved=%u lua=%u observer=%u", RequestedMode(), snapshot.bot_count, allies, enemies, moved, snapshot.lua_error_count, snapshot.lua_error_observer_available);
    context->Observe(observation);
    context->Assert(authenticatedPilot && snapshot.profile_pilot_id == authenticatedPilot, "the battle uses the pilot captured after this run's accepted login");
    const std::wstring expectedTitle(roomTitle, roomTitle + strlen(roomTitle));
    context->Assert(!wcscmp(snapshot.room_title, expectedTitle.c_str()), "the actual battle belongs to this run's server-created room");
    context->Assert(snapshot.human_participant_count == 1, "the real room contains the authenticated test player");
    context->Assert(snapshot.ai_participant_count == expectedAi && snapshot.bot_count == expectedAi, "all server-provided AI participants spawned as native units");
    context->Assert(snapshot.unit_manager_player_count == 1 + expectedAi, "native player and HUD count includes the real player and every AI unit");
    context->Assert(snapshot.local_team >= 1 && snapshot.local_team <= 2 && allies == expectedAi / 2 && enemies == (expectedAi + 1) / 2, "actual AI team placement matches the requested allies and enemies");
    std::set<uint32_t> pilotIds, objectIds;
    for (unsigned i = 0; i < snapshot.bot_count && i < 12; ++i) {
        const auto& bot = snapshot.bots[i];
        context->Assert(bot.valid_registered_object && bot.ai_enabled, "the native AI unit is registered and enabled by the normal game-ready flow");
        context->Assert(bot.team == 1 || bot.team == 2, "the native AI is assigned to a valid battle team");
        context->Assert((bot.pilot_id & 0x80000000u) && pilotIds.insert(bot.pilot_id).second && bot.object_id && objectIds.insert(bot.object_id).second, "each AI has its own server pilot and registered native object");
        context->Assert(bot.npc_id && bot.local_control && !bot.remote_copy && bot.master_npc && bot.position_valid, "the room host owns each AI with a real unit and finite world position");
        context->Assert(bot.native_deck_count_valid && bot.raw_deck_count >= 1 && bot.raw_deck_count <= 4,
                        "the native AI deck vector has a valid, unclamped size");
        if (RequestedMode() == 6 || RequestedMode() == 7) {
            const SdgoPvpAiClientStartDeckSnapshotV1* deck = nullptr;
            for (unsigned j = 0; j < snapshot.received_ai_deck_count && j < 12; ++j)
                if (snapshot.received_ai_decks[j].pilot_id == bot.pilot_id) deck = &snapshot.received_ai_decks[j];
            bool unitInDeck = false;
            if (deck) for (unsigned j = 0; j < deck->unit_count && j < 4; ++j)
                if (deck->npc_ids[j] == bot.npc_id) unitInDeck = true;
            context->Assert(deck && deck->unit_count_valid && bot.native_deck_count_valid && deck->unit_count == bot.raw_deck_count && unitInDeck,
                            "the native deck count agrees with the server record and its current unit belongs to that deck");
            if (deck && deck->unit_count_valid && bot.native_deck_count_valid && deck->unit_count == bot.raw_deck_count) {
                for (unsigned j = 0; j < deck->unit_count; ++j) {
                    const uint32_t expected = UnitDataForNpc(context, deck->npc_ids[j]);
                    context->Assert(expected && bot.native_deck_unit_data_ids[j] == expected,
                                    "each native deck entry matches the unit data resolved from its server-provided NPC");
                }
            }
        }
    }
    if (RequestedMode() == 6 || RequestedMode() == 7) {
        std::set<uint32_t> serverPilots;
        for (unsigned i = 0; i < snapshot.received_ai_deck_count && i < 12; ++i) serverPilots.insert(snapshot.received_ai_decks[i].pilot_id);
        context->Assert(snapshot.received_ai_deck_count == snapshot.bot_count && serverPilots == pilotIds,
                        "the server deck records and native AI objects refer to exactly the same pilots");
    }
    context->Assert(moved != 0, "at least one locally enabled AI moved horizontally across normal game ticks");
    context->Assert(snapshot.lua_error_observer_available, "Lua error observation is active for this real battle");
    context->Assert(snapshot.lua_error_count == 0, "the real battle ran without a Lua AI error");
    return SDGO_GAME_TEST_PASS;
}
int __cdecl Return(const SdgoGameTestContextV1* context) {
    SdgoPvpAiClientLuaStatusV1 lua{sizeof(lua), SDGO_PVP_AI_CLIENT_OBSERVER_VERSION, 0, 0, 0};
    if (!getLuaStatus || !getLuaStatus(&lua) || !lua.observer_available ||
        !activeBattleStartSequence || lua.battle_start_sequence != activeBattleStartSequence) {
        context->Assert(0, "whole-battle Lua observation is unavailable or belongs to another battle");
        return SDGO_GAME_TEST_FAIL;
    }
    if (lua.error_count != 0) {
        char message[112];
        sprintf_s(message, "the real battle raised %u Lua error(s), including after initial AI spawn", lua.error_count);
        context->Assert(0, message);
        return SDGO_GAME_TEST_FAIL;
    }
    const auto ended = rooms.BattleEndState();
    if (!endObserved && ended.sequence > endSequence && ended.original_handler_completed) {
        endObserved = true;
        char message[128];
        sprintf_s(message, "real battle end opcode=%04X result=%u app=%u sequence=%u", ended.reply_opcode, ended.result_type, ended.native_app_state, ended.sequence);
        context->Observe(message);
        context->Assert(ended.reply_opcode == 0x0871, "the original client consumed the genuine server battle-end reply");
        context->Assert(1, "Lua observation stayed available with zero errors through the entire real battle");
    }
    if (!endObserved) return SDGO_GAME_TEST_WAIT;
    const auto state = actions.AppState();
    if (!leaveRequested && (state == 18 || state == 20)) {
        leaveSequence = rooms.LeaveState().reply_sequence;
        if (!rooms.Leave()) return SDGO_GAME_TEST_WAIT;
        leaveRequested = true;
        context->Observe("native results returned to the waiting room; sent its original leave-room request");
    }
    const auto left = rooms.LeaveState();
    if (leaveRequested && left.reply_sequence > leaveSequence && left.result == RoomActions::kLeaveRejected) {
        context->Assert(0, "the real backend rejected the leave-room request");
        return SDGO_GAME_TEST_FAIL;
    }
    if (!leaveRequested || left.reply_sequence <= leaveSequence || left.result != RoomActions::kLeaveAccepted || state != 6)
        return SDGO_GAME_TEST_WAIT;
    context->Assert(1, "the original client consumed the real leave-room reply and returned to its lobby");
    ++modeIndex;
    channels.Reset();
    activeBattleStartSequence = 0;
    roomRequested = startRequested = leaveRequested = endObserved = false;
    return SDGO_GAME_TEST_PASS;
}
int __cdecl Onboarding(const SdgoGameTestContextV1* context) {
    const auto transport = actions.TransportState();
    static uint32_t lastSend = 0;
    if (transport.send_sequence != lastSend) {
        char message[112];
        sprintf_s(message, "observed original transport opcode=%04X bytes=%u result=%u", transport.opcode, transport.packet_size, transport.result);
        context->Observe(message); lastSend = transport.send_sequence;
    }
    const auto state = actions.OnboardingState();
    // Login acceptance precedes the real profile load. Freeze the first
    // nonzero server-loaded identity here, rather than expecting a pilot id
    // in the earlier authentication acknowledgement or comparing live aliases.
    if (!authenticatedPilot && state.profile_pilot_id) {
        authenticatedPilot = state.profile_pilot_id;
        char message[96]; sprintf_s(message, "captured real post-login profile pilot=%u", authenticatedPilot);
        context->Observe(message);
    }
    static uint32_t lastStage = UINT32_MAX, lastLevel = UINT32_MAX, lastPath = UINT32_MAX, lastEvents = UINT32_MAX;
    if (state.native_login_state != lastStage || state.tutorial_level != lastLevel ||
        state.login_path_state != lastPath || state.pending_login_event_count != lastEvents) {
        char message[180];
        sprintf_s(message, "real onboarding state=%u tutorial_level=%u login_path=%u pending=%u channel=%u pilot=%u", state.native_login_state, state.tutorial_level, state.login_path_state, state.pending_login_event_count, state.channel_id, state.profile_pilot_id);
        context->Observe(message);
        lastStage = state.native_login_state; lastLevel = state.tutorial_level; lastPath = state.login_path_state; lastEvents = state.pending_login_event_count;
    }
    const auto nickname = actions.NicknameState();
    if (nickname.reply_sequence != nicknameReplySeen) {
        nicknameReplySeen = nickname.reply_sequence;
        char message[144];
        sprintf_s(message, "real nickname reply opcode=%04X status=%u service_state=%u pilot=%u", nickname.reply_opcode, nickname.reply_status, nickname.native_login_state, nickname.profile_pilot_id);
        context->Observe(message);
        if (nickname.result == NativeActions::kNicknameRejected) {
            context->Assert(0, "real backend rejected the generated test nickname");
            return SDGO_GAME_TEST_FAIL;
        }
        if (nickname.result == NativeActions::kNicknameAccepted) {
            nicknameAccepted = true;
            context->Assert(1, "the real nickname-create reply was accepted by the original client service");
        }
    }
    if (!nicknameRequested && actions.LoginState().native_login_state == 4) {
        if (importedExistingAccount) {
            context->Assert(0, "an imported existing account unexpectedly requires a nickname; its identity was preserved");
            return SDGO_GAME_TEST_FAIL;
        }
        nicknameRequested = true;
        if (!actions.CreateNickname()) {
            context->Assert(0, "original nickname request could not be sent");
            return SDGO_GAME_TEST_FAIL;
        }
        const auto sent = actions.TransportState();
        context->Assert(sent.opcode == 0x232C && sent.packet_size == 0x63 && sent.result != 0,
                        "the original confirmation business routine transmitted a real nickname request");
        char transportMessage[112];
        sprintf_s(transportMessage, "observed original transport opcode=%04X bytes=%u result=%u", sent.opcode, sent.packet_size, sent.result);
        context->Observe(transportMessage); lastSend = sent.send_sequence;
        context->Observe("sent the original nickname-create request for the generated test account");
    }
    const auto trial = tutorial.State();
    if (trial.result == TutorialActions::kNativeFault) {
        context->Assert(0, "the original beginner-exit business method raised a native exception");
        return SDGO_GAME_TEST_FAIL;
    }
    if (trial.update_sequence && !trial.exit_request_sequence && state.native_app_state == 22 && state.tutorial_level < 2) {
        if (tutorial.ExitBeginnerTrial())
            context->Observe("invoked the original beginner-trial exit business method on the real tutorial mode");
    }
    static bool trialLeftReported = false;
    if (!trialLeftReported && trial.result == TutorialActions::kLeftGame) {
        trialLeftReported = true;
        context->Assert(1, "the original beginner exit completed its normal scene transition");
    }
    auto starter = onboarding.StarterState();
    static uint32_t previousStarterStage = UINT32_MAX, previousStarterReply = 0;
    if (starter.onboarding_stage != previousStarterStage) {
        char message[96]; sprintf_s(message, "observed native onboarding stage=%u app_state=%u", starter.onboarding_stage, starter.native_app_state);
        context->Observe(message); previousStarterStage = starter.onboarding_stage;
    }
    if (starter.native_fault) {
        context->Assert(0, "the normal first-unit business callback raised a native exception"); return SDGO_GAME_TEST_FAIL;
    }
    if (state.tutorial_level < 2 && !starter.request_attempted && onboarding.DrawStarterUnit())
        context->Observe("invoked the original first-unit Buy business callback on the live onboarding capsule");
    if (starter.reply_sequence != previousStarterReply) {
        previousStarterReply = starter.reply_sequence;
        char message[144]; sprintf_s(message, "real starter reply opcode=%04X error=%u unit=%u slot=%u tutorial=%u", starter.reply_opcode, starter.reply_error, starter.picked_unit_id, starter.picked_unit_slot, starter.tutorial_level);
        context->Observe(message);
        if (starter.result == OnboardingActions::kStarterRejected) {
            context->Assert(0, "the real backend rejected the first-unit request"); return SDGO_GAME_TEST_FAIL;
        }
        context->Assert(starter.picked_unit_id != 0, "the real first-unit reply supplied a unit id");
    }
    if (onboarding.ConfirmStarterResultPopup())
        context->Observe("invoked the original starter-result confirmation business callback on the real result popup");
    if (onboarding.ConfirmInitialLoadingPopup())
        context->Observe("called the original initial-loading popup confirmation business method");
    const auto initial = onboarding.InitialLoadingState();
    if (initial.result == OnboardingActions::kInitialLoadingRejected) {
        context->Assert(0, "the backend rejected initial-loading completion"); return SDGO_GAME_TEST_FAIL;
    }
    const auto inventory = onboarding.InventoryState();
    static uint32_t previousOwned = UINT32_MAX;
    if (inventory.initialized && inventory.owned_unit_count != previousOwned) {
        char message[128]; sprintf_s(message, "real inventory owned=%u first_unit=%u slot=%u scanned=%u/%u", inventory.owned_unit_count, inventory.first_unit_id, inventory.first_unit_slot, inventory.slots_examined, inventory.slot_capacity);
        context->Observe(message); previousOwned = inventory.owned_unit_count;
    }
    if (inventory.native_fault) {
        context->Assert(0, "the read-only native inventory observation failed"); return SDGO_GAME_TEST_FAIL;
    }
    const sdgo_game_test::OnboardingCompletionEvidence completion{
        nicknameRequested,
        nicknameAccepted,
        trial.exit_request_sequence,
        trial.result,
        starter.request_attempted != 0,
        starter.result,
        starter.result_confirm_sequence,
        initial.confirm_sequence,
        initial.result,
        state.tutorial_level,
        state.starter_draw_busy != 0,
        inventory.initialized != 0,
        inventory.scan_complete != 0,
        inventory.owned_unit_count,
        inventory.first_unit_id,
        actions.AppState(),
        authenticatedPilot,
        state.profile_pilot_id,
    };
    if (sdgo_game_test::IsOnboardingComplete(completion)) {
        context->Assert(1, "the authenticated account has a real initialized inventory containing an owned unit");
        context->Assert(authenticatedPilot && state.profile_pilot_id == authenticatedPilot, "onboarding retains the real profile loaded after the accepted login");
        context->Assert(1, "every attempted one-time native onboarding action completed before the lobby");
        return SDGO_GAME_TEST_PASS;
    }
    // Continue only after the normal tutorial/starter-unit/window workflow has
    // provided real profile and inventory data. Nickname success alone is insufficient.
    return SDGO_GAME_TEST_WAIT;
}
int __cdecl AcquireBattleship(const SdgoGameTestContextV1* context) {
    auto state = battleships.State();
    BattleShipActions::Result result;
    if (state.reply_sequence) {
        if (state.reply_error || !state.reply_consumed) {
            context->Assert(0, "the real initial-battleship reply was rejected or not consumed");
            return SDGO_GAME_TEST_FAIL;
        }
        result = battleships.ConfirmAcquisitionPopup();
    } else {
        result = battleships.OpenTutorial();
        if (result == BattleShipActions::kTutorialOpened)
            result = battleships.DriveTutorial();
    }
    state = battleships.State();
    static uint32_t lastResult = UINT32_MAX, lastReply = UINT32_MAX;
    static uint8_t lastPage = UINT8_MAX;
    if (state.result != lastResult || state.reply_sequence != lastReply || state.page != lastPage) {
        char text[224];
        sprintf_s(text, "native battleship acquisition result=%s tutorial=%u page=%u/%u request=%u reply=%04X/%04X consumed=%u ship=%u uid=%u:%u popup=%u",
            BattleShipActions::ResultName(result), state.tutorial_level, state.page, state.page_count,
            state.request_transport_sequence,state.reply_opcode,state.reply_error,state.reply_consumed,
            state.ship_id,state.ship_uid_high,state.ship_uid_low,state.result_popup_ready);
        context->Observe(text); lastResult=state.result;lastReply=state.reply_sequence;lastPage=state.page;
    }
    if (result == BattleShipActions::kAlreadyCompleted || result == BattleShipActions::kConfirmed) {
        context->Assert(state.profile_pilot_id == authenticatedPilot && authenticatedPilot,
                        "the native battleship belongs to this authenticated test account");
        context->Assert(state.tutorial_level >= 4 && (state.ship_uid_low || state.ship_uid_high),
                        "the original game profile contains a real acquired and selected battleship");
        if (result == BattleShipActions::kConfirmed)
            context->Assert(state.reply_opcode == 0x08EF && state.reply_consumed && state.confirm_sequence,
                            "the real acquisition reply and original result confirmation completed");
        return SDGO_GAME_TEST_PASS;
    }
    if (result == BattleShipActions::kWaitingForLobby || result == BattleShipActions::kWaitingForTutorial ||
        result == BattleShipActions::kTutorialOpened || result == BattleShipActions::kPaging ||
        result == BattleShipActions::kRequestSent || result == BattleShipActions::kNativeRequestPending ||
        result == BattleShipActions::kResultPopupUnavailable || result == BattleShipActions::kAccepted)
        return SDGO_GAME_TEST_WAIT;
    char failure[128];sprintf_s(failure,"native battleship acquisition failed: %s",BattleShipActions::ResultName(result));
    context->Assert(0,failure);return SDGO_GAME_TEST_FAIL;
}

int __cdecl SendGiftRequests(const SdgoGameTestContextV1* context) {
    if (authenticatedPilot != giftSenderPilot) {
        context->Assert(0,"the native authenticated sender differs from the existing gift-helper account");
        return SDGO_GAME_TEST_FAIL;
    }
    const auto receipt = sdgo_test::directory + L"\\gift-processor.ini";
    wchar_t runId[64]{};
    GetPrivateProfileStringW(L"run",L"id",L"",runId,64,receipt.c_str());
    const std::wstring expected(sdgo_test::runId.begin(),sdgo_test::runId.end());
    if (expected != runId) return SDGO_GAME_TEST_WAIT;
    const auto sender=GetPrivateProfileIntW(L"gift",L"sender",0,receipt.c_str());
    const auto recipient=GetPrivateProfileIntW(L"gift",L"recipient",0,receipt.c_str());
    const auto processed=GetPrivateProfileIntW(L"gift",L"processed_count",0,receipt.c_str());
    const auto succeeded=GetPrivateProfileIntW(L"gift",L"success_count",0,receipt.c_str());
    if (sender!=giftSenderPilot || recipient!=giftRecipientPilot || processed>giftSent || succeeded!=processed) {
        context->Assert(0,"the normal mail processor rejected a request or reported another identity");
        return SDGO_GAME_TEST_FAIL;
    }
    if (processed != giftSent) return SDGO_GAME_TEST_WAIT;
    if (giftSent == giftCount) {
        context->Assert(1,"every native mail request was processed by the normal isolated gift service");
        return SDGO_GAME_TEST_PASS;
    }
    if (giftSent) {
        wchar_t tickText[32]{};
        GetPrivateProfileStringW(L"gift",L"last_processed_tick",L"0",tickText,32,receipt.c_str());
        const auto processedTick=_wcstoui64(tickText,nullptr,10);
        if (!processedTick || GetTickCount64() < processedTick || GetTickCount64()-processedTick < 61000)
            return SDGO_GAME_TEST_WAIT;
    }
    const auto result=gifts.SendUnitGift(giftRecipient,giftPages[giftSent],giftPositions[giftSent]);
    if (result==GiftActions::kWaitingForLobby) return SDGO_GAME_TEST_WAIT;
    if (result!=GiftActions::kRequestSent) {
        context->Assert(0,"the original native mail request could not be constructed and transported");
        return SDGO_GAME_TEST_FAIL;
    }
    ++giftSent;
    context->Assert(1,"the original mail constructor and transport sent one real gift command");
    char text[96];sprintf_s(text,"native gift mail sent %u/%u; awaiting independent normal server processing",giftSent,giftCount);
    context->Observe(text);return SDGO_GAME_TEST_WAIT;
}

int __cdecl ConfigureDeck(const SdgoGameTestContextV1* context) {
    if (actions.AppState()!=20) {
        const auto result=hangarNavigation.EnsureHangar();
        const auto navigation=hangarNavigation.State();
        static HangarNavigation::Observation lastNavigation{};
        if (memcmp(&lastNavigation,&navigation,sizeof(navigation))) {
            char message[256];
            sprintf_s(message,"native hangar result=%s app=%u request=%u/%04X/%u radio=%u services=%u/%u/%u roots=%u/%u",
                HangarNavigation::ResultName(result),navigation.native_app_state,navigation.request_transport_sequence,
                navigation.request_opcode,navigation.request_size,navigation.unit_radio_invocations,
                navigation.downmenu_present,navigation.mainframe_present,navigation.unit_manager_present,
                navigation.mainframe_root_ready,navigation.unit_manager_root_ready);
            context->Observe(message);lastNavigation=navigation;
        }
        if (result>=HangarNavigation::kDownmenuServiceMismatch || result==HangarNavigation::kUnbound) {
            context->Assert(0,"original hangar navigation rejected its native service or control identity");
            return SDGO_GAME_TEST_FAIL;
        }
        if (result!=HangarNavigation::kReady)return SDGO_GAME_TEST_WAIT;
    }
    const auto result=decks.EnsureDeck(deckUnitUids);
    const auto state=decks.State();
    static BattleShipDeckActions::Observation previous{};
    if (memcmp(&previous,&state,sizeof(state))) {
        char message[480];
        sprintf_s(message,"native deck result=%s app=%u stage=%u ship=%llu leader=%llu selected=%u cost=%u units=%llu,%llu,%llu,%llu save=%u/%04X goout=%u/%04X editor/manager/profile=%u/%u/%u",
            BattleShipDeckActions::ResultName(result),state.native_app_state,state.stage,state.ship_uid,state.leader_uid,
            state.selected_count,state.total_cost,state.unit_uids[0],state.unit_uids[1],state.unit_uids[2],state.unit_uids[3],
            state.save_reply_sequence,state.save_reply_opcode,state.goout_reply_sequence,state.goout_reply_opcode,
            state.editor_matches,state.manager_matches,state.profile_matches);
        context->Observe(message);previous=state;
    }
    if (result==BattleShipDeckActions::kAccepted) {
        context->Assert(state.editor_matches && state.manager_matches && state.profile_matches,
            "the original save and GoOut consumers agree with the genuine four-unit deck in the editor, manager and profile");
        return SDGO_GAME_TEST_PASS;
    }
    if (result>=BattleShipDeckActions::kInvalidTarget || result==BattleShipDeckActions::kUnbound) {
        context->Assert(0,"native battleship deck configuration rejected the real account or native state");
        return SDGO_GAME_TEST_FAIL;
    }
    return SDGO_GAME_TEST_WAIT;
}
const SdgoGameTestCaseV1 deckCase{"setup.deck",180000,&ConfigureDeck};
const SdgoGameTestCaseV1 battleshipCase{"flow.battleship",180000,&AcquireBattleship};
const SdgoGameTestCaseV1 giftCase{"setup.gift_requests",360000,&SendGiftRequests};
const SdgoGameTestCaseV1 realCases[] = {
    {"framework.bootstrap", 60000, &Bootstrap},
    {"flow.login", 120000, &Login},
    {"flow.onboarding", 180000, &Onboarding},
    {"pvp.room", 120000, &Room},
    {"pvp.start", 120000, &Start},
    {"pvp.ai_spawn", 60000, &AiSpawn},
    {"pvp.return", 600000, &Return},
};
SdgoGameTestProviderV1 provider{sizeof(provider), SDGO_GAME_TEST_VERSION, "pvp-ai", "client", 7, realCases};
std::vector<std::string> caseNames;
std::vector<SdgoGameTestCaseV1> plannedCases;
void RegisterPlannedCases() {
    if (bootstrapOnly) { provider.count = 3; return; }
    const bool needsShip = battleshipOnly || deckOnly || std::find(requestedModes.begin(),requestedModes.end(),6)!=requestedModes.end() ||
        std::find(requestedModes.begin(),requestedModes.end(),7)!=requestedModes.end();
    if (requestedModes.size() == 1 && !needsShip && !giftRequestsOnly) return;
    const size_t count = 4 + requestedModes.size() * 4;
    caseNames.reserve(count); plannedCases.reserve(count);
    for (unsigned i = 0; i < 3; ++i) plannedCases.push_back(realCases[i]);
    if (giftRequestsOnly) plannedCases.push_back(giftCase);
    else if (needsShip) plannedCases.push_back(battleshipCase);
    if (deckOnly) plannedCases.push_back(deckCase);
    const char* suffixes[] = {"room", "start", "ai_spawn", "return"};
    if (!giftRequestsOnly && !battleshipOnly && !deckOnly) for (const auto mode : requestedModes) {
        for (unsigned i = 0; i < 4; ++i) {
            if (requestedModes.size()==1) { plannedCases.push_back(realCases[3+i]);continue; }
            caseNames.push_back("pvp.mode" + std::to_string(mode) + "." + suffixes[i]);
            auto test = realCases[3 + i];
            test.id = caseNames.back().c_str();
            plannedCases.push_back(test);
        }
    }
    provider.count = static_cast<uint32_t>(plannedCases.size());
    provider.cases = plannedCases.data();
}

HWND WINAPI FindOwnInstance(LPCWSTR className, LPCWSTR title) {
    // Allow a separate test client while preserving the other client's window.
    // Login, the real launcher event, initialization and all game requests stay native.
    if (className && !IS_INTRESOURCE(className) && !wcscmp(className, L"_GONLINE_") &&
        title && !IS_INTRESOURCE(title) && !wcscmp(title, L"Gundam Online")) return nullptr;
    return originalFindWindow(className, title);
}
int WINAPI QuietMessageBoxW(HWND owner,LPCWSTR text,LPCWSTR caption,UINT type) {
    if (!verifyQuietRuntime)return originalMessageBoxW(owner,text,caption,type);
    char message[1024]="native Windows dialog during quiet test";
    if (text)WideCharToMultiByte(CP_UTF8,0,text,-1,message,sizeof(message),nullptr,nullptr);
    PublishQuietSocketReceipt();
    sdgo_test::Fail(message);
    return IDCANCEL;
}
int WINAPI QuietMessageBoxA(HWND owner,LPCSTR text,LPCSTR caption,UINT type) {
    if (!verifyQuietRuntime)return originalMessageBoxA(owner,text,caption,type);
    sdgo_test::Fail(text ? text:"native Windows dialog during quiet test");
    return IDCANCEL;
}
void PublishQuietSocketReceipt() {
    if (!verifyQuietRuntime) return;
    const auto observed=quietSockets.State();
    const auto audio=quietAudio.State();
    const auto graphics=quietGraphics.State();
    static QuietSockets::Observation previous{};
    static QuietAudio::Observation previousAudio{};
    static QuietGraphics::Observation previousGraphics{};
    static bool written=false;
    static uint64_t lastWritten=0;
    const auto now=GetTickCount64();
    if (written && now-lastWritten<250 && observed.escaped==previous.escaped && audio.failed==previousAudio.failed &&
        graphics.dispatch_failures==previousGraphics.dispatch_failures && !graphics.proxy_visible && !graphics.proxy_appwindow)return;
    if (written && !memcmp(&previous,&observed,sizeof(observed)) && !memcmp(&previousAudio,&audio,sizeof(audio)) &&
        !memcmp(&previousGraphics,&graphics,sizeof(graphics)))return;
    const auto path=sdgo_test::directory+L"\\quiet-sockets.ini";
    const auto temporary=path+L".tmp";
    FILE* file=_wfopen(temporary.c_str(),L"wb");
    if (!file)return;
    fprintf(file,"[run]\nid=%s\n[policy]\npid=%lu\nactive=%u\nloopback_only=%u\n[sockets]\nbinds=%u\nredirected=%u\nfailed=%u\nescaped=%u\n[audio]\nactive=%u\nopened=%u\nmuted=%u\nfailed=%u\n",
        sdgo_test::runId.c_str(),GetCurrentProcessId(),observed.active,quietLoopback ? 1u:0u,observed.binds,observed.redirected,observed.failed,observed.escaped,
        audio.active,audio.opened,audio.muted,audio.failed);
    fprintf(file,"[graphics]\nactive=%u\nworker_ready=%u\ndispatch_failures=%u\ncreated_devices=%u\nfailed_devices=%u\ncreate_hresult=%ld\npresents=%u\nproxy_hwnd=%lu\nproxy_visible=%u\nproxy_noactivate=%u\nproxy_toolwindow=%u\nproxy_appwindow=%u\n",
        graphics.active,graphics.worker_ready,graphics.dispatch_failures,graphics.created_devices,graphics.failed_devices,
        static_cast<long>(graphics.last_create_hresult),graphics.presents,reinterpret_cast<unsigned long>(graphics.proxy_hwnd),
        graphics.proxy_visible,graphics.proxy_noactivate,graphics.proxy_toolwindow,graphics.proxy_appwindow);
    fclose(file);
    if (MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        previous=observed;previousAudio=audio;previousGraphics=graphics;written=true;lastWritten=now;
    }
}
LONG CALLBACK NativeFault(EXCEPTION_POINTERS* fault) {
    if (!fault || !fault->ExceptionRecord || !fault->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = fault->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_INT_OVERFLOW || code == EXCEPTION_FLT_DIVIDE_BY_ZERO || code == EXCEPTION_ILLEGAL_INSTRUCTION) {
        static LONG reported = 0;
        if (InterlockedIncrement(&reported) <= 8) {
            const auto* c = fault->ContextRecord;
            sdgo_test::api->Log(sdgo_test::mod, SDGO_LOG_ERROR,
                "native exception %08lX at %p: eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX ebp %08lX esp %08lX",
                code, fault->ExceptionRecord->ExceptionAddress, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
            __try {
                const auto* stack=reinterpret_cast<const uint32_t*>(c->Esp);
                for (unsigned i=0;i<16;++i) {
                    HMODULE owner=nullptr;
                    const auto address=reinterpret_cast<const wchar_t*>(stack[i]);
                    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,address,&owner))
                        sdgo_test::api->Log(sdgo_test::mod,SDGO_LOG_ERROR,"native stack[%u]=%08X module=%p offset=%08X",
                            i,stack[i],owner,stack[i]-reinterpret_cast<uint32_t>(owner));
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void __fastcall Update(uint8_t* app, void*) {
    originalUpdate(app);
    PublishQuietSocketReceipt();
    if (!mainThread) mainThread = GetCurrentThreadId();
    if (GetCurrentThreadId() != mainThread) return;
    static uint32_t previousState = UINT32_MAX;
    uint32_t state = *reinterpret_cast<uint32_t*>(app + 0x4A0);
    if (state != previousState) {
        char text[96]; sprintf_s(text, "observed normal client state=%lu", static_cast<unsigned long>(state));
        sdgo_test::Observe(text); previousState = state;
    }
    sdgo_test::Poll();
}
}
SDGO_GAME_TEST_PROVIDER_ENTRY { return &provider; }
SDGO_MOD_ENTRY {
    if (!sdgo_test::Open(api, mod, "client")) return 0;
    const bool loaded = LoadPlan();
    if (verifyQuietRuntime) {
        if (!quietAudio.Install(api,mod) || (quietLoopback && !quietSockets.Install(api,mod)) ||
            !api->HookImport(mod,"user32.dll","MessageBoxW",reinterpret_cast<void*>(&QuietMessageBoxW),reinterpret_cast<void**>(&originalMessageBoxW)) ||
            !api->HookImport(mod,"user32.dll","MessageBoxA",reinterpret_cast<void*>(&QuietMessageBoxA),reinterpret_cast<void**>(&originalMessageBoxA)))return 0;
        PublishQuietSocketReceipt();
    }
    // Map 0 selects the game's normal quick-room random-map policy; the
    // actual map and participants remain provided by the server.
    if (!loaded) sdgo_test::Fail("invalid private account or native scenario configuration");
    RegisterPlannedCases();
    const NativeActions::Config config{
        accountName,
        accountPassword,
        roomTitle,
        static_cast<uint16_t>(loaded ? RequestedMode() : 1),
        0,
        passwordIsMd5};
    if (!loaded || !actions.Bind(api, mod, config) ||
        !channels.Bind(api, mod, &actions) ||
        !battleships.Bind(api,mod,&actions) || !gifts.Bind(api,mod,&actions) ||
        (deckOnly && !decks.Bind(api,mod,&actions)) ||
        (deckOnly && !hangarNavigation.Bind(api,mod,&actions)) ||
        !rooms.Bind(api, mod, &actions, config) ||
        !tutorial.Bind(api, mod, &actions) || !onboarding.Bind(api, mod, &actions))
        sdgo_test::Fail("normal-flow native request bindings or private account configuration are unavailable");
    getSnapshot = reinterpret_cast<SdgoPvpAiClientGetSnapshotV1Fn>(GetProcAddress(GetModuleHandleW(L"match_ai.dll"), SDGO_PVP_AI_CLIENT_SNAPSHOT_EXPORT));
    if (!getSnapshot) sdgo_test::Fail("production PvP AI observation export is unavailable");
    getLuaStatus = reinterpret_cast<SdgoPvpAiClientGetLuaStatusV1Fn>(GetProcAddress(GetModuleHandleW(L"match_ai.dll"), SDGO_PVP_AI_CLIENT_LUA_STATUS_EXPORT));
    if (!getLuaStatus) sdgo_test::Fail("production whole-battle Lua counter export is unavailable");
    if (!AddVectoredExceptionHandler(1, &NativeFault)) return 0;
    if (!api->HookImport(mod, "user32.dll", "FindWindowW", reinterpret_cast<void*>(&FindOwnInstance), reinterpret_cast<void**>(&originalFindWindow)) ||
        !api->HookCode(mod, "test.client_update", reinterpret_cast<void*>(&Update), reinterpret_cast<void**>(&originalUpdate)))
        return 0;
    if (verifyQuietRuntime && !quietGraphics.Install(api,mod))return 0;
    PublishQuietSocketReceipt();
    api->Log(mod, SDGO_LOG_INFO, "observing normal launcher/login/room/battle flow");
    return 1;
}
