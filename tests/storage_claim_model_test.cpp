#include "sdgo/storage_claim_model.h"
#include <cassert>
#include <iostream>
using namespace sdgo::storage;
namespace {
void Sent(Batch& b,uint64_t now=1){const auto r=b.Next();assert(r);b.Sent(*r,now,true);}
void Claim(Batch& b,Key key){Sent(b);assert(b.CheckReply(key,true));Sent(b);assert(b.ClaimReply(key,true));}
}
int main() {
    Batch b;assert(b.Start(Tab::Items,7,7,5000,0));Sent(b);
    const Key large{0xFEDCBA9812345678ULL,0};assert(b.ListReply(Tab::Items,7,{{large,false},{{2,0},false}}));
    Claim(b,large);Sent(b);b.ListReply(Tab::Items,6,{{{2,0},false}});Claim(b,{2,0});
    Sent(b);b.ListReply(Tab::Items,0,{});assert(b.phase()==Phase::Complete && b.completed()==2);
    // Every next list is page zero as removal shifts the following pages forward.
    assert(b.page()==0 && !b.Next());
    Batch gifts;assert(gifts.Start(Tab::Gifts,7,0,5000,0));Sent(gifts);
    const Key gift{0xF0000001,0x8765432112345678ULL};gifts.ListReply(Tab::Gifts,8,{{gift,false}});Claim(gifts,gift);
    Sent(gifts);gifts.ListReply(Tab::Gifts,8,{{gift,true}});assert(gifts.page()==1);Sent(gifts);
    gifts.ListReply(Tab::Gifts,8,{{{22,2},true}});assert(gifts.phase()==Phase::Complete && gifts.completed()==1);
    Batch cancel;cancel.Start(Tab::Items,7,1,10,0);Sent(cancel);cancel.ListReply(Tab::Items,1,{{{4,0},false}});
    Sent(cancel);cancel.Stop(Reason::Cancelled);assert(!cancel.Next() && cancel.Outstanding());
    assert(!cancel.Start(Tab::Items,7,1,10,3));cancel.CheckReply({4,0},true);assert(!cancel.Outstanding() && !cancel.Next());
    Batch inflight;inflight.Start(Tab::Items,7,1,10,0);Sent(inflight);inflight.ListReply(Tab::Items,1,{{{4,0},false}});
    Sent(inflight);inflight.CheckReply({4,0},true);Sent(inflight);inflight.Stop(Reason::Cancelled);
    assert(inflight.Busy());inflight.ClaimReply({4,0},true);assert(inflight.completed()==1 && inflight.phase()==Phase::Stopped && !inflight.Next());
    Batch timeout;timeout.Start(Tab::Items,7,1,10,0);Sent(timeout);timeout.ListReply(Tab::Items,1,{{{4,0},false}});
    Sent(timeout);timeout.CheckReply({4,0},true);Sent(timeout);timeout.Tick(20000,10000);
    assert(timeout.phase()==Phase::Unknown && !timeout.Next());assert(!timeout.Start(Tab::Items,7,1,10,20001));
    timeout.ClaimReply({4,0},true);assert(timeout.completed()==1 && timeout.phase()==Phase::Stopped);
    Batch selection;selection.Start(Tab::Gifts,7,1,10,0);Sent(selection);selection.ListReply(Tab::Gifts,1,{{gift,false}});
    Sent(selection);selection.CheckReply(gift,true,true);assert(selection.reason()==Reason::Selection && !selection.Next());
    Batch duplicate;duplicate.Start(Tab::Items,7,1,10,0);Sent(duplicate);duplicate.ListReply(Tab::Items,1,{{{4,0},false}});Claim(duplicate,{4,0});
    Sent(duplicate);duplicate.ListReply(Tab::Items,1,{{{4,0},false}});assert(duplicate.reason()==Reason::Changed && !duplicate.Next());
    Batch mismatch;mismatch.Start(Tab::Items,7,1,10,0);Sent(mismatch);mismatch.ListReply(Tab::Items,1,{{{4,0},false}});
    Sent(mismatch);mismatch.CheckReply({4,0},true);Sent(mismatch);mismatch.ClaimReply({5,0},true);
    assert(mismatch.phase()==Phase::Unknown && mismatch.Outstanding() && mismatch.completed()==0);
    mismatch.ClaimReply({4,0},true);assert(mismatch.completed()==1 && !mismatch.Outstanding());
    Batch empty;empty.Start(Tab::Items,7,0,10,0);Sent(empty);empty.ListReply(Tab::Items,0,{});assert(empty.phase()==Phase::Complete);
    Batch pages;pages.Start(Tab::Gifts,7,2000,5000,0);
    for(uint32_t i=0;i<255;++i){assert(pages.Next()->page==i);Sent(pages);pages.ListReply(Tab::Gifts,2000,{{{i+1,1},true}});}
    assert(pages.reason()==Reason::PageLimit && !pages.Next());
    const auto wire=Packet({Operation::Claim,Tab::Items,0,large});assert(wire.size()==14 && wire[0]==0xCC && wire[1]==9 && wire[6]==0x78 && wire[13]==0xFE);
    const auto giftWire=Packet({Operation::Claim,Tab::Gifts,0,gift});assert(giftWire.size()==18 && giftWire[0]==0x30 && giftWire[1]==6 && giftWire[9]==0xF0 && giftWire[17]==0x87);
    assert(Packet({Operation::List,Tab::Items,254,{}})[6]==255);
    bool rejected=false;try{Packet({Operation::List,Tab::Gifts,255,{}});}catch(const std::exception&){rejected=true;}assert(rejected);
    std::cout<<"STORAGE CLAIM MODEL PASSED: stable 64-bit IDs, pagination, one outstanding request, fresh lists, cancellation, selection, timeout/late replies, no retries and native wire layouts\n";
}
