#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "sdgo/battle_rewards_policy.h"
#include "mods/battle_rewards/policy_config.h"

using namespace sdgo::battle_rewards;

namespace {

int checks = 0;
int failures = 0;

void Check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL line " << line << ": " << expression << "\n";
    }
}

#define CHECK(expression) Check(!!(expression), #expression, __LINE__)

std::string DefaultIni(const std::string& extra = std::string()) {
    return
        "[meta]\r\n"
        "SchemaVersion=1\r\n"
        "PolicyRevision=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\r\n"
        "[timing]\r\n"
        "FullRewardSeconds=180\r\n"
        "MinimumActiveSeconds=30\r\n"
        "[multipliers]\r\n"
        "Base=10\r\n"
        "ExtraHuman=1.1\r\n"
        "[pvp]\r\n"
        "BaseWinGb=500\r\nBaseWinMb=2\r\n"
        "BaseLoseGb=300\r\nBaseLoseMb=1\r\n"
        "BaseDrawGb=400\r\nBaseDrawMb=1.5\r\n"
        "KillGb=60\r\nKillMb=0.2\r\n"
        "AssistGb=20\r\nAssistMb=0.1\r\n"
        "DamagePer1000Gb=15\r\nDamagePer1000Mb=0.1\r\n"
        "ObjectiveGb=30\r\nObjectiveMb=0.1\r\n"
        "ContributionCapGb=400\r\nContributionCapMb=2\r\n"
        "MvpGb=200\r\nMvpMb=6\r\n"
        "[pve]\r\n"
        "BaseClearGb=100\r\nBaseClearMb=0\r\n"
        "BaseFailGb=35\r\nBaseFailMb=0\r\n"
        "MinionGb=2\r\nMinionMb=0\r\n"
        "BossCreditGb=45\r\nBossCreditMb=0.5\r\n"
        "DamagePer1000Gb=1\r\nDamagePer1000Mb=0\r\n"
        "ObjectiveGb=10\r\nObjectiveMb=0\r\n"
        "ContributionCapGb=150\r\nContributionCapMb=1.5\r\n"
        "TotalCap1xGb=300\r\nTotalCap1xMb=2\r\n"
        "DifficultyNormal=1\r\nDifficultyHard=1.25\r\nDifficultyExpert=1.5\r\n" +
        extra;
}

MatchContext PvpContext() {
    MatchContext context;
    context.mode = Mode::Pvp;
    context.outcome = Outcome::Win;
    context.human_count = 1;
    context.event_multiplier = 1;
    context.duration_seconds = 180;
    return context;
}

ParticipantStats PvpStats() {
    ParticipantStats stats;
    stats.active_seconds = 180;
    stats.kills = 3;
    stats.damage = 8000;
    return stats;
}

void CalculationTests() {
    const Policy policy = DefaultPolicy();
    Error error;
    Reward reward;

    MatchContext context = PvpContext();
    ParticipantStats stats = PvpStats();
    CHECK(Calculate(policy, context, stats, &reward, &error));
    CHECK(reward.gb == 8000);
    CHECK(reward.mb == 34);

    context.human_count = 4;
    context.event_multiplier = 2;
    stats.is_mvp = true;
    CHECK(Calculate(policy, context, stats, &reward, &error));
    CHECK(reward.gb == 26620);
    CHECK(reward.mb == 250);

    context = {};
    context.mode = Mode::Pve;
    context.outcome = Outcome::Clear;
    context.human_count = 4;
    context.event_multiplier = 2;
    context.duration_seconds = 180;
    context.bosses_defeated = 1;
    stats = {};
    stats.active_seconds = 180;
    stats.minion_kills = 15;
    stats.boss_credit_micro = 1000000;
    stats.damage = 8000;
    CHECK(Calculate(policy, context, stats, &reward, &error));
    CHECK(reward.gb == 4871);
    CHECK(reward.mb == 13);

    context = PvpContext();
    context.duration_seconds = 60;
    stats = PvpStats();
    stats.active_seconds = 60;
    stats.kills = 255;
    stats.damage = 100000;
    CHECK(Calculate(policy, context, stats, &reward, &error));
    CHECK(reward.gb == 3000);

    context = PvpContext();
    context.human_count = 12;
    context.event_multiplier = 2;
    stats = PvpStats();
    stats.kills = 255;
    stats.assists = 255;
    stats.damage = 100000000;
    stats.objectives = 1000;
    stats.is_mvp = true;
    CHECK(Calculate(policy, context, stats, &reward, &error,
                    NativeSettlementLimits()));
    CHECK(reward.gb == 62768);
    Policy excessive = policy;
    excessive.base_multiplier_micro = 100000000;
    CHECK(!Calculate(excessive, context, stats, &reward, &error,
                     NativeSettlementLimits()));
    CHECK(error.code == ErrorCode::WalletRange);

    context = PvpContext();
    stats = PvpStats();
    stats.kind = ParticipantKind::Ai;
    CHECK(Calculate(policy, context, stats, &reward, &error));
    CHECK(!reward.eligible && reward.gb == 0 && reward.mb == 0);
    stats.kind = ParticipantKind::Human;
    stats.active_seconds = 20;
    CHECK(Calculate(policy, context, stats, &reward, &error));
    CHECK(reward.reason == EligibilityReason::TooShort);

    context.human_count = 0;
    CHECK(!Calculate(policy, context, stats, &reward, &error));
    CHECK(error.code == ErrorCode::InvalidHumanCount);
    context = PvpContext();
    stats = PvpStats();
    stats.boss_credit_micro = 1;
    CHECK(!Calculate(policy, context, stats, &reward, &error));
    CHECK(error.code == ErrorCode::InvalidBossCredit);

    context = PvpContext();
    stats = {};
    stats.active_seconds = 180;
    stats.is_mvp = true;
    CHECK(Calculate(policy, context, stats, &reward, &error));
    CHECK(reward.gb == 5000 && reward.mb == 20);

    context = {};
    context.mode = Mode::Pve;
    context.outcome = Outcome::Clear;
    context.difficulty = Difficulty::Expert;
    context.human_count = 1;
    context.event_multiplier = 1;
    context.duration_seconds = 180;
    context.bosses_defeated = 100;
    stats = {};
    stats.active_seconds = 180;
    stats.minion_kills = 10000;
    stats.damage = 100000000;
    stats.boss_credit_micro = 100000000;
    stats.is_mvp = true;
    CHECK(Calculate(policy, context, stats, &reward, &error));
    CHECK(reward.gb == 3000 && reward.mb == 20);

    context.mode = static_cast<Mode>(99);
    CHECK(!Calculate(policy, context, stats, &reward, &error));
    CHECK(error.code == ErrorCode::InvalidMode);
}

void ParserTests() {
    Policy policy;
    PolicyConfigError error;
    CHECK(ParsePolicyIni(DefaultIni(), &policy, &error));
    CHECK(policy.base_multiplier_micro == 10000000);
    CHECK(policy.pvp.base_draw.mb_micro == 1500000);
    CHECK(policy.pve.difficulty_expert_micro == 1500000);
    CHECK(policy.revision[0] == '0' && policy.revision[63] == 'f');

    CHECK(!ParsePolicyIni(DefaultIni("[unknown]\r\nValue=1\r\n"),
                          &policy, &error));
    CHECK(error.message.find("unknown key") != std::string::npos);

    std::string invalid = DefaultIni();
    const size_t base = invalid.find("Base=10");
    invalid.replace(base, 7, "Base=10.0000001");
    CHECK(!ParsePolicyIni(invalid, &policy, &error));

    invalid = DefaultIni();
    const size_t human = invalid.find("ExtraHuman=1.1");
    invalid.replace(human, 14, "ExtraHuman=1.2");
    CHECK(!ParsePolicyIni(invalid, &policy, &error));
    CHECK(error.message.find("metadata") != std::string::npos);

    invalid = DefaultIni();
    invalid += "[pvp]\r\nKillGb=1\r\n";
    CHECK(!ParsePolicyIni(invalid, &policy, &error));
    CHECK(error.message.find("duplicate key") != std::string::npos);

    invalid = DefaultIni();
    const size_t maximum = invalid.find("BaseWinGb=500");
    invalid.replace(maximum, 13, "BaseWinGb=1000000");
    CHECK(ParsePolicyIni(invalid, &policy, &error));
    CHECK(policy.pvp.base_win.gb_micro == 1000000000000ULL);

    invalid = DefaultIni();
    const size_t excessive = invalid.find("BaseWinGb=500");
    invalid.replace(excessive, 13, "BaseWinGb=1000000.000001");
    CHECK(!ParsePolicyIni(invalid, &policy, &error));
}

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> fields;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab - start));
        if (tab == std::string::npos) return fields;
        start = tab + 1;
    }
}

uint32_t U32(const std::string& text) {
    return static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
}

int32_t I32(const std::string& text) {
    return static_cast<int32_t>(std::strtol(text.c_str(), nullptr, 10));
}

bool RunBatch(const Policy& policy) {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::vector<std::string> f = Split(line);
        if (f.size() != 19) {
            std::cout << "?\tERR\tinput\n";
            continue;
        }
        MatchContext context;
        context.mode = f[1] == "pvp" ? Mode::Pvp : Mode::Pve;
        if (f[2] == "win") context.outcome = Outcome::Win;
        else if (f[2] == "lose") context.outcome = Outcome::Lose;
        else if (f[2] == "draw") context.outcome = Outcome::Draw;
        else if (f[2] == "clear") context.outcome = Outcome::Clear;
        else context.outcome = Outcome::Fail;
        if (f[3] == "hard") context.difficulty = Difficulty::Hard;
        else if (f[3] == "expert") context.difficulty = Difficulty::Expert;
        else context.difficulty = Difficulty::Normal;
        context.human_count = U32(f[4]);
        context.event_multiplier = U32(f[5]);
        context.duration_seconds = U32(f[6]);
        context.bosses_defeated = U32(f[7]);

        ParticipantStats stats;
        if (f[8] == "ai") stats.kind = ParticipantKind::Ai;
        else if (f[8] == "observer") stats.kind = ParticipantKind::Observer;
        stats.active_seconds = U32(f[9]);
        stats.kills = U32(f[10]);
        stats.assists = U32(f[11]);
        stats.minion_kills = U32(f[12]);
        stats.damage = U32(f[13]);
        stats.objectives = U32(f[14]);
        stats.boss_credit_micro = U32(f[15]);
        stats.is_mvp = U32(f[16]) != 0;

        Reward reward;
        Error error;
        if (!Calculate(policy, context, stats, &reward, &error,
                       DeliveryLimits{I32(f[17]), I32(f[18])})) {
            std::cout << f[0] << "\tERR\t" << static_cast<int>(error.code) << "\n";
        } else {
            std::cout << f[0] << "\tOK\t" << reward.gb << "\t" << reward.mb
                      << "\t" << (reward.eligible ? 1 : 0) << "\t"
                      << static_cast<int>(reward.reason) << "\n";
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--batch") {
        Policy policy = DefaultPolicy();
        if (argc >= 3) {
            PolicyConfigError error;
            if (!LoadPolicyIniFile(argv[2], &policy, &error)) {
                std::cerr << "policy load failed: " << error.message << "\n";
                return 2;
            }
        }
        return RunBatch(policy) ? 0 : 2;
    }
    CalculationTests();
    ParserTests();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
