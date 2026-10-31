// Exercises the actual co-op mod's parser and native-title adapter, without a game.
#include "../mods/coop_ai/coop_ai.cpp"

namespace {
int failures = 0;
std::string providerTitle;
unsigned releases = 0;
uint8_t lobby[0x2218] = {};

void Check(bool value, const char* label)
{
    if (!value) {
        std::printf("FAIL %s\n", label);
        ++failures;
    }
}

uint32_t __cdecl Query(uint32_t pilot, char* out, uint32_t capacity)
{
    if (pilot != 1001 || providerTitle.empty() || capacity <= providerTitle.size())
        return 0;
    std::memcpy(out, providerTitle.c_str(), providerTitle.size() + 1);
    return static_cast<uint32_t>(providerTitle.size());
}

int __fastcall Find(void*, void*, const uint32_t* pilot, int* index)
{
    *index = *pilot == 1001 ? 7 : 0;
    return *index != 0;
}

uint8_t* __fastcall LockRoom(void*, void*, int index, int)
{
    return index == 7 ? lobby : nullptr;
}

void __fastcall ReleaseRoom(void*, void*, int) { ++releases; }
} // namespace

int main()
{
    g_settings.defaultCount = 0;
    g_settings.defaultLevel = 2;
    g_settings.requireRoomCommand = 1;
    Check(!ParseTitle("practice").enabled, "plain native title has no AI");
    auto plan = ParseTitle("!AI3 LV4");
    Check(plan.enabled && plan.count == 3 && plan.level == 4, "legacy commands preserve case and meaning");
    plan = ParseTitle("!ai lv5");
    Check(plan.enabled && plan.count == 0 && plan.level == 5, "bare command still fills free slots");
    Check(!ParseTitle("lv5").enabled, "level alone does not request AI");
    const std::string longTitle = std::string(32, 'x') + " !ai3 lv4";
    plan = ParseTitle(longTitle.c_str());
    Check(plan.enabled && plan.count == 3 && plan.level == 4, "rules past byte 20 are parsed");
    const std::string unicodeTitle = u8"一起完成任务，一起完成任务 !ai2 lv5";
    plan = ParseTitle(unicodeTitle.c_str());
    Check(plan.enabled && plan.count == 2 && plan.level == 5, "UTF-8 title with ASCII rule suffix");
    plan = ParseTitle((std::string("!ai") + std::string(100, '9') + " lv3").c_str());
    Check(plan.enabled && plan.count == 12 && plan.level == 3, "large counts saturate without integer overflow");
    for (uint8_t mode : {uint8_t{5}, uint8_t{8}, uint8_t{10}})
        Check(IsMissionMode(mode), "all co-op modes retain mission handling");
    for (uint8_t mode : {uint8_t{1}, uint8_t{2}, uint8_t{6}, uint8_t{9}, uint8_t{11}, uint8_t{12}})
        Check(!IsMissionMode(mode), "other modes are not treated as co-op");

    std::vector<uint8_t> proc(0x60);
    g_lobbyProc = proc.data();
    InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(g_lobbyProc + kLobbyMapLock));
    g_lobbyMapFind = reinterpret_cast<MapFindFn>(&Find);
    g_lobbyRoom = reinterpret_cast<LobbyRoomFn>(&LockRoom);
    g_lobbyRelease = reinterpret_cast<LobbyReleaseFn>(&ReleaseRoom);
    const std::string native = "practice !ai1 lv2";
    lobby[kLobbyTitleLength] = static_cast<uint8_t>(native.size());
    std::memcpy(lobby + kLobbyTitle, native.data(), native.size());
    const std::vector<uint8_t> original(lobby, lobby + sizeof(lobby));
    char title[SDGO_ROOM_TITLE_MAX_UTF8_BYTES + 1] = {};
    int index = 0;
    g_titleApi = nullptr;
    Check(LobbyTitle(1001, title, &index) && std::string(title) == native && index == 7,
          "no provider keeps the native title path");
    const SdgoRoomTitleApi api{sizeof(SdgoRoomTitleApi), 1, &Query};
    g_titleApi = &api;
    providerTitle = longTitle;
    Check(LobbyTitle(1001, title, &index) && std::string(title) == longTitle,
          "confirmed metadata replaces only the local parsing copy");
    plan = ParseTitle(title);
    Check(plan.count == 3 && plan.level == 4, "co-op adapter consumes full rules");
    providerTitle.clear();
    Check(LobbyTitle(1001, title, &index) && std::string(title) == native,
          "provider without room metadata falls back to native");
    Check(!LobbyTitle(9999, title, &index) && index == 0, "no cross-pilot title lookup");
    Check(releases == 3, "native room locks are released on both paths");
    Check(std::memcmp(lobby, original.data(), original.size()) == 0, "native room record is never enlarged or changed");
    DeleteCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(g_lobbyProc + kLobbyMapLock));
    std::puts(failures ? "CO-OP TITLE RULE CHECKS FAILED" : "ALL CO-OP TITLE RULE CHECKS PASSED");
    return failures ? 1 : 0;
}
