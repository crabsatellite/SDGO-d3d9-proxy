// Per-round, room-title-controlled random HUMAN loadouts. The native battle
// constructors and start packets remain responsible for stats and replication.
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/mod_settings.h"
#include "sdgo/random_loadout.h"
#include "inventory.h"

namespace {
namespace policy = sdgo::random_loadout;
constexpr size_t kSlotBase=0x533, kSlotStride=0x48E, kOwner=0x429;
constexpr size_t kActive=0x49, kDeck=0x19E, kActiveSerial=0x162;
constexpr size_t kTitleLength=0x79, kTitle=0x7A, kTitleMax=20;
constexpr uint32_t kAiBit=0x80000000u;
constexpr int kStartError=0xFBE;
using DispatchFn=int(__thiscall*)(void*,int,void*,uint32_t*,uint32_t*);
using SpecialFn=int(__thiscall*)(void*,void*,uintptr_t);
using ValidateFn=int(__thiscall*)(void*,uint8_t*,int);
using BuildFn=int(__thiscall*)(void*,uint8_t*,uint8_t*,uint8_t*,uint8_t,uint8_t);
using ReleaseFn=void(__thiscall*)(uint8_t*);
using UnitLookupFn=uint8_t*(__thiscall*)(void*,uint32_t);
using LobbyFindFn=int(__thiscall*)(void*,const uint32_t*,int*);
using LobbyRoomFn=uint8_t*(__thiscall*)(void*,int,uint16_t);
using LobbyUnlockFn=void(__thiscall*)(void*,int);
using InventoryFn=bool(*)(const room_random_inventory::Settings&,const std::vector<uint32_t>&,
                         std::vector<policy::Candidate>&,std::string&);

const SdgoApi* g_api=nullptr;
SdgoMod* g_mod=nullptr;
DispatchFn g_dispatch=nullptr;
SpecialFn g_special=nullptr;
ValidateFn g_validate=nullptr;
BuildFn g_build=nullptr;
ReleaseFn g_release=nullptr;
UnitLookupFn g_lookup=nullptr;
void* g_unitTable=nullptr;
void* g_lobby=nullptr;
LobbyFindFn g_lobbyFind=nullptr;
LobbyRoomFn g_lobbyRoom=nullptr;
LobbyUnlockFn g_lobbyUnlock=nullptr;
InventoryFn g_inventory=&room_random_inventory::Read;
room_random_inventory::Settings g_settings;
bool g_log=true;

template<class T> T Get(const uint8_t* bytes,size_t offset) {
    T value; std::memcpy(&value,bytes+offset,sizeof(value)); return value;
}
template<class T> void Put(uint8_t* bytes,size_t offset,T value) {
    std::memcpy(bytes+offset,&value,sizeof(value));
}

struct Signature {
    uint32_t key=0;
    uint32_t host=0;
    uint8_t type=0,mode=0,minimum=0,maximum=0,onlyC=0;
    std::array<uint32_t,12> owners{};
    std::array<uint8_t,12> ready{};
};
bool SignatureOf(uint8_t* room,Signature* out) {
    __try {
        out->key=Get<uint32_t>(room,0);
        out->host=Get<uint32_t>(room,0x4CE);
        out->type=room[4]; out->mode=room[5]; out->minimum=room[0x14]; out->maximum=room[0x15]; out->onlyC=room[0x20];
        for(int i=0;i<12;++i) {
            out->owners[i]=Get<uint32_t>(room,kSlotBase+i*kSlotStride+kOwner);
            out->ready[i]=room[kSlotBase+i*kSlotStride+0x42F];
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SameHumans(const Signature& a,const Signature& b) {
    if(a.key!=b.key || a.host!=b.host || a.type!=b.type || a.mode!=b.mode || a.minimum!=b.minimum
       || a.maximum!=b.maximum || a.onlyC!=b.onlyC) return false;
    for(size_t i=0;i<a.owners.size();++i) {
        const uint32_t x=(a.owners[i]&kAiBit)?0:a.owners[i];
        const uint32_t y=(b.owners[i]&kAiBit)?0:b.owners[i];
        if(x!=y) return false;
        if(x && a.ready[i]!=b.ready[i]) return false;
    }
    return true;
}
bool RawTitle(uint8_t* room,char* text) {
    __try {
        const size_t n=room[kTitleLength];
        if(n>kTitleMax) return false;
        std::memcpy(text,room+kTitle,n); text[n]=0;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool LobbyTitle(uint32_t owner,char* text) {
    if(!g_lobby || !g_lobbyFind || !g_lobbyRoom || !g_lobbyUnlock) return false;
    int index=0;
    EnterCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(static_cast<uint8_t*>(g_lobby)+0x38));
    const int found=g_lobbyFind(static_cast<uint8_t*>(g_lobby)+0x14,&owner,&index);
    LeaveCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(static_cast<uint8_t*>(g_lobby)+0x38));
    if(!found || index<=0) return false;
    uint8_t* room=g_lobbyRoom(g_lobby,index,0);
    if(!room) return false;
    const size_t n=room[0x53];
    bool ok=n<=kTitleMax;
    if(ok) { std::memcpy(text,room+0x54,n); text[n]=0; }
    g_lobbyUnlock(g_lobby,index);
    return ok;
}
bool Requested(uint8_t* room,const Signature& signature) {
    char title[kTitleMax+1]{};
    if(RawTitle(room,title) && policy::Command(title,std::strlen(title),g_api)) return true;
    // Use the authoritative lobby title as a fallback on every native path;
    // special starts need not retain the PvP room's inline title.
    for(uint32_t owner:signature.owners)
        if(owner && !(owner&kAiBit) && LobbyTitle(owner,title))
            return policy::Command(title,std::strlen(title),g_api);
    return false;
}
bool UsesDeck(uint8_t mode) { return mode==6 || mode==7; }

struct Source {
    uint32_t owner=0;
    std::array<uint8_t,0x99> active{};
    std::array<uint8_t,4*0x99> deck{};
    uint64_t activeSerial=0;
};
bool Capture(uint8_t* info,Source* source) {
    __try {
        source->owner=Get<uint32_t>(info,kOwner);
        std::memcpy(source->active.data(),info+kActive,source->active.size());
        std::memcpy(source->deck.data(),info+kDeck,source->deck.size());
        source->activeSerial=Get<uint64_t>(info,kActiveSerial);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Apply(uint8_t* info,const policy::Selection& selection,bool deck) {
    if(selection.units.empty() || selection.units.size()>4) return false;
    __try {
        std::memcpy(info+kActive,selection.units[0].data.data(),0x99);
        info[kActive+6]=1;
        if(deck) {
            std::memset(info+kDeck,0,4*0x99);
            for(size_t i=0;i<selection.units.size();++i) {
                std::memcpy(info+kDeck+i*0x99,selection.units[i].data.data(),0x99);
                info[kDeck+i*0x99+6]=i==0?1:0;
            }
            Put(info,kActiveSerial,selection.units[0].serial);
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Restore(uint8_t* info,const Source& source,bool deck) {
    __try {
        if(Get<uint32_t>(info,kOwner)!=source.owner) return false;
        std::memcpy(info+kActive,source.active.data(),source.active.size());
        if(deck) {
            std::memcpy(info+kDeck,source.deck.data(),source.deck.size());
            Put(info,kActiveSerial,source.activeSerial);
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct Player {
    int slot=0;
    uint32_t owner=0;
    policy::Selection selection;
    Source previewOriginal;
};
struct SavedRoom {
    uint64_t token=0;
    uint32_t key=0;
    bool deck=false;
    std::vector<std::pair<int,Source>> players;
};
struct History { std::vector<uint32_t> units; uint64_t stamp=0; };
std::mutex g_state;
std::map<uint8_t*,SavedRoom> g_saved;
std::map<uint64_t,History> g_history;
std::atomic<uint64_t> g_token{0};

struct Attempt {
    uint64_t token=0;
    bool checked=false,enabled=false,prepared=false,applied=false,failed=false;
    uint8_t* preview=nullptr;
    uint8_t* actual=nullptr;
    Signature signature;
    std::vector<Player> players;
    std::string error;
};
thread_local Attempt* g_attempt=nullptr;
uint64_t HistoryKey(uint32_t room,uint32_t owner) { return (static_cast<uint64_t>(room)<<32)|owner; }
void Fail(Attempt& a,const char* message) {
    if(!a.failed) g_api->Log(g_mod,SDGO_LOG_WARN,"random start cancelled: %s",message);
    a.failed=true; a.error=message;
}
std::vector<uint32_t> Previous(uint32_t key,uint32_t owner) {
    std::lock_guard<std::mutex> lock(g_state);
    auto it=g_history.find(HistoryKey(key,owner));
    return it==g_history.end()?std::vector<uint32_t>{}:it->second.units;
}
bool ResolveUnit(policy::Candidate& u) {
    __try {
        uint8_t* native=g_lookup(g_unitTable,u.unit);
        if(!native) return false;
        u.rank=native[6]; u.points=native[0xB];
        return u.rank>=1 && u.rank<=4 && u.points>=1 && u.points<=10;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Prepare(Attempt& a,uint8_t* room) {
    if(a.failed) return false;
    if(a.prepared) return true;
    if(!SignatureOf(room,&a.signature)) { Fail(a,"unreadable room"); return false; }
    if(!a.checked) { a.enabled=Requested(room,a.signature); a.checked=true; }
    if(!a.enabled) return true;
    std::vector<uint32_t> owners;
    for(uint32_t owner:a.signature.owners)
        if(owner && !(owner&kAiBit)) {
            if(std::find(owners.begin(),owners.end(),owner)!=owners.end()) {
                Fail(a,"duplicate human owner in room"); return false;
            }
            owners.push_back(owner);
        }
    if(owners.empty()) { Fail(a,"room has no human players"); return false; }
    policy::Restrictions restriction;
    if(a.signature.minimum) {
        if(a.signature.minimum>4 || a.signature.maximum<a.signature.minimum || a.signature.maximum>4) {
            Fail(a,"invalid room rank range"); return false;
        }
        restriction.minimumRank=a.signature.minimum; restriction.maximumRank=a.signature.maximum;
    }
    restriction.onlyC=a.signature.onlyC!=0;
    std::vector<policy::Candidate> inventory;
    std::string error;
    if(!g_inventory(g_settings,owners,inventory,error)) { Fail(a,error.c_str()); return false; }
    for(auto& u:inventory) u.eligible=u.eligible && ResolveUnit(u);
    uint64_t seed=0;
    if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&seed),sizeof(seed),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0) {
        Fail(a,"random source unavailable"); return false;
    }
    std::mt19937_64 rng(seed);
    const bool deck=UsesDeck(a.signature.mode);
    // No real room is written until every human has a valid plan.
    for(int slot=0;slot<12;++slot) {
        const uint32_t owner=a.signature.owners[slot];
        if(!owner || (owner&kAiBit)) continue;
        Player p; p.owner=owner; p.slot=slot;
        p.selection=policy::Choose(inventory,owner,restriction,deck,Previous(a.signature.key,owner),rng);
        if(p.selection.error!=policy::Error::None) {
            g_api->Log(g_mod,SDGO_LOG_WARN,"owner %lu cannot randomize: %s",static_cast<unsigned long>(owner),
                p.selection.error==policy::Error::NoExactDeck ? "no owned deck totals exactly 10 points in four slots" : "no eligible owned loadout");
            Fail(a,"one player's eligible inventory cannot satisfy the room"); return false;
        }
        if(!Capture(room+kSlotBase+slot*kSlotStride,&p.previewOriginal)) {
            Fail(a,"unreadable source loadout"); return false;
        }
        a.players.push_back(std::move(p));
    }
    a.preview=room;
    a.prepared=true;
    for(const auto& p:a.players)
        if(!Apply(room+kSlotBase+p.slot*kSlotStride,p.selection,deck)) {
            for(const auto& rollback:a.players) Restore(room+kSlotBase+rollback.slot*kSlotStride,rollback.previewOriginal,deck);
            Fail(a,"cannot prepare native validation snapshot"); return false;
        }
    return true;
}

bool ApplyActual(Attempt& a,uint8_t* room) {
    if(a.applied) return a.actual==room;
    Signature current;
    if(!SignatureOf(room,&current) || !SameHumans(a.signature,current)) {
        Fail(a,"room membership or restrictions changed during preparation"); return false;
    }
    SavedRoom saved; saved.token=a.token; saved.key=current.key; saved.deck=UsesDeck(current.mode);
    for(const auto& p:a.players) {
        Source source;
        if(room==a.preview) source=p.previewOriginal;
        else if(!Capture(room+kSlotBase+p.slot*kSlotStride,&source)) {
            Fail(a,"cannot capture real room loadout"); return false;
        }
        saved.players.emplace_back(p.slot,source);
    }
    {
        std::lock_guard<std::mutex> lock(g_state);
        if(g_saved.find(room)!=g_saved.end()) { Fail(a,"previous battle has not released its loadouts"); return false; }
        g_saved.emplace(room,saved);
    }
    a.actual=room; a.applied=true;
    for(const auto& p:a.players)
        if(!Apply(room+kSlotBase+p.slot*kSlotStride,p.selection,saved.deck)) {
            Fail(a,"cannot apply native room loadout"); return false;
        }
    return true;
}
void RestoreActual(uint8_t* room,uint64_t token=0) {
    std::lock_guard<std::mutex> lock(g_state);
    auto it=g_saved.find(room);
    if(it==g_saved.end() || (token && it->second.token!=token)) return;
    Signature current;
    if(SignatureOf(room,&current) && current.key==it->second.key)
        for(const auto& entry:it->second.players)
            Restore(room+kSlotBase+entry.first*kSlotStride,entry.second,it->second.deck);
    g_saved.erase(it);
}
void Commit(const Attempt& a) {
    std::lock_guard<std::mutex> lock(g_state);
    for(const auto& p:a.players) {
        g_history[HistoryKey(a.signature.key,p.owner)]={policy::Identity(p.selection.units),a.token};
        if(g_log) {
            char message[192]{}; size_t used=0; unsigned points=0;
            for(const auto& u:p.selection.units) {
                used+=static_cast<size_t>(sprintf_s(message+used,sizeof(message)-used,"%s%lu(%u)",used?" + ":"",
                      static_cast<unsigned long>(u.unit),u.points));
                points+=u.points;
            }
            g_api->Log(g_mod,SDGO_LOG_INFO,"round %llu owner %lu: %s; %u points%s",a.token,
                static_cast<unsigned long>(p.owner),message,points,
                p.selection.alternativeToPrevious?"":" (only one feasible model set)");
        }
    }
    while(g_history.size()>8192) {
        auto oldest=std::min_element(g_history.begin(),g_history.end(),[](const auto& x,const auto& y){ return x.second.stamp<y.second.stamp; });
        g_history.erase(oldest);
    }
}
int __fastcall Validate(void* proc,void*,uint8_t* room,int slot) {
    if(g_attempt && !Prepare(*g_attempt,room)) return kStartError;
    return g_validate(proc,room,slot);
}
int __fastcall Build(void* proc,void*,uint8_t* room,uint8_t* info,uint8_t* pilot,uint8_t type,uint8_t mode) {
    if(g_attempt) {
        Attempt& a=*g_attempt;
        if(!Prepare(a,room)) return 0;
        if(a.enabled) {
            if(UsesDeck(mode)!=UsesDeck(a.signature.mode)) { Fail(a,"native mode/deck contract mismatch"); return 0; }
            if(!ApplyActual(a,room)) return 0;
            // A native random/special path may rewrite a later player's lobby
            // record after the first human was built. Enforce that human's
            // already prepared OWNED choice immediately before construction.
            auto player=std::find_if(a.players.begin(),a.players.end(),[&](const Player& p) {
                return room+kSlotBase+p.slot*kSlotStride==info;
            });
            if(player==a.players.end() || Get<uint32_t>(info,kOwner)!=player->owner
               || !Apply(info,player->selection,UsesDeck(mode))) {
                Fail(a,"native human source no longer matches the prepared owner"); return 0;
            }
        }
    }
    return g_build(proc,room,info,pilot,type,mode);
}
template<class Call> int RunAttempt(Call&& call) {
    Attempt attempt; attempt.token=++g_token;
    Attempt* parent=g_attempt; g_attempt=&attempt;
    try {
        const int result=call();
        if(result==0 && attempt.applied && !attempt.failed) Commit(attempt);
        else if(attempt.applied) RestoreActual(attempt.actual,attempt.token);
        g_attempt=parent;
        return attempt.failed ? kStartError : result;
    } catch(...) {
        if(attempt.applied) RestoreActual(attempt.actual,attempt.token);
        g_attempt=parent;
        throw;
    }
}
int __fastcall Dispatch(void* proc,void*,int session,void* request,uint32_t* key,uint32_t* index) {
    return RunAttempt([&] { return g_dispatch(proc,session,request,key,index); });
}
int __fastcall SpecialStart(void* proc,void*,void* source,uintptr_t extra) {
    // Separate 13/14 entry: ret 8, with two opaque 32-bit stack arguments.
    return RunAttempt([&] { return g_special(proc,source,extra); });
}
void __fastcall Release(uint8_t* room,void*) {
    g_release(room);
    RestoreActual(room);
}
template<class T> bool Bind(T& pointer,const char* symbol) {
    pointer=reinterpret_cast<T>(g_api->Symbol(g_mod,symbol)); return pointer!=nullptr;
}
bool LoadSettings() {
    sdgo::ModSettings settings(g_api,g_mod);
    char value[2048]{};
    if(settings.String("DatabaseIni","",value,sizeof(value))>=sizeof(value)-1) return false;
    if(value[0]) {
        const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,-1,nullptr,0);
        if(n<2) return false;
        std::wstring path(static_cast<size_t>(n),L'\0');
        MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,-1,path.data(),n); path.pop_back();
        const bool absolute=(path.size()>1 && path[1]==L':') || (path.size()>1 && path[0]==L'\\' && path[1]==L'\\');
        g_settings.databaseIni=absolute?path:std::wstring(g_api->ModDirectory(g_mod))+L"\\"+path;
    }
    auto number=[&](const char* name,const char* def,unsigned minimum,unsigned maximum,unsigned& out) {
        char text[32]{};
        if(settings.String(name,def,text,sizeof(text))>=sizeof(text)-1) return false;
        unsigned v=0;
        if(!text[0]) return false;
        for(const char* p=text;*p;++p) {
            if(*p<'0'||*p>'9'||v>maximum/10) return false;
            v=v*10+static_cast<unsigned>(*p-'0'); if(v>maximum) return false;
        }
        if(v<minimum) return false;
        out=v; return true;
    };
    if(!number("QueryTimeoutSeconds","2",1,10,g_settings.timeoutSeconds)
       || !number("MaxRowsPerPlayer","4096",1,16384,g_settings.maxRowsPerPlayer)) return false;
    if(settings.String("Driver","SQL Server",value,sizeof(value))>=sizeof(value)-1) return false;
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,-1,nullptr,0);
    if(n<2) return false;
    g_settings.driver.assign(static_cast<size_t>(n),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,-1,g_settings.driver.data(),n); g_settings.driver.pop_back();
    return true;
}
} // namespace

SDGO_MOD_ENTRY {
    g_api=api; g_mod=mod;
    if(!LoadSettings()) { api->Log(mod,SDGO_LOG_ERROR,"invalid room_random configuration"); return 0; }
    if(!Bind(g_lookup,"battle.unit_lookup") || !Bind(g_unitTable,"battle.unit_table")
       || !Bind(g_lobby,"battle.lobby_proc") || !Bind(g_lobbyFind,"battle.lobby_map_find")
       || !Bind(g_lobbyRoom,"battle.lobby_room") || !Bind(g_lobbyUnlock,"battle.lobby_release")) return 0;
    if(!api->HookCode(mod,"battle.start_dispatch",reinterpret_cast<void*>(&Dispatch),reinterpret_cast<void**>(&g_dispatch))
       || !api->HookCode(mod,"battle.start_special",reinterpret_cast<void*>(&SpecialStart),reinterpret_cast<void**>(&g_special))
       || !api->HookCode(mod,"battle.validate_unit",reinterpret_cast<void*>(&Validate),reinterpret_cast<void**>(&g_validate))
       || !api->HookCode(mod,"battle.build_human_unit",reinterpret_cast<void*>(&Build),reinterpret_cast<void**>(&g_build))
       || !api->HookCode(mod,"battle.release_pilots",reinterpret_cast<void*>(&Release),reinterpret_cast<void**>(&g_release))) return 0;
    api->Log(mod,SDGO_LOG_INFO,"ready: !rand / !random assigns owned units once per battle; decks total exactly 10 points");
    return 1;
}
