#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace sdgo::hangar {
struct Rule {
    uint32_t unit = 0;
    int rank = 0, subrank = 0, gb = 0, item = 0, amount = 0;
    std::wstring name;
    std::wstring rewardName;
};
struct Unit {
    uint64_t uid = 0;
    uint32_t type = 0;
    uint16_t slot = 0;
    uint8_t level = 0, rental = 0, ship = 0, lock = 0, special = 0;
    bool fullReady = false;
    std::array<uint8_t,153> full{};
};
struct Snapshot {
    uint64_t session = 0;
    uint16_t capacity = 0, activeSlot = 0;
    std::vector<Unit> units;
};
enum class Kind { Swap, GB, Item };
struct Action {
    Kind kind = Kind::Swap;
    uint64_t first = 0, second = 0;
    uint32_t type = 0;
    uint16_t from = 0, to = 0;
    std::array<uint8_t,153> signature{};
};
struct Plan {
    Snapshot before;
    std::vector<Action> actions;
    size_t removed = 0, pinned = 0, variants = 0, detailsMissing = 0;
    int64_t expectedGB = 0;
    std::map<int,int64_t> expectedItems;
};
using Rules = std::map<uint32_t,Rule>;

inline bool Protected(const Unit& u, uint16_t active) {
    return u.slot == active || u.lock || u.ship || u.rental || u.special;
}
inline void Validate(const Snapshot& s) {
    if (!s.session || !s.capacity) throw std::runtime_error("no live hangar");
    std::set<uint64_t> ids;
    std::set<uint16_t> slots;
    for (const auto& u:s.units) {
        if (!u.uid || !u.type || !u.slot || u.slot>s.capacity || !ids.insert(u.uid).second || !slots.insert(u.slot).second)
            throw std::runtime_error("incomplete or inconsistent hangar snapshot");
    }
}
inline const Unit* ById(const Snapshot& s,uint64_t uid) {
    for (const auto& u:s.units) if (u.uid==uid) return &u;
    return nullptr;
}
inline std::array<uint8_t,153> Signature(const Unit& u) {
    auto bytes=u.full;
    // Full native record: +4 slot, +8 unique inventory ID. All other bytes
    // remain significant: paint, skills, experience and custom state survive.
    bytes[4]=bytes[5]=0;
    std::fill(bytes.begin()+8,bytes.begin()+16,uint8_t{0});
    return bytes;
}
inline bool SameLayout(const Snapshot& a,const Snapshot& b) {
    if (a.session!=b.session || a.capacity!=b.capacity || a.units.size()!=b.units.size()) return false;
    for (const auto& u:a.units) {
        const auto* v=ById(b,u.uid);
        if (!v || u.type!=v->type || u.slot!=v->slot) return false;
    }
    return true;
}
inline Snapshot Applied(Snapshot s,const Action& a) {
    if (a.kind!=Kind::Swap) {
        s.units.erase(std::remove_if(s.units.begin(),s.units.end(),[&](const Unit& u){return u.uid==a.first;}),s.units.end());
    } else {
        for (auto& u:s.units) {
            if (u.uid==a.first) u.slot=a.to;
            else if (a.second && u.uid==a.second) u.slot=a.from;
        }
    }
    return s;
}
inline bool Allowed(const Snapshot& s,const Action& a) {
    const auto* first=ById(s,a.first);
    if (!first || first->slot!=a.from || first->type!=a.type || Protected(*first,s.activeSlot)) return false;
    if (a.kind!=Kind::Swap)
        return first->level<=1 && first->fullReady && Signature(*first)==a.signature;
    if (!a.to || a.to>s.capacity || a.to==s.activeSlot) return false;
    const auto* second=ById(s,a.second);
    if (a.second && (!second || second->slot!=a.to || Protected(*second,s.activeSlot))) return false;
    if (!a.second) for (const auto& u:s.units) if (u.slot==a.to) return false;
    return true;
}
inline Plan Sort(const Snapshot& s,const Rules& rules) {
    Validate(s);
    Plan p;p.before=s;
    std::set<uint16_t> pinned;
    std::vector<Unit> movable;
    for (const auto& u:s.units) {
        if (Protected(u,s.activeSlot) || !rules.count(u.type)) {pinned.insert(u.slot);++p.pinned;}
        else movable.push_back(u);
    }
    std::sort(movable.begin(),movable.end(),[&](const Unit& a,const Unit& b){
        const auto& x=rules.at(a.type);const auto& y=rules.at(b.type);
        return std::make_tuple(x.rank,x.subrank,a.type,a.level,a.uid)>
               std::make_tuple(y.rank,y.subrank,b.type,b.level,b.uid);
    });
    Snapshot predicted=s;
    uint16_t destination=1;
    for (const auto& wanted:movable) {
        while (pinned.count(destination)) ++destination;
        const auto* current=ById(predicted,wanted.uid);
        if (current->slot!=destination) {
            uint64_t other=0;
            for (const auto& u:predicted.units) if (u.slot==destination) other=u.uid;
            Action a;a.first=wanted.uid;a.second=other;a.type=wanted.type;a.from=current->slot;a.to=destination;
            if (!Allowed(predicted,a)) throw std::runtime_error("unsafe slot plan");
            p.actions.push_back(a);predicted=Applied(predicted,a);
        }
        ++destination;
    }
    return p;
}
inline Plan Duplicates(const Snapshot& s,const Rules& rules,int keep,Kind reward,bool sortAfter) {
    Validate(s);
    if (keep<1 || keep>20 || reward==Kind::Swap) throw std::runtime_error("invalid duplicate policy");
    Plan p;p.before=s;
    std::map<uint32_t,std::vector<Unit>> types;
    for (const auto& u:s.units) types[u.type].push_back(u);
    for (auto& entry:types) {
        const auto r=rules.find(entry.first);
        if (r==rules.end() || r->second.subrank==1) continue;
        std::map<std::array<uint8_t,153>,std::vector<Unit>> variants;
        for (const auto& u:entry.second) {
            if (Protected(u,s.activeSlot) || u.level>1) {++p.pinned;continue;}
            if (!u.fullReady) {++p.detailsMissing;continue;}
            variants[Signature(u)].push_back(u);
        }
        p.variants+=variants.size();
        for (auto& group:variants) {
            auto& copies=group.second;
            std::sort(copies.begin(),copies.end(),[](const Unit& a,const Unit& b){return a.uid>b.uid;});
            for (size_t i=static_cast<size_t>(keep);i<copies.size();++i) {
                const auto& u=copies[i];
                if ((reward==Kind::GB && r->second.gb<=0) || (reward==Kind::Item && (r->second.item<=0 || r->second.amount<=0))) continue;
                Action a;a.kind=reward;a.first=u.uid;a.type=u.type;a.from=u.slot;a.signature=group.first;
                p.actions.push_back(a);++p.removed;
                if (reward==Kind::GB) p.expectedGB+=r->second.gb;
                else p.expectedItems[r->second.item]+=r->second.amount;
            }
        }
    }
    if (sortAfter) {
        Snapshot remaining=s;
        for (const auto& a:p.actions) remaining=Applied(remaining,a);
        auto reorder=Sort(remaining,rules);
        p.actions.insert(p.actions.end(),reorder.actions.begin(),reorder.actions.end());
    }
    return p;
}

// Exact wire layouts copied from the verified native send sites, not a new
// server protocol. All writes go through the authenticated game session.
#pragma pack(push,1)
struct SwapPacket {uint16_t op=0x800;uint32_t reserved=0;uint64_t first=0;uint16_t destination=0;uint64_t second=0;uint16_t secondDestination=0;};
struct DeletePacket {uint16_t op=0x6D2;uint32_t reserved=0;uint64_t uid=0;uint32_t type=0;uint8_t reward=0;};
struct DetailPacket {uint16_t op=0x78B;uint32_t reserved=0;uint64_t uid=0;};
#pragma pack(pop)
static_assert(sizeof(SwapPacket)==26 && sizeof(DeletePacket)==19 && sizeof(DetailPacket)==14);
inline std::vector<uint8_t> Packet(const Action& a) {
    if (a.kind==Kind::Swap) {
        SwapPacket p;p.first=a.first;p.destination=a.to;p.second=a.second;p.secondDestination=a.from;
        const auto* b=reinterpret_cast<const uint8_t*>(&p);return {b,b+sizeof(p)};
    }
    DeletePacket p;p.uid=a.first;p.type=a.type;p.reward=a.kind==Kind::GB?1:2;
    const auto* b=reinterpret_cast<const uint8_t*>(&p);return {b,b+sizeof(p)};
}

enum class State { Idle, Ready, Waiting, Complete, Stopped, Unknown };
class Runner {
    Plan plan_;
    Snapshot confirmed_;
    size_t index_=0;
    uint64_t sentAt_=0;
    bool cancel_=false;
    State state_=State::Idle;
public:
    void Start(Plan p,const Snapshot& current) {
        if (!SameLayout(p.before,current)) throw std::runtime_error("preview is stale");
        plan_=std::move(p);confirmed_=current;index_=0;cancel_=false;
        state_=plan_.actions.empty()?State::Complete:State::Ready;
    }
    State state() const {return state_;}
    size_t completed() const {return index_;}
    size_t total() const {return plan_.actions.size();}
    void Cancel() {cancel_=true;if(state_==State::Ready)state_=State::Stopped;}
    void Disconnect() {state_=state_==State::Waiting?State::Unknown:State::Stopped;}
    const Action* Next(const Snapshot& s) {
        if(state_!=State::Ready)return nullptr;
        if(!SameLayout(s,confirmed_) || !Allowed(s,plan_.actions[index_])) {state_=State::Stopped;return nullptr;}
        return &plan_.actions[index_];
    }
    void Sent(uint64_t now,bool accepted) {sentAt_=now;state_=accepted?State::Waiting:State::Unknown;}
    void Observe(const Snapshot& s,uint64_t now,uint64_t timeout) {
        if(state_!=State::Waiting)return;
        if(s.session!=confirmed_.session) {state_=State::Unknown;return;}
        if(SameLayout(s,Applied(confirmed_,plan_.actions[index_]))) {
            confirmed_=s;++index_;
            state_=index_==plan_.actions.size()?State::Complete:(cancel_?State::Stopped:State::Ready);
        } else if(!SameLayout(s,confirmed_) || now-sentAt_>=timeout)state_=State::Unknown;
    }
};
}
