#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace sdgo::storage {
enum class Tab { Items=0, Gifts=1 };
struct Key {
    uint64_t id=0,stamp=0;
    bool operator==(const Key& v)const{return id==v.id && stamp==v.stamp;}
    bool operator<(const Key& v)const{return std::tie(id,stamp)<std::tie(v.id,v.stamp);}
};
struct Entry {Key key;bool claimed=false;};
enum class Phase { Idle, NeedList, List, NeedCheck, Check, NeedClaim, Claim, Complete, Stopped, Unknown };
enum class Reason { None, Cancelled, Rejected, Selection, Changed, Timeout, SendFailed, Limit, Disconnected, PageLimit };
enum class Operation { List, Check, Claim };
struct Request {Operation operation;Tab tab;uint32_t page;Key key;};
class Batch {
public:
    bool Start(Tab tab,uint64_t session,uint32_t total,uint32_t limit,uint64_t now) {
        if(Busy() || outstanding_ || !session || !limit)return false;
        *this=Batch{};tab_=tab;session_=session;limit_=limit;
        // Gift list page numbers are native one-based bytes, never wrap at 256.
        lastPage_=total?std::min<uint32_t>((total-1)/5,254):0;
        phase_=Phase::NeedList;since_=now;return true;
    }
    bool Busy()const{return phase_>=Phase::NeedList && phase_<=Phase::Claim;}
    bool Outstanding()const{return outstanding_;}
    Phase phase()const{return phase_;}
    Reason reason()const{return reason_;}
    Tab tab()const{return tab_;}
    uint64_t session()const{return session_;}
    uint32_t completed()const{return completed_;}
    uint32_t total()const{return total_;}
    uint32_t page()const{return page_;}
    Key pending()const{return target_;}
    void Stop(Reason reason) {
        cancel_=true;reason_=reason;
        if(outstanding_ && pending_==Operation::Claim){phase_=Phase::Claim;return;}
        phase_=Phase::Stopped;
    }
    void Disconnect(){reason_=Reason::Disconnected;phase_=outstanding_ && pending_==Operation::Claim?Phase::Unknown:Phase::Stopped;cancel_=true;}
    void Tick(uint64_t now,uint64_t timeout) {
        if(outstanding_ && now>=since_ && now-since_>=timeout && phase_!=Phase::Unknown) {
            reason_=Reason::Timeout;cancel_=true;
            phase_=pending_==Operation::Claim?Phase::Unknown:Phase::Stopped;
        }
    }
    std::optional<Request> Next()const {
        if(outstanding_)return {};
        if(phase_==Phase::NeedList)return Request{Operation::List,tab_,page_,{}};
        if(phase_==Phase::NeedCheck)return Request{Operation::Check,tab_,page_,target_};
        if(phase_==Phase::NeedClaim)return Request{Operation::Claim,tab_,page_,target_};
        return {};
    }
    void Sent(const Request& request,uint64_t now,bool success) {
        if(!Next() || request.operation!=Next()->operation || request.tab!=tab_ || request.page!=page_ || !(request.key==Next()->key))throw std::runtime_error("stale storage request");
        pending_=request.operation;since_=now;
        // A failed native send may still have queued a mutation. Never retry it.
        if(!success){cancel_=true;reason_=Reason::SendFailed;phase_=pending_==Operation::Claim?Phase::Unknown:Phase::Stopped;outstanding_=pending_==Operation::Claim;return;}
        outstanding_=true;phase_=pending_==Operation::List?Phase::List:pending_==Operation::Check?Phase::Check:Phase::Claim;
    }
    bool Waiting(Operation op)const{return outstanding_ && pending_==op;}
    bool ListReply(Tab tab,uint32_t total,const std::vector<Entry>& entries) {
        if(!Waiting(Operation::List) || tab!=tab_)return false;
        outstanding_=false;if(cancel_)return true;
        if(entries.size()>5 || total>1000000){Fail(Reason::Changed);return true;}
        total_=total;if(!scopeSet_){lastPage_=total?std::min<uint32_t>((total-1)/5,254):0;scopeSet_=true;}
        std::set<Key> ids;
        for(const auto& entry:entries) {
            if(!entry.key.id || !ids.insert(entry.key).second){Fail(Reason::Changed);return true;}
            if(entry.claimed)continue;
            if(done_.count(entry.key)){Fail(Reason::Changed);return true;}
        }
        for(const auto& entry:entries)if(!entry.claimed) {
            if(completed_>=limit_){Fail(Reason::Limit);return true;}
            target_=entry.key;phase_=Phase::NeedCheck;return true;
        }
        if(tab_==Tab::Gifts && page_<lastPage_ && (page_+1)*5<total_){++page_;phase_=Phase::NeedList;return true;}
        if(tab_==Tab::Gifts && page_==254 && (page_+1)*5<total_){Fail(Reason::PageLimit);return true;}
        if(tab_==Tab::Items && total_ && entries.empty()){Fail(Reason::Changed);return true;}
        phase_=Phase::Complete;return true;
    }
    bool CheckReply(Key key,bool allowed,bool choice=false) {
        if(!Waiting(Operation::Check))return false;
        if(!(key==target_)){Fail(Reason::Changed);return true;}
        outstanding_=false;
        if(cancel_)return true;
        if(!allowed){Fail(Reason::Rejected);return true;}
        if(choice){Fail(Reason::Selection);return true;}
        phase_=Phase::NeedClaim;return true;
    }
    bool ClaimReply(Key key,bool success) {
        if(!Waiting(Operation::Claim))return false;
        if(!(key==target_)){cancel_=true;reason_=Reason::Changed;phase_=Phase::Unknown;return true;}
        outstanding_=false;
        if(!success){Fail(Reason::Rejected);return true;}
        if(!done_.insert(key).second){Fail(Reason::Changed);return true;}
        ++completed_;
        if(cancel_){phase_=Phase::Stopped;return true;}
        phase_=Phase::NeedList;return true;
    }
    void Reject(Reason reason=Reason::Rejected) {outstanding_=false;Fail(reason);}
private:
    void Fail(Reason reason){phase_=Phase::Stopped;reason_=reason;cancel_=true;}
    Phase phase_=Phase::Idle;Reason reason_=Reason::None;
    Tab tab_=Tab::Items;Operation pending_=Operation::List;
    uint64_t session_=0,since_=0;uint32_t page_=0,lastPage_=0,completed_=0,total_=0,limit_=5000;
    Key target_;std::set<Key> done_;bool outstanding_=false,cancel_=false,scopeSet_=false;
};

// Host wire formats: original authenticated requests; no account identity field.
inline std::vector<uint8_t> Packet(const Request& r) {
    const uint16_t op=r.operation==Operation::List?(r.tab==Tab::Items?0x9A1:0x9A2):
        r.operation==Operation::Check?(r.tab==Tab::Items?0x6CB:0x906):(r.tab==Tab::Items?0x9CC:0x630);
    std::vector<uint8_t> bytes(r.operation==Operation::List?10:r.tab==Tab::Items?14:18,0);
    auto put=[&](size_t offset,uint64_t value,size_t n){for(size_t i=0;i<n;++i)bytes[offset+i]=static_cast<uint8_t>(value>>(8*i));};
    put(0,op,2);
    if(r.operation==Operation::List){if(r.page>254)throw std::runtime_error("native storage page overflow");put(6,r.page+1,4);}
    else if(r.tab==Tab::Items)put(6,r.key.id,8);
    else {if(r.key.id>UINT32_MAX)throw std::runtime_error("gift note ID overflow");put(6,r.key.id,4);put(10,r.key.stamp,8);}
    return bytes;
}
}
