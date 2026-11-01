#pragma once
#include <cstdint>
#include <cstddef>

namespace sdgo::starter {
struct Settings {
    int gp = 5000;
    int cash = 0;
    int rank = 5;
    int step = 1;
};
struct RankStart { int experience = 0; };
// The native input/output structures are packed, as established by the verified
// OLE DB binding descriptors. They contain no pointers or server credentials.
#pragma pack(push, 1)
struct LoginInput {
    char id[15];
    char password[33];
    char ip[17];
    int agent;
    int result;
};
struct UserInput {
    int account;
    char pilot[17];
    uint8_t rank;
    int step;
    int expRank, killRank, birth;
    uint8_t pcBang;
    int16_t agentPort;
    char accountId[13];
    uint8_t tutorial;
    int result;
};
struct UserOutput {
    int experience, kills, deaths, battles, wins, losses, ties, platoon;
    uint8_t rank;
    int step;
};
#pragma pack(pop)
static_assert(sizeof(LoginInput) == 0x49 && offsetof(LoginInput, result) == 0x45);
static_assert(sizeof(UserInput) == 0x3b && offsetof(UserInput, result) == 0x37);
static_assert(sizeof(UserOutput) == 0x25 && offsetof(UserOutput, rank) == 0x20);

struct Database {
    virtual ~Database() = default;
    virtual bool Account(const char* id, bool lock, int& account) = 0;
    virtual bool User(int account, bool lock, bool& exists) = 0;
    virtual bool Begin() = 0;
    virtual bool Active() = 0;
    virtual bool Commit() = 0;
    virtual void Abort() = 0;
    virtual bool Wallet(int account, const Settings&) = 0;
    virtual bool Rank(const Settings&, RankStart&) = 0;
    virtual bool Profile(int account, const Settings&, const RankStart&) = 0;
};
class Transaction {
    Database& db_;
    bool active_ = false;
public:
    explicit Transaction(Database& db) : db_(db) {}
    ~Transaction() { if (active_) db_.Abort(); }
    bool Begin() { active_ = db_.Begin(); return active_; }
    bool Commit() { if (!db_.Commit()) return false; active_ = false; return true; }
};
constexpr int kDatabaseError = 1000;

template<class Native>
int Login(Database& db, const Settings& settings, LoginInput& input, Native native) {
    int account = 0;
    if (!db.Account(input.id, false, account)) { input.result = 4; return kDatabaseError; }
    if (account) return native(input);
    Transaction transaction(db);
    if (!transaction.Begin() || !db.Account(input.id, true, account)) { input.result = 4; return kDatabaseError; }
    if (account) {
        if (!transaction.Commit()) { input.result = 4; return kDatabaseError; }
        return native(input);
    }
    const int result = native(input); // normal auto-registration may return login-failed once
    if (!db.Active()) { input.result = 4; return kDatabaseError; }
    if (!db.Account(input.id, false, account)) { input.result = 4; return kDatabaseError; }
    if (account && !db.Wallet(account, settings)) { input.result = 4; return kDatabaseError; }
    if (!transaction.Commit()) { input.result = 4; return kDatabaseError; }
    return result; // preserve the original retry/login/result protocol
}

template<class Native>
int User(Database& db, const Settings& settings, UserInput& input, UserOutput& output, Native native) {
    bool exists = false;
    if (!db.User(input.account, false, exists)) { input.result = 1; return kDatabaseError; }
    if (exists) return native(input, output);
    Transaction transaction(db);
    if (!transaction.Begin() || !db.User(input.account, true, exists)) { input.result = 1; return kDatabaseError; }
    if (exists) {
        if (!transaction.Commit()) { input.result = 1; return kDatabaseError; }
        return native(input, output);
    }
    RankStart rank;
    if (!db.Rank(settings, rank) || rank.experience < 0) { input.result = 1; return kDatabaseError; }
    UserInput staged = input;
    staged.rank = static_cast<uint8_t>(settings.rank);
    staged.step = settings.step;
    UserOutput stagedOutput = output;
    const int result = native(staged, stagedOutput);
    input.tutorial = staged.tutorial;
    input.result = staged.result;
    if (result || staged.result) return result ? result : kDatabaseError;
    if (!db.Active() || !db.Profile(input.account, settings, rank) || !db.Wallet(input.account, settings)) {
        input.result = 1; return kDatabaseError;
    }
    if (!transaction.Commit()) { input.result = 1; return kDatabaseError; }
    stagedOutput.experience = rank.experience;
    stagedOutput.rank = static_cast<uint8_t>(settings.rank);
    stagedOutput.step = settings.step;
    output = stagedOutput; // publish the corrected row only after commit
    return result;
}
}
