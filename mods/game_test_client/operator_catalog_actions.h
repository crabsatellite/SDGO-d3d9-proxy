#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/operator_model.h"

// Read-only acceptance of the configured catalog in the real loaded native
// grid. In particular, a successfully loaded DLL does not prove that its
// category template survived the original loader's security filter.
class OperatorCatalogActions {
public:
    struct Observation {uint32_t configured=0,verified=0,operator_id=0,category_id=0,error=0;};
    using GridData=void*(__cdecl*)();
    using GridFind=uint8_t(__thiscall*)(void*,const uint8_t**,uint32_t,int,char);
    bool Bind(const SdgoApi* api,SdgoMod* mod) {
        data_=reinterpret_cast<GridData>(api->Symbol(mod,"grid.data"));
        find_=reinterpret_cast<GridFind>(api->Symbol(mod,"grid.find"));
        return data_ && find_;
    }
    bool Check(const std::wstring& file) {
        state_={};
        if (!data_ || !find_)return Fail(1);
        if (GetPrivateProfileIntW(L"catalog",L"Version",0,file.c_str())!=1)return Fail(2);
        std::vector<wchar_t> sections(65536);
        if (GetPrivateProfileSectionNamesW(sections.data(),static_cast<DWORD>(sections.size()),file.c_str())>=sections.size()-2)
            return Fail(3);
        for (const wchar_t* section=sections.data();*section;section+=wcslen(section)+1) {
            if (wcsncmp(section,L"operator:",9))continue;
            wchar_t* end=nullptr;
            const auto id=wcstoul(section+9,&end,10);
            const auto category=GetPrivateProfileIntW(section,L"Category",0,file.c_str());
            if (!end || *end || id<60001 || id>60999 || category<1200000 || category>=1300000 || state_.configured>=256)
                return Fail(4);
            ++state_.configured;state_.operator_id=id;state_.category_id=category;
            wchar_t hex[200]{};
            const auto chars=GetPrivateProfileStringW(section,L"NameHex",L"",hex,200,file.c_str());
            std::wstring expected;
            if (chars) {
                if (chars%4 || chars>192)return Fail(5);
                for (DWORD n=0;n<chars;n+=4) {
                    uint32_t bytes[4]{};
                    for (unsigned digit=0;digit<4;++digit) {
                        const wchar_t c=hex[n+digit];
                        if (c>=L'0' && c<=L'9')bytes[digit]=c-L'0';
                        else if (c>=L'a' && c<=L'f')bytes[digit]=c-L'a'+10;
                        else if (c>=L'A' && c<=L'F')bytes[digit]=c-L'A'+10;
                        else return Fail(5);
                    }
                    const auto c=static_cast<wchar_t>((bytes[0]*16+bytes[1])|((bytes[2]*16+bytes[3])<<8));
                    if (c<32)return Fail(5);
                    expected.push_back(c);
                }
            } else {
                wchar_t name[100]{};
                GetPrivateProfileStringW(section,L"Name",L"",name,100,file.c_str());expected=name;
            }
            if (expected.empty() || expected.size()>48)return Fail(5);
            std::array<uint8_t,sdgo::operators::kOperatorSize> op{};
            std::array<uint8_t,sdgo::operators::kCategorySize> item{};
            if (!Read(id,0x2C,op.data(),op.size()) || !Read(category,0x22,item.data(),item.size()))return Fail(6);
            uint32_t reference=0;memcpy(&reference,item.data()+105,4);
            if (sdgo::operators::ReadId(op.data())!=id || sdgo::operators::ReadId(item.data())!=category || item[4]!=0 || reference!=id)
                return Fail(7);
            std::array<wchar_t,100> actual{};memcpy(actual.data(),op.data()+5,actual.size()*sizeof(wchar_t));
            if (wmemcmp(actual.data(),expected.c_str(),expected.size()+1))return Fail(8);
            ++state_.verified;
        }
        if (!state_.configured)return Fail(9);
        return state_.verified==state_.configured;
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
    bool Fail(uint32_t error){state_.error=error;return false;}
    GridData data_=nullptr;GridFind find_=nullptr;Observation state_{};
};
