#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include "sdgo/sdgo_api.h"
#include "sdgo/game_test_capsule_observer.h"
class CapsuleCatalogActions {
public:
    struct Observation { uint32_t expected=0,verified=0,tables=0,error=0,collection_machine=0,collection_price=0; };
    using GridData=void*(__cdecl*)();
    using GridFind=uint8_t(__thiscall*)(void*,const uint8_t**,uint32_t,int,char);
    bool Bind(const SdgoApi* api,SdgoMod* mod) {
        data_=reinterpret_cast<GridData>(api->Symbol(mod,"grid.data"));
        find_=reinterpret_cast<GridFind>(api->Symbol(mod,"grid.find"));
        const auto module=GetModuleHandleW(L"capsule_currency_fix.dll");
        observe_=module?reinterpret_cast<SdgoCapsuleGameTestGetSnapshotV1Fn>(GetProcAddress(module,SDGO_CAPSULE_GAME_TEST_SNAPSHOT_EXPORT)):nullptr;
        return data_ && find_ && observe_;
    }
    bool Check(const std::wstring& runtime) {
        state_={};
        if(!data_ || !find_){state_.error=1;return false;}
        struct Table {const wchar_t* name;uint32_t first;int type;};
        const Table tables[]={{L"fixed",155001,0x17},{L"coin",135001,0x16},{L"custom",3519001,0x41},
                              {L"candidates",3520001,0x42},{L"activity",145001,0x3C}};
        for(const auto& table:tables) {
            const auto count=GetPrivateProfileIntW(L"counts",table.name,0,runtime.c_str());
            if(!count || count>8192){state_.error=2;return false;}
            state_.expected+=count;
            for(uint32_t i=0;i<count;++i) {
                const uint8_t* record=nullptr;
                if(!Find(table.first+i,table.type,&record) || Value(record,0)!=table.first+i){state_.error=3;return false;}
                ++state_.verified;
                if(table.type==0x3C && Value(record,5)==990001) {
                    state_.collection_machine=990001;state_.collection_price=Value(record,15);
                }
            }
            const uint8_t* extra=nullptr;
            if(Find(table.first+count,table.type,&extra)){state_.error=4;return false;}
            ++state_.tables;
        }
        return state_.verified==state_.expected && state_.tables==5;
    }
    bool Currency(SdgoCapsuleGameTestSnapshotV1& state)const {
        state={};state.size=sizeof(state);state.version=1;return observe_ && observe_(&state)!=0;
    }
    Observation State()const{return state_;}
private:
    bool Find(uint32_t id,int type,const uint8_t** row)const {
        __try{return data_ && find_ && find_(data_(),row,id,type,0) && *row;}
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    static uint32_t Value(const uint8_t* row,size_t offset) {
        __try {uint32_t n=0;memcpy(&n,row+offset,4);return n;}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    }
    GridData data_=nullptr;GridFind find_=nullptr;
    SdgoCapsuleGameTestGetSnapshotV1Fn observe_=nullptr;
    Observation state_{};
};
