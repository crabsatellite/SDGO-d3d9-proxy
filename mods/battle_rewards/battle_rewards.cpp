// Native battle reward integration. Combat telemetry comes from the verified
// server HP-loss helper; normal G delivery remains in the game's own pipeline.
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/mod_settings.h"
#include "sdgo/battle_reward_stats.h"
#include "sdgo/battle_reward_mission.h"
#include "boss_catalog.h"
#include "combat_tracker.h"
#include "daily_schedule.h"
#include "native_cash_notify.h"
#include "native_combat_hook.h"
#include "native_sites.h"
#include "runtime_state.h"
#include "result_presentation.h"
#include "site_bridge.h"
#include "wallet_jobs.h"

namespace {
using namespace sdgo::battle_rewards;
namespace fs = std::filesystem;
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
std::unique_ptr<RuntimeState> g_runtime;
std::mutex g_mutex;
BossCatalog g_bosses;
bool g_bosses_loaded = false, g_trace = false;
std::atomic<bool> g_contract_failure{false};
DailySchedule g_daily;
std::string g_boot, g_error;
PayoutCounters g_counters;
uint64_t g_serial = 0;
std::array<BYTE*, 11> g_sites{};

bool MissionMode(uint8_t mode) { return mode == 5 || mode == 8 || mode == 10; }

bool ReadNativeBytes(const void* base, size_t offset, void* output, size_t size) {
    const BYTE* address = nullptr;
    if (!output || offset > static_cast<size_t>(PTRDIFF_MAX) ||
        !native_stats::AddOffset(base, static_cast<ptrdiff_t>(offset), &address)) return false;
    __try { memcpy(output, address, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct Session {
    uint32_t room_id = 0, started = 0, mission_id = 0;
    uint8_t mode = 0;
    uint64_t last_seen = 0;
    uint64_t completed_unix = 0;
    std::string match_id;
    CombatTracker combat;
    BossCatalog bosses;
    bool boss_catalog_loaded = false, frozen = false, declined = false;
    std::string declined_error;
    Policy policy;
    MatchContext context;
    std::string wallet_scope;
    std::array<uint32_t, 12> accounts{}, settled{};
    unsigned account_count = 0, settled_count = 0;
};
std::map<uintptr_t, Session> g_sessions;

struct Pending {
    WalletJob job;
    native_cash_notify::SessionRoute route;
    uint64_t since = 0;
};
std::vector<Pending> g_pending;

bool HooksActive() {
    for (BYTE* site : g_sites) {
        BYTE instruction = 0;
        if (!native_stats::Read(site, 0, &instruction) || instruction != 0xE9) return false;
    }
    return true;
}

bool NetworkReady() {
    void* net = nullptr;
    void* transport = nullptr;
    return native_cash_notify::detail::SafeRead(native_cash_notify::net_frame_proc_global, 0, &net) && net &&
           native_cash_notify::detail::SafeRead(net, native_cash_notify::kNetFrameTransport, &transport) && transport;
}

Session* CurrentSession(BYTE* room) {
    uint32_t room_id = 0, start = 0, mission = 0;
    uint8_t mode = 0;
    if (!native_stats::Read(room, 0, &room_id) || !native_stats::Read(room, 0x2EEB4, &start) ||
        !native_stats::Read(room, 5, &mode) || !native_stats::Read(room, 0x1B, &mission) ||
        !start || mode < 1 || mode > 14) return nullptr;
    const uintptr_t key = reinterpret_cast<uintptr_t>(room);
    auto found = g_sessions.find(key);
    if (found != g_sessions.end() && found->second.started == start && found->second.room_id == room_id) {
        found->second.last_seen = GetTickCount64();
        return &found->second;
    }
    if (g_sessions.size() >= 1024 && found == g_sessions.end()) {
        const uint64_t now = GetTickCount64();
        for (auto it = g_sessions.begin(); it != g_sessions.end();) {
            if (now - it->second.last_seen > 24 * 60 * 60 * 1000ULL) it = g_sessions.erase(it);
            else ++it;
        }
        if (g_sessions.size() >= 1024) { g_error = "native match registry capacity exceeded"; return nullptr; }
    }
    Session fresh;
    fresh.room_id = room_id; fresh.started = start; fresh.mission_id = mission; fresh.mode = mode;
    fresh.last_seen = GetTickCount64(); fresh.bosses = g_bosses; fresh.boss_catalog_loaded = g_bosses_loaded;
    if (!BossCatalog::Sha256(g_boot + ":" + std::to_string(++g_serial) + ":" + std::to_string(room_id) + ":" + std::to_string(start), &fresh.match_id))
        return nullptr;
    auto result = g_sessions.insert_or_assign(key, std::move(fresh));
    ++g_counters.matches_seen;
    return &result.first->second;
}

bool __stdcall EventGate(const native_combat_hook::NativeDamageSnapshot* event) {
    if (!event) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* session = CurrentSession(static_cast<BYTE*>(event->room));
    return session && !g_contract_failure && !session->frozen && session->combat.Valid();
}

bool __stdcall Classify(const native_combat_hook::NativeDamageSnapshot* event,
                        native_combat_hook::VictimClassification* result) {
    if (!event || !result) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* session = CurrentSession(static_cast<BYTE*>(event->room));
    if (!session || session->frozen) return false;
    result->npc = (event->victim_entity & 0x80000000u) != 0;
    if (MissionMode(event->room_mode)) {
        if (!session->boss_catalog_loaded) {
            g_error = "mission Boss identity registry is unavailable"; return false;
        }
        if (result->npc) {
            mission_native::MissionVictimIdentity identity;
            auto reader = &ReadNativeBytes;
            if (mission_native::ReadMissionVictim(event->room, event->victim_pilot, reader, &identity) != mission_native::ReadError::None) {
                g_error = "native mission NPC identity did not match the verified contract";
                session->combat.Invalidate("native mission NPC identity could not be verified");
                return false;
            }
            result->boss = session->bosses.IsBoss(identity.mission_id, identity.npc_data_id);
        }
        // Native mission actors outside the twelve party slots are not wallet
        // recipients. Retain their Boss damage/death so a wingman's final hit
        // does not erase a human's contribution; their other hits are irrelevant.
        if (event->attacker_slot >= 12 && !result->boss) return false;
    }
    result->accepted = true; return true;
}

void __stdcall Record(const native_combat_hook::NativeDamageSnapshot* event,
                      const native_combat_hook::VictimClassification* classification) {
    if (!event || !classification) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* session = CurrentSession(static_cast<BYTE*>(event->room));
    if (!session || session->frozen) return;
    CombatEvent hit{event->attacker_account, event->victim_entity, event->attacker_team, event->victim_team,
                    event->effective_damage, event->death_flag != 0, classification->npc, classification->boss};
    if (!session->combat.Record(hit, MissionMode(event->room_mode) ? Mode::Pve : Mode::Pvp))
        g_error = session->combat.Error();
}

void __stdcall CombatDiagnostic(native_combat_hook::DiagnosticReason reason,
                                const native_combat_hook::NativeDamageSnapshot* event) noexcept {
    using Reason = native_combat_hook::DiagnosticReason;
    if (reason != Reason::CallbackFault && reason != Reason::SnapshotFault && reason != Reason::InvalidClassification) return;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        Session* session = event ? CurrentSession(static_cast<BYTE*>(event->room)) : nullptr;
        if (session && !session->frozen) {
            session->combat.Invalidate("native combat collection failed; partial statistics cannot be paid");
            g_error = session->combat.Error();
        }
    } catch (...) {
        // Allocation failure cannot be repaired by pretending an empty damage
        // tracker is authoritative. Stop this integration until restart.
        g_contract_failure = true;
    }
}

bool Freeze(Session& session, BYTE* room, uint32_t duration) {
    if (session.frozen) return true;
    if (session.declined) { g_error = session.declined_error; return false; }
    if (!session.combat.Valid()) { g_error = session.combat.Error(); return false; }
    if (MissionMode(session.mode) && !session.boss_catalog_loaded) {
        g_error = "mission Boss identity registry is unavailable"; return false;
    }
    if (!g_runtime->SnapshotPolicy(&session.policy)) { g_error = g_runtime->LastError(); return false; }
    WalletHealth wallet = ReadWalletHealth(g_runtime->RuntimeRoot());
    if (!wallet.healthy) { g_error = wallet.error; return false; }
    native_stats::Participant roster[12]{};
    unsigned count = 0;
    if (!native_stats::ReadParticipants(room, roster, 12, &count)) { g_error = "unable to snapshot native participants"; return false; }
    unsigned humans = 0;
    std::array<uint32_t, 12> frozen_accounts{};
    std::set<uint32_t> accounts;
    for (unsigned i = 0; i < count; ++i) {
        if (roster[i].kind == native_stats::ParticipantKind::Human && roster[i].active) {
            if (!roster[i].account || !accounts.insert(roster[i].account).second) { g_error = "duplicate native wallet account"; return false; }
            frozen_accounts[humans] = roster[i].account;
            ++humans;
        }
    }
    if (!humans || humans > 12 || duration > 86400) { g_error = "invalid native attendance/duration"; return false; }
    session.context.mode = MissionMode(session.mode) ? Mode::Pve : Mode::Pvp;
    session.context.human_count = humans;
    session.context.duration_seconds = duration;
    session.context.bosses_defeated = session.combat.BossesDefeated();
    SYSTEMTIME local{}; GetLocalTime(&local);
    session.context.event_multiplier = g_daily.Factor(local.wHour * 60u + local.wMinute);
    if (session.context.mode == Mode::Pve) {
        mission_native::MissionSettlementIdentity identity;
        auto reader = &ReadNativeBytes;
        if (mission_native::ReadMissionSettlement(room, reader, &identity) != mission_native::ReadError::None) {
            g_error = "unknown native mission difficulty or completion state"; return false;
        }
        session.context.difficulty = identity.difficulty;
        session.context.outcome = identity.outcome;
    }
    session.wallet_scope = wallet.scope;
    session.accounts = frozen_accounts;
    session.account_count = humans;
    session.completed_unix = job_detail::UnixNow();
    session.frozen = true;
    return true;
}

bool WriteG(BYTE* frame, unsigned site, uint16_t amount) {
    return WriteNativeGResult(frame, site, amount);
}

void SettleImpl(unsigned site, BYTE* frame) {
    native_stats::NativeFrame native;
    native_stats::CaptureError capture;
    if (!native_stats::ReadSettlementFrame(site, frame, &native, &capture)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_error = "native settlement frame did not match the verified contract";
        g_contract_failure = true;
        ++g_counters.settlements_failed; return;
    }
    if (native.kind != native_stats::ParticipantKind::Human || !native.native_eligible) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_contract_failure) return;
    Session* session = CurrentSession(native.room);
    if (!session || !Freeze(*session, native.room, native.duration_seconds)) {
        if (session && !session->declined) {
            // Recovery during this native loop must not give later players a
            // different policy than participants whose settlement was declined.
            session->declined = true;
            session->declined_error = g_error;
        }
        ++g_counters.settlements_failed;
        g_api->Log(g_mod, SDGO_LOG_WARN, "new reward settlement declined for account %u: %s (original game path retained)", native.account, g_error.c_str());
        return;
    }
    if (std::find(session->accounts.begin(), session->accounts.begin() + session->account_count, native.account) ==
        session->accounts.begin() + session->account_count) {
        g_error = "settlement account changed after the match snapshot";
        ++g_counters.payouts_rejected; return;
    }
    if (std::find(session->settled.begin(), session->settled.begin() + session->settled_count, native.account) !=
        session->settled.begin() + session->settled_count) {
        // Do not generate another normal native G credit if the same final
        // callback is retried. MB also has a database-enforced receipt key.
        WriteG(frame, site, 0); ++g_counters.idempotent_replays; return;
    }
    MatchContext context = session->context;
    if (context.mode == Mode::Pve) {
        const auto outcome = native.outcome == native_stats::NativeOutcome::WinOrClear ? Outcome::Clear : Outcome::Fail;
        if (outcome != context.outcome) { g_error = "native mission completion sources disagree"; ++g_counters.settlements_failed; return; }
    }
    else context.outcome = native.outcome == native_stats::NativeOutcome::WinOrClear ? Outcome::Win :
                           native.outcome == native_stats::NativeOutcome::LoseOrFail ? Outcome::Lose : Outcome::Draw;
    ParticipantStats stats;
    stats.kind = ParticipantKind::Human;
    stats.active_seconds = (std::min)(native.duration_seconds, context.duration_seconds);
    stats.kills = native.kills;
    stats.is_mvp = context.mode == Mode::Pvp && native.native_rank_first;
    if (!session->combat.Apply(native.native_id, &stats)) { g_error = "native combat statistics unavailable"; ++g_counters.payouts_rejected; return; }
    Reward reward; Error error;
    if (!Calculate(session->policy, context, stats, &reward, &error, NativeSettlementLimits())) {
        g_error = error.message; ++g_counters.payouts_rejected; return;
    }
    if (reward.eligible) {
        if (g_pending.size() >= 4096) { g_error = "native wallet response queue is full"; ++g_counters.payouts_rejected; return; }
        Pending pending;
        pending.job = {session->match_id, "", session->policy.revision.data(), session->wallet_scope,
                       native.account, static_cast<uint32_t>(reward.mb)};
        pending.job.completed_unix = session->completed_unix;
        pending.job.active_seconds = stats.active_seconds;
        pending.job.contributed = stats.damage > 0 || stats.kills > 0 || stats.assists > 0 ||
            stats.minion_kills > 0 || stats.boss_credit_micro > 0;
        const std::string intent = pending.job.match_id + ":" + std::to_string(native.account) + ":" +
            pending.job.policy_revision + ":" + std::to_string(reward.gb) + ":" + std::to_string(reward.mb);
        if (!BossCatalog::Sha256(intent, &pending.job.plan_hash)) { g_error = "unable to hash native reward intent"; return; }
        if (!native_cash_notify::Capture(native.room_pilot, &pending.route)) {
            // A disconnect must not forfeit an earned durable MB award. The
            // native cash query refreshes it on the next login/store read.
            g_api->Log(g_mod, SDGO_LOG_INFO, "MB notification deferred to the native cash query for account %u (session route unavailable)", native.account);
        }
        pending.since = GetTickCount64();
        std::string name;
        if (!PublishWalletJob(g_runtime->RuntimeRoot(), pending.job, &name, &g_error)) { ++g_counters.payouts_rejected; return; }
        g_pending.push_back(std::move(pending));
    }
    if (!WriteG(frame, site, static_cast<uint16_t>(reward.gb))) { g_error = "unable to write native G settlement"; ++g_counters.settlements_failed; return; }
    session->settled[session->settled_count++] = native.account;
    if (!reward.eligible) ++g_counters.payouts_committed;
    ++g_counters.settlements_committed;
    if (g_trace) g_api->Log(g_mod, SDGO_LOG_INFO,
        "native reward prepared match %s account %u G=%d MB=%d event=%u humans=%u policy=%s",
        session->match_id.c_str(), native.account, reward.gb, reward.mb, context.event_multiplier, context.human_count, session->policy.revision.data());
}

void __stdcall Settle(unsigned site, BYTE* frame) noexcept {
    try { SettleImpl(site, frame); }
    catch (...) { g_contract_failure = true; }
}

void ReadReceipts() {
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        WalletReceipt receipt;
        std::string error;
        if (!ReadWalletReceipt(g_runtime->RuntimeRoot(), it->job, &receipt, &error)) {
            if (error != "wallet receipt pending") g_error = error;
            ++it; continue;
        }
        const auto now = job_detail::UnixNow();
        auto notified = native_cash_notify::NotifyResult::StaleSession;
        // Never replay a historical absolute balance into an active wallet UI.
        if (WalletReceiptFresh(receipt, now, 15))
            notified = native_cash_notify::NotifyCurrentMb(it->route, it->job.account, receipt.current_mb);
        if ((notified == native_cash_notify::NotifyResult::TransportFailure || notified == native_cash_notify::NotifyResult::BindingUnavailable) &&
            GetTickCount64() - it->since < 15000) { ++it; continue; }
        ++g_counters.payouts_committed;
        if (receipt.already_applied) ++g_counters.idempotent_replays;
        it = g_pending.erase(it);
    }
}

DWORD WINAPI Worker(void*) {
    for (;;) {
        try {
            std::string policyError;
            g_runtime->ReloadPolicy(&policyError);
            WalletHealth wallet = ReadWalletHealth(g_runtime->RuntimeRoot());
            g_runtime->SetWalletWorkerHealth(wallet.healthy, wallet.revision, wallet.error);
            BossCatalog catalog;
            std::string bossError;
            bool bossesLoaded = catalog.LoadFile((g_runtime->RuntimeRoot() / L"reward-bosses.txt").c_str(), &bossError);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_bosses_loaded = bossesLoaded;
                if (bossesLoaded) g_bosses = std::move(catalog);
                // A corrected config/worker connection may recover without a
                // server restart. A mismatched native frame stays latched.
                if (!g_contract_failure && wallet.healthy && policyError.empty() && bossesLoaded) g_error.clear();
                ReadReceipts();
                g_runtime->SetPayoutCounters(g_counters);
                const bool network = NetworkReady();
                g_runtime->SetSettlementReady(HooksActive() && network && bossesLoaded && !g_contract_failure && g_error.empty(),
                    !bossesLoaded ? bossError : !network ? "native network framework is not ready" :
                    g_contract_failure && g_error.empty() ? "native reward capture failed; inspect log and restart" : g_error);
            }
            SYSTEMTIME local{}; GetLocalTime(&local);
            char from[6], to[6];
            sprintf_s(from, "%02u:%02u", g_daily.start / 60, g_daily.start % 60);
            sprintf_s(to, "%02u:%02u", g_daily.end / 60, g_daily.end % 60);
            g_runtime->SetActivityWindow(from, to, g_daily.Factor(local.wHour * 60 + local.wMinute) == 2);
            g_runtime->WriteHeartbeat();
        } catch (...) {
            g_runtime->SetSettlementReady(false, "native reward worker failure");
            g_runtime->WriteHeartbeat();
        }
        Sleep(1000);
    }
}

bool Settings(const SdgoApi* api, SdgoMod* mod) {
    sdgo::ModSettings settings(api, mod);
    char root[4096] = {}, identity[96] = {};
    if (settings.String("RuntimeDirectory", "", root, sizeof(root)) >= sizeof(root) - 1 || !*root ||
        settings.String("InstanceId", "", identity, sizeof(identity)) >= sizeof(identity) - 1) return false;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root, -1, nullptr, 0);
    if (n < 2) return false;
    std::wstring runtime(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root, -1, runtime.data(), n);
    runtime.pop_back();
    wchar_t exe[32768] = {}; GetModuleFileNameW(nullptr, exe, 32768);
    fs::path directory = fs::path(exe).parent_path();
    fs::path runtimePath(runtime);
    if (runtimePath.is_relative()) runtimePath = directory / runtimePath;
    std::string instance(identity);
    if (instance.empty()) {
        std::string hash;
        std::wstring path(exe);
        if (!BossCatalog::Sha256(std::string(reinterpret_cast<const char*>(path.data()), path.size() * sizeof(wchar_t)), &hash)) return false;
        instance = "battle-" + hash.substr(0, 16);
    }
    std::array<UCHAR, 32> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
        !BossCatalog::Sha256(std::string(reinterpret_cast<const char*>(random.data()), random.size()), &g_boot)) return false;
    g_runtime = std::make_unique<RuntimeState>(runtimePath.wstring(), instance, RuntimeRootKind::ExplicitRuntimeDirectory);
    g_pending.reserve(4096); // no allocation after a durable MB job is published
    std::string error;
    if (!g_runtime->ReloadPolicy(&error)) {
        api->Log(mod, SDGO_LOG_ERROR, "reward policy rejected: %s", error.c_str());
        g_runtime->SetSettlementReady(false, error); g_runtime->WriteHeartbeat(); return false;
    }
    if (!g_daily.Load(GetModuleHandleW(L"daily_rewards.dll"))) return false;
    char trace[8] = {};
    settings.String("Trace", "0", trace, sizeof(trace));
    if (strcmp(trace, "0") && strcmp(trace, "1")) return false;
    g_trace = trace[0] == '1';
    return true;
}
}

SDGO_MOD_ENTRY {
    if (!api || api->version != SDGO_API_VERSION || api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) || !api->HookSite) return 0;
    g_api = api; g_mod = mod;
    if (!Settings(api, mod)) { api->Log(mod, SDGO_LOG_ERROR, "configure RuntimeDirectory using the owner reward assistant first"); return 0; }
    native_cash_notify::net_frame_proc_global = static_cast<void**>(api->Symbol(mod, "battle.rewards.net_frame_proc"));
    native_cash_notify::send_to_server = reinterpret_cast<native_cash_notify::SendToServerFn>(api->Symbol(mod, "battle.rewards.net_send"));
    if (!native_cash_notify::net_frame_proc_global || !native_cash_notify::send_to_server) return 0;
    native_combat_hook::event_gate = &EventGate;
    native_combat_hook::classify_victim = &Classify;
    native_combat_hook::record_sink = &Record;
    native_combat_hook::diagnostic_sink = &CombatDiagnostic;
    bridge::callback = &Settle;
    for (unsigned i = 0; i < kSettlementSites.size(); ++i) {
        g_sites[i] = static_cast<BYTE*>(api->Symbol(mod, kSettlementSites[i].symbol));
        if (!g_sites[i] || !api->HookSite(mod, kSettlementSites[i].symbol, bridge::detours[i], bridge::originals[i])) return 0;
    }
    g_sites[10] = static_cast<BYTE*>(api->Symbol(mod, "battle.rewards.record_damage"));
    if (!g_sites[10] || !api->HookCode(mod, "battle.rewards.record_damage", reinterpret_cast<void*>(&native_combat_hook::Detour), reinterpret_cast<void**>(&native_combat_hook::original))) return 0;
    HANDLE thread = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (!thread) return 0;
    CloseHandle(thread);
    api->Log(mod, SDGO_LOG_INFO, "native battle reward integration active; G uses original settlement, MB uses the owner durable native wallet worker");
    return 1;
}
