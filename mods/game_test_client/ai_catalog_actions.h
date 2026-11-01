#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include "sdgo/sdgo_api.h"
#include "sdgo/ai_catalog_format.h"

// Validate every owner-provided record against the real native grid, without
// creating an NPC or inserting a record on behalf of the production Mod.
class AiCatalogActions {
public:
    using GridData=void*(__cdecl*)();
    using GridFind=uint8_t(__thiscall*)(void*,const uint8_t**,uint32_t,int,char);
    struct Observation {uint32_t configured=0,verified=0,npc_id=0,unit_id=0,levels=0,error=0;};
    bool Bind(const SdgoApi* api,SdgoMod* mod) {
        data_=reinterpret_cast<GridData>(api->Symbol(mod,"grid.data"));
        find_=reinterpret_cast<GridFind>(api->Symbol(mod,"grid.find"));return data_ && find_;
    }
    bool Check(const std::wstring& file) {
        state_={};sdgo::catalog::Catalog expected;std::string error;
        if (!data_ || !find_){state_.error=1;return false;}
        if (!expected.Load(file,error)){state_.error=2;return false;}
        state_.configured=static_cast<uint32_t>(expected.records.size());
        for (const auto& row:expected.records) {
            state_.npc_id=row.npcId;state_.unit_id=row.unitId;
            std::array<uint8_t,sdgo::catalog::kClientNpcBytes> npc{};
            std::array<uint8_t,sdgo::catalog::kClientUnitBytes> unit{};
            if (!Read(row.npcId,41,npc.data(),npc.size()) || !Read(row.unitId,2,unit.data(),unit.size())) {
                state_.error=3;return false;
            }
            if (npc!=row.clientNpc || sdgo::catalog::Hash64(unit.data(),unit.size())!=row.clientUnitHash) {
                state_.error=4;return false;
            }
            ++state_.verified;state_.levels|=1u<<(row.level-1);
        }
        return state_.configured && state_.configured==state_.verified;
    }
    Observation State()const{return state_;}
private:
    bool Read(uint32_t id,int type,uint8_t* output,size_t size)const {
        __try {
            const uint8_t* row=nullptr;
            if (!find_(data_(),&row,id,type,0) || !row)return false;
            memcpy(output,row,size);return true;
        }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    GridData data_=nullptr;GridFind find_=nullptr;Observation state_{};
};
