// Exercises the actual PvP mod's parser and room-copy adapter, without a game.
#include "../mods/pvp_ai/pvp_ai.cpp"

namespace {
int failures = 0;
std::string providerTitle;

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
} // namespace

int main()
{
    g_settings.defaultLevel = 2;
    auto plan = ParseTitle("!AI e3 a1 LV4");
    Check(plan.enabled && plan.enemies == 3 && plan.allies == 1 && plan.level == 4,
          "legacy enemy, ally and level commands preserve case and meaning");
    plan = ParseTitle("!ai");
    Check(plan.enabled && plan.enemies == kFill && plan.allies == kFill, "bare command keeps fill behavior");
    plan = ParseTitle("!ai3");
    Check(plan.enemies == 3 && plan.allies == 0, "count shorthand still requests enemies");
    Check(!ParseTitle("practice lv5").enabled, "level alone does not request AI");
    const std::string longTitle = std::string(32, 'x') + " !ai e3 a1 lv5";
    plan = ParseTitle(longTitle.c_str());
    Check(plan.enabled && plan.enemies == 3 && plan.allies == 1 && plan.level == 5,
          "all PvP options past byte 20 are parsed");
    plan = ParseTitle((std::string(32, 'x') + " !ai e2 !rand a5 lv5").c_str());
    Check(plan.enabled && plan.enemies == 2 && plan.allies == 0 && plan.level == 2,
          "full title does not consume options owned by another command");
    plan = ParseTitle(u8"一起练习对战，一起练习对战 !ai e2 a2 lv4");
    Check(plan.enabled && plan.enemies == 2 && plan.allies == 2 && plan.level == 4,
          "UTF-8 title retains ASCII rule suffix");
    plan = ParseTitle(u8"一起练习对战，一起练习对战 !AI e2 a1 lv4 HARD !rand easy");
    Check(plan.enabled && plan.enemies == 2 && plan.allies == 1 && plan.level == 4 &&
              plan.difficulty == battle_ai::Difficulty::Hard,
          "full UTF-8 difficulty composes with pool rules without reading the next command");
    Check(ParseTitle("!ai !rand hard").difficulty == battle_ai::Difficulty::Medium,
          "random-loadout arguments cannot set AI difficulty");
    Check(ParseTitle("!ai easy !ai hard").difficulty == battle_ai::Difficulty::Easy,
          "the first AI command owns its difficulty");
    plan = ParseTitle((std::string("!ai e") + std::string(100, '9') + " a0 lv3").c_str());
    Check(plan.enabled && plan.enemies == kSlotsPerTeam && plan.allies == 0 && plan.level == 3,
          "long numeric fields saturate without integer overflow");
    for (uint8_t mode : {uint8_t{1}, uint8_t{2}, uint8_t{3}, uint8_t{4}, uint8_t{6}, uint8_t{7}, uint8_t{9}})
        Check(IsPvpMode(mode), "all normal PvP modes retain handling");
    for (uint8_t mode : {uint8_t{5}, uint8_t{8}, uint8_t{10}, uint8_t{11}, uint8_t{12}})
        Check(!IsPvpMode(mode), "co-op and native AI modes keep separate handling");

    std::vector<uint8_t> room(0x32880);
    const std::string native = "practice !ai1 lv2";
    room[kRoomTitleLength] = static_cast<uint8_t>(native.size());
    std::memcpy(room.data() + kRoomTitle, native.data(), native.size());
    const uint32_t host = 1001;
    std::memcpy(room.data() + kRoomHost, &host, sizeof(host));
    const auto original = room;
    g_titleApi = nullptr;
    Check(std::string(ViewRoom(room.data()).title) == native, "no provider keeps native room-copy title");
    const SdgoRoomTitleApi api{sizeof(SdgoRoomTitleApi), 1, &Query};
    g_titleApi = &api;
    providerTitle = longTitle;
    auto view = ViewRoom(room.data());
    Check(view.titled && std::string(view.title) == longTitle, "host pilot resolves confirmed full metadata");
    plan = ParseTitle(view.title);
    Check(plan.enemies == 3 && plan.allies == 1 && plan.level == 5, "PvP adapter consumes full rules");
    providerTitle.clear();
    Check(std::string(ViewRoom(room.data()).title) == native, "no metadata falls back to native");
    const uint32_t otherHost = 9999;
    std::memcpy(room.data() + kRoomHost, &otherHost, sizeof(otherHost));
    providerTitle = longTitle;
    Check(std::string(ViewRoom(room.data()).title) == native, "other room host cannot borrow metadata");
    std::memcpy(room.data() + kRoomHost, &host, sizeof(host));
    Check(room == original, "adapter never enlarges or changes the native room");
    std::puts(failures ? "PVP TITLE RULE CHECKS FAILED" : "ALL PVP TITLE RULE CHECKS PASSED");
    return failures ? 1 : 0;
}
