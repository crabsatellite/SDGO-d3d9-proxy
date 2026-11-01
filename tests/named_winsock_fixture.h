// SDK test programs import Winsock by ordinal; shipped GOnline imports these
// three functions by name. Give the synthetic host the same import metadata
// before core initialization, so the synthetic client models GOnline faithfully.
#pragma once
#include <windows.h>
#include <cstring>
inline bool NameClientWinsockImports() {
    struct Name { WORD hint; char text[16]; };
    static Name names[]={{0,"connect"},{0,"sendto"},{0,"recvfrom"}};
    const WORD ordinals[]={4,20,17};
    auto* base=reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto* desc=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    unsigned found=0;
    for(;desc->Name;++desc) {
        if(_stricmp(reinterpret_cast<char*>(base+desc->Name),"ws2_32.dll"))continue;
        auto* thunk=reinterpret_cast<IMAGE_THUNK_DATA*>(base+desc->OriginalFirstThunk);
        for(;thunk->u1.AddressOfData;++thunk) {
            for(unsigned i=0;i<3;++i) {
                if(thunk->u1.Ordinal&IMAGE_ORDINAL_FLAG) {
                    if(IMAGE_ORDINAL(thunk->u1.Ordinal)!=ordinals[i])continue;
                    DWORD old=0,ignored=0;
                    if(!VirtualProtect(thunk,sizeof(*thunk),PAGE_READWRITE,&old))return false;
                    thunk->u1.AddressOfData=static_cast<DWORD>(reinterpret_cast<BYTE*>(&names[i])-base);
                    VirtualProtect(thunk,sizeof(*thunk),old,&ignored);
                    ++found;break;
                }
                auto* name=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+thunk->u1.AddressOfData);
                if(!strcmp(reinterpret_cast<char*>(name->Name),names[i].text)){++found;break;}
            }
        }
    }
    return found==3;
}
