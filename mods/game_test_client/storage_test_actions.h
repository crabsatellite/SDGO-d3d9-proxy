#pragma once
#include <windows.h>
#include <cstring>
#include "sdgo/game_test.h"
#include "sdgo/game_test_page_observer.h"

// Main-thread consumer of the production page observer. All actions remain
// original storage Mod button events; only genuine replies advance its batch.
class StorageTestActions {
public:
    enum Result { Waiting=0, Ready, Armed, Running, Complete, Invalid=0x100,
                  Unavailable, IdentityMismatch, Changed, Rejected, UnexpectedMutation };
    struct Exports { SdgoStorageGameTestGetSnapshotV1Fn snapshot=nullptr;
                     SdgoStorageGameTestActionV1Fn action=nullptr; };
    bool Bind() {
        auto module=GetModuleHandleW(L"storage_claim.dll");
        return Bind({module?reinterpret_cast<SdgoStorageGameTestGetSnapshotV1Fn>(GetProcAddress(module,SDGO_STORAGE_GAME_TEST_SNAPSHOT_EXPORT)):nullptr,
                     module?reinterpret_cast<SdgoStorageGameTestActionV1Fn>(GetProcAddress(module,SDGO_STORAGE_GAME_TEST_ACTION_EXPORT)):nullptr});
    }
    bool Bind(Exports exports) { exports_=exports;stage_=0;state_={};before_={};return exports.snapshot && exports.action; }
    Result Read(const SdgoGameTestContextV1* context) {
        if(!context || context->size<sizeof(*context) || context->version!=SDGO_GAME_TEST_VERSION ||
           !context->role || strcmp(context->role,"client") || !context->run_id)return Invalid;
        const auto hash=sdgo::game_test_page_observer::ClientRunHash(context->run_id,strlen(context->run_id),"client",6);
        if(!hash)return Invalid;
        if(!exports_.snapshot || !exports_.action)return Unavailable;
        SdgoStorageGameTestSnapshotV1 next{};next.size=sizeof(next);next.version=SDGO_GAME_TEST_PAGE_OBSERVER_VERSION;
        if(!exports_.snapshot(&next))return Rejected;
        state_=next;
        if(next.size!=sizeof(next) || next.version!=SDGO_GAME_TEST_PAGE_OBSERVER_VERSION ||
           next.run_hash!=hash || next.process_id!=GetCurrentProcessId())return IdentityMismatch;
        if(!next.loaded || !next.visible || !next.session)return Waiting;
        if(next.ui_thread_id!=GetCurrentThreadId())return IdentityMismatch;
        if(next.current_tab>SDGO_STORAGE_GAME_TEST_TAB_GIFTS || next.page_entry_count>5 || next.page_entry_count>next.total)return Invalid;
        return Ready;
    }
    Result Claim(const SdgoGameTestContextV1* context) {
        const auto result=Read(context);
        if(result!=Ready)return result;
        if(stage_ && (state_.session!=before_.session || state_.generation!=before_.generation || state_.current_tab!=before_.current_tab))return Changed;
        if(stage_==0) {
            if(state_.busy || state_.outstanding || state_.armed)return UnexpectedMutation;
            before_=state_;
            if(!exports_.action(state_.run_hash,SDGO_STORAGE_GAME_TEST_ACTION_CLAIM_ALL))return Rejected;
            stage_=1;return Waiting;
        }
        if(stage_==1) {
            if(state_.action_sequence!=before_.action_sequence+1 || !state_.armed || state_.busy || state_.outstanding ||
               state_.reply_sequence!=before_.reply_sequence || state_.preflight_reply_sequence!=before_.preflight_reply_sequence)return UnexpectedMutation;
            if(!exports_.action(state_.run_hash,SDGO_STORAGE_GAME_TEST_ACTION_CLAIM_ALL))return Rejected;
            stage_=2;return Armed;
        }
        if(state_.action_sequence!=before_.action_sequence+2)return UnexpectedMutation;
        if(state_.batch_phase==SDGO_STORAGE_GAME_TEST_PHASE_STOPPED || state_.batch_phase==SDGO_STORAGE_GAME_TEST_PHASE_UNKNOWN)return Rejected;
        if(state_.batch_phase!=SDGO_STORAGE_GAME_TEST_PHASE_COMPLETE || state_.busy || state_.outstanding)return Running;
        if(state_.armed || state_.reply_sequence<=before_.reply_sequence ||
           (state_.completed && state_.preflight_reply_sequence<=before_.preflight_reply_sequence))return Rejected;
        stage_=3;return Complete;
    }
    SdgoStorageGameTestSnapshotV1 State()const{return state_;}
private:
    Exports exports_{};
    SdgoStorageGameTestSnapshotV1 state_{},before_{};
    unsigned stage_=0;
};
