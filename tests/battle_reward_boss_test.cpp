#include <cstdio>
#include <fstream>
#include <iterator>
#include "mods/battle_rewards/boss_catalog.h"
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    sdgo::battle_rewards::BossCatalog registry;
    std::string error;
    if (!registry.LoadFile(argv[1], &error) || registry.Count() != 2 ||
        !registry.IsBoss(10, 20) || !registry.IsBoss(11, 30) || registry.IsBoss(11, 20)) {
        printf("FAIL native owner registry: %s\n", error.c_str()); return 1;
    }
    std::ifstream input(argv[1], std::ios::binary);
    std::string bytes(std::istreambuf_iterator<char>(input), {});
    bytes.back() = 'x';
    if (registry.Parse(bytes, &error) || error != "Boss registry checksum mismatch" || registry.Count() != 2) {
        puts("FAIL invalid registry replaced a prior verified snapshot"); return 1;
    }
    puts("ALL NATIVE BOSS REGISTRY CHECKS PASSED"); return 0;
}
