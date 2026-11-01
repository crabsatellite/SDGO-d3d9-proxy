// Offline asynchronous consumer contract. This never claims game acceptance.
#include "../mods/game_test_client/storage_test_actions.h"
#include <cstdio>
namespace {
SdgoStorageGameTestSnapshotV1 snapshot{};
unsigned calls=0,failures=0;
bool allow=true;
int __cdecl Get(SdgoStorageGameTestSnapshotV1* output){*output=snapshot;return 1;}
int __cdecl Action(uint64_t hash,uint32_t action){
    if(!allow || hash!=snapshot.run_hash || action!=SDGO_STORAGE_GAME_TEST_ACTION_CLAIM_ALL)return 0;
    ++calls;++snapshot.action_sequence;
    if(!snapshot.armed)snapshot.armed=1;
    else {snapshot.armed=0;snapshot.busy=1;snapshot.outstanding=1;snapshot.batch_phase=SDGO_STORAGE_GAME_TEST_PHASE_LIST;}
    return 1;
}
void Check(bool value,const char* message){if(!value){++failures;std::fprintf(stderr,"FAIL %s\n",message);}}
SdgoGameTestContextV1 Context(){
    SdgoGameTestContextV1 c{};c.size=sizeof(c);c.version=SDGO_GAME_TEST_VERSION;
    c.run_id="12345678-1234-1234-1234-123456789abc";c.role="client";return c;
}
void Reset(){
    snapshot={};snapshot.size=sizeof(snapshot);snapshot.version=SDGO_GAME_TEST_PAGE_OBSERVER_VERSION;
    const auto c=Context();snapshot.run_hash=sdgo::game_test_page_observer::ClientRunHash(c.run_id,36,c.role,6);
    snapshot.session=11;snapshot.process_id=GetCurrentProcessId();snapshot.ui_thread_id=GetCurrentThreadId();
    snapshot.loaded=1;snapshot.visible=1;calls=0;allow=true;
}
StorageTestActions Driver(){StorageTestActions d;d.Bind({&Get,&Action});return d;}
void Start(StorageTestActions& d,const SdgoGameTestContextV1& c){
    Check(d.Claim(&c)==StorageTestActions::Waiting && calls==1,"first original button only arms");
    Check(d.Claim(&c)==StorageTestActions::Armed && calls==2,"second original button starts real batch");
}
}
int main(){
    const auto c=Context();Reset();auto d=Driver();
    Check(d.Read(&c)==StorageTestActions::Ready,"same-run native page is ready");
    snapshot.visible=0;Check(d.Read(&c)==StorageTestActions::Waiting,"invisible page stays pending");snapshot.visible=1;
    ++snapshot.run_hash;Check(d.Read(&c)==StorageTestActions::IdentityMismatch,"foreign run rejected");--snapshot.run_hash;
    ++snapshot.ui_thread_id;Check(d.Read(&c)==StorageTestActions::IdentityMismatch,"foreign UI thread rejected");--snapshot.ui_thread_id;
    Start(d,c);Check(d.Claim(&c)==StorageTestActions::Running && calls==2,"await replies without extra actions");
    snapshot.busy=0;snapshot.outstanding=0;snapshot.batch_phase=SDGO_STORAGE_GAME_TEST_PHASE_COMPLETE;
    Check(d.Claim(&c)==StorageTestActions::Rejected,"completion without a genuine list reply rejected");
    ++snapshot.reply_sequence;
    Check(d.Claim(&c)==StorageTestActions::Complete,"empty native list can complete without a preflight");
    Reset();d=Driver();Start(d,c);snapshot.busy=0;snapshot.outstanding=0;snapshot.batch_phase=SDGO_STORAGE_GAME_TEST_PHASE_COMPLETE;
    snapshot.completed=1;++snapshot.reply_sequence;
    Check(d.Claim(&c)==StorageTestActions::Rejected,"positive claim requires preflight reply");
    ++snapshot.preflight_reply_sequence;Check(d.Claim(&c)==StorageTestActions::Complete,"positive claim retains preflight and settlement evidence");
    Reset();d=Driver();Start(d,c);++snapshot.session;
    Check(d.Claim(&c)==StorageTestActions::Changed && calls==2,"session replacement stops action scheduling");
    Reset();d=Driver();d.Claim(&c);++snapshot.reply_sequence;
    Check(d.Claim(&c)==StorageTestActions::UnexpectedMutation && calls==1,"pre-confirmation mutation fails closed");
    Reset();d=Driver();allow=false;Check(d.Claim(&c)==StorageTestActions::Rejected && !calls,"rejected original action cannot advance");
    Reset();d=Driver();Start(d,c);snapshot.batch_phase=SDGO_STORAGE_GAME_TEST_PHASE_UNKNOWN;
    Check(d.Claim(&c)==StorageTestActions::Rejected,"unknown result is never retried or accepted");
    std::printf("storage native observer consumer: %u failures\n",failures);return failures?1:0;
}
