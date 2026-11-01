// Explicit read-only inventory probe. No game process is started or modified.
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <cstdlib>
#include "../mods/room_random/inventory.h"

int wmain(int argc,wchar_t** argv) {
    if(argc!=3) { std::fwprintf(stderr,L"usage: room_random_inventory_probe <WorldServer.ini> <owner-id>\n"); return 2; }
    wchar_t* end=nullptr;
    const unsigned long owner=std::wcstoul(argv[2],&end,10);
    if(!end || *end || owner>INT32_MAX) return 2;
    room_random_inventory::Settings settings;
    settings.databaseIni=argv[1];
    std::vector<sdgo::random_loadout::Candidate> inventory;
    std::string error;
    if(!room_random_inventory::Read(settings,{static_cast<uint32_t>(owner)},inventory,error)) {
        std::fprintf(stderr,"read-only inventory probe: %s\n",error.c_str()); return 1;
    }
    size_t eligible=0;
    for(const auto& u:inventory) if(u.eligible) ++eligible;
    std::printf("read-only inventory probe passed: %zu rows, %zu eligible; no database writes\n",inventory.size(),eligible);
    return 0;
}
