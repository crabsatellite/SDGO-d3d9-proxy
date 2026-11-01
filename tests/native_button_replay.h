#pragma once
// Shared isolated replay helper. No target entry point or imports are invoked.
#include "native_title_image.h"
#include <cassert>
#include <cstring>
namespace native_button_replay {
template<class T>void Put(void* p,size_t n,T value){memcpy(static_cast<uint8_t*>(p)+n,&value,sizeof(value));}
template<class T>T Get(const void* p,size_t n){T value;memcpy(&value,static_cast<const uint8_t*>(p)+n,sizeof(value));return value;}
class Snippet {
    uint8_t* code_=nullptr;
public:
    Snippet(const std::vector<uint8_t>& file,uint32_t va,size_t size,const char* expected) {
        auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
        auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(file.data()+dos->e_lfanew);
        const auto rva=va-nt->OptionalHeader.ImageBase;
        const auto* sections=IMAGE_FIRST_SECTION(nt);const uint8_t* source=nullptr;
        for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
            const auto& section=sections[i];
            if(rva>=section.VirtualAddress && rva-section.VirtualAddress+size<=section.SizeOfRawData)
                source=file.data()+section.PointerToRawData+rva-section.VirtualAddress;
        }
        if(!source || room_title_native::detail::Sha256(source,size)!=expected)throw std::runtime_error("native snippet mismatch");
        code_=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        if(!code_)throw std::runtime_error("snippet allocation failed");
        memcpy(code_,source,size);
    }
    ~Snippet(){if(code_)VirtualFree(code_,0,MEM_RELEASE);}
    Snippet(const Snippet&)=delete;
    Snippet& operator=(const Snippet&)=delete;
    template<class T>void Call(size_t offset,T target) {
        assert(code_[offset]==0xE8);
        const auto delta=reinterpret_cast<uintptr_t>(target)-reinterpret_cast<uintptr_t>(code_+offset+5);
        Put<uint32_t>(code_,offset+1,static_cast<uint32_t>(delta));
    }
    void MouseTable() {
        assert(code_[0xB7]==0xFF && code_[0xB8]==0x24 && code_[0xB9]==0x85);
        Put(code_,0xBA,code_+0x1F0);
        for(size_t i=0;i<4;++i) {
            const uint32_t original=Get<uint32_t>(code_,0x1F0+4*i);
            assert(original>=0xC69080 && original<0xC69270);
            Put(code_,0x1F0+4*i,code_+original-0xC69080);
        }
    }
    void RelocatePointer(size_t offset,uint32_t oldBase,size_t span) {
        uint32_t pointer=0;memcpy(&pointer,code_+offset,4);
        assert(pointer>=oldBase && pointer-oldBase<span);
        const auto pointerValue=reinterpret_cast<uintptr_t>(code_+pointer-oldBase);
        memcpy(code_+offset,&pointerValue,4);
    }
    void Seal(){DWORD old=0;if(!VirtualProtect(code_,4096,PAGE_EXECUTE_READ,&old))throw std::runtime_error("snippet seal failed");FlushInstructionCache(GetCurrentProcess(),code_,4096);}
    template<class T>T Function(){return reinterpret_cast<T>(code_);}
};

}
