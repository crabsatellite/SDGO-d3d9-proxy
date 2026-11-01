#pragma once
#include "native_title_image.h"
#include <array>
#include <memory>

// Only the stock notification's memory-copy instructions run here. No client
// imports, entry point, service callbacks, game state, account or socket runs.
namespace room_random_native_test {
__declspec(naked) inline void __cdecl Invoke(void*,const void*,void*) {
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        mov ebp,[esp+14h]
        mov ebx,[esp+18h]
        mov eax,[esp+1Ch]
        call eax
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
class ClientCopies {
    struct Free {void operator()(uint8_t* p) const {if(p) VirtualFree(p,0,MEM_RELEASE);}};
    using Code=std::unique_ptr<uint8_t,Free>;
    Code unit_,ship_;
    static const uint8_t* At(const std::vector<uint8_t>& file,uint32_t rva,size_t length) {
        using namespace room_title_native::detail;
        const auto dos=ReadStruct<IMAGE_DOS_HEADER>(file,0);
        const auto nt=ReadStruct<IMAGE_NT_HEADERS32>(file,static_cast<size_t>(dos.e_lfanew));
        Require(nt.FileHeader.Machine==IMAGE_FILE_MACHINE_I386,"client fixture is not PE32");
        const size_t sections=static_cast<size_t>(dos.e_lfanew)+24+nt.FileHeader.SizeOfOptionalHeader;
        for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i) {
            const auto s=ReadStruct<IMAGE_SECTION_HEADER>(file,sections+i*sizeof(IMAGE_SECTION_HEADER));
            if(rva>=s.VirtualAddress && rva-s.VirtualAddress<s.SizeOfRawData) {
                const size_t off=s.PointerToRawData+rva-s.VirtualAddress;
                Require(off<=file.size() && length<=file.size()-off,"client code span exceeds file");
                return file.data()+off;
            }
        }
        throw std::runtime_error("missing client code RVA");
    }
    static Code Copy(const uint8_t* bytes,const char* expected,bool lastByte) {
        using namespace room_title_native::detail;
        Require(Sha256(bytes,0x20)==expected,"native client copy instructions changed");
        Code code(static_cast<uint8_t*>(VirtualAlloc(nullptr,0x22,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)));
        Require(code!=nullptr,"native copy fixture allocation failed");
        std::memcpy(code.get(),bytes,0x20);
        // The single-unit copy's final MOVSB follows a log-call stack cleanup
        // in the original handler. Execute it without that unrelated cleanup.
        code.get()[0x20]=lastByte ? 0xA4 : 0xC3;
        code.get()[0x21]=0xC3;
        DWORD previous=0;
        Require(VirtualProtect(code.get(),0x22,PAGE_EXECUTE_READ,&previous)!=FALSE,"native copy protection failed");
        FlushInstructionCache(GetCurrentProcess(),code.get(),0x22);
        return code;
    }
public:
    explicit ClientCopies(const std::wstring& path) {
        using namespace room_title_native::detail;
        const auto file=ReadFile(path);
        Require(Sha256(file.data(),file.size())=="e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92",
                "unsupported GOnline.exe fixture SHA-256");
        Require(*At(file,0x0063FA05,1)==0xA4,"native unit-copy final byte changed");
        unit_=Copy(At(file,0x0063F9E2,0x20),"04550dab61ba6e12f802263fcd995f2420e6fd87aec7e35e3509ebafd91b24ce",true);
        ship_=Copy(At(file,0x00640845,0x20),"ef3d15ab15ca035ded83064d4af9d09db53c831386e15333cbc0fb7ad130f604",false);
    }
    void Apply(std::vector<uint8_t>& service,const uint8_t* packet,uint16_t opcode) {
        std::array<uint8_t,0x1000> frame{};
        auto* base=frame.data()+0x800;
        auto* pointer=service.data();std::memcpy(base-0x7D8,&pointer,sizeof(pointer));
        Invoke(base,packet,opcode==0x0861 ? unit_.get() : ship_.get());
    }
};
}
