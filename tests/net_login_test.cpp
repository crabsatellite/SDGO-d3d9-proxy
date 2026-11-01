// Real core + real net_route, with an x86 fixture for the verified login call.
// No game files or accounts are needed or executed.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "sdgo/sdgo_api.h"
#include "named_winsock_fixture.h"
namespace {
    int failures = 0;
    unsigned char callCode[32];
    using ParseFn = LPWSTR*(WINAPI*)(LPCWSTR, int*);
    void Check(bool value, const char* label) { printf("%s %s\n", value ? "ok  " : "FAIL", label); if (!value) ++failures; }
    std::wstring Root() {
        wchar_t path[32768]; GetModuleFileNameW(nullptr,path,32768);
        std::wstring result=path; return result.substr(0,result.find_last_of(L"\\/"));
    }
    void Write(const std::wstring& path, const std::string& text) {
        HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        DWORD n=0; Check(file!=INVALID_HANDLE_VALUE && WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&n,nullptr) && n==text.size(),"write fixture configuration");
        if(file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    sockaddr_in Address(unsigned short port) {
        sockaddr_in a={}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=htons(port); return a;
    }
    unsigned short Bind(SOCKET socket) {
        auto a=Address(0); Check(bind(socket,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0,"reserve loopback endpoint");
        int n=sizeof(a); getsockname(socket,reinterpret_cast<sockaddr*>(&a),&n); return ntohs(a.sin_port);
    }
    void** ArgvImport(HMODULE host) {
        auto* base=reinterpret_cast<BYTE*>(host);
        auto* nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        auto* d=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        for(;d->Name;++d) {
            if(_stricmp(reinterpret_cast<char*>(base+d->Name),"shell32.dll"))continue;
            auto* names=reinterpret_cast<IMAGE_THUNK_DATA*>(base+d->OriginalFirstThunk);
            auto* iat=reinterpret_cast<IMAGE_THUNK_DATA*>(base+d->FirstThunk);
            for(size_t i=0;names[i].u1.AddressOfData;++i) {
                if(names[i].u1.Ordinal&IMAGE_ORDINAL_FLAG)continue;
                auto* name=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+names[i].u1.AddressOfData);
                if(!strcmp(reinterpret_cast<char*>(name->Name),"CommandLineToArgvW"))return reinterpret_cast<void**>(&iat[i].u1.Function);
            }
        }
        return nullptr;
    }
}
int wmain(int argc,wchar_t** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    Check(NameClientWinsockImports(),"synthetic host models GOnline named Winsock imports");
    const bool reject=argc>1 && !wcscmp(argv[1],L"reject");
    const bool invalid=argc>1 && !wcscmp(argv[1],L"invalid");
    const auto root=Root(); const auto host=GetModuleHandleW(nullptr);
    WSADATA ws={}; WSAStartup(MAKEWORD(2,2),&ws);
    SOCKET listener=socket(AF_INET,SOCK_STREAM,0), guard=socket(AF_INET,SOCK_STREAM,0);
    const auto external=Bind(listener), logical=Bind(guard); listen(listener,1);
    const wchar_t* command=L"\"modified launch.exe\" old-ip 1234 \"keep space\" \"\" \"literal\\\"quote\" C:\\path\\file \"C:\\tail\\\\\"";
    int originalCount=0; LPWSTR* original=CommandLineToArgvW(command,&originalCount);
    void** slot=ArgvImport(host); Check(slot!=nullptr,"fixture finds real shell32 IAT");
    if(!slot)return 2;
    const BYTE code[]={0xff,0x74,0x24,0x08,0xff,0x74,0x24,0x08,0xff,0x15,0,0,0,0,0xc2,0x08,0x00};
    memcpy(callCode,code,sizeof(code)); const auto operand=reinterpret_cast<uintptr_t>(slot); memcpy(callCode+10,&operand,4);
    DWORD previous=0; Check(VirtualProtect(callCode,sizeof(callCode),PAGE_EXECUTE_READWRITE,&previous)!=0,"prepare exact x86 call fixture");
    FlushInstructionCache(GetCurrentProcess(),callCode,sizeof(callCode));
    auto* base=reinterpret_cast<BYTE*>(host);
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);
    std::wstring stem=exe;stem=stem.substr(stem.find_last_of(L"\\/")+1);stem=stem.substr(0,stem.find_last_of(L'.'));
    wchar_t key[128]; swprintf_s(key,L"%s-%08X-%08X",stem.c_str(),nt->FileHeader.TimeDateStamp,nt->OptionalHeader.SizeOfImage);
    char symbols[1024];sprintf_s(symbols,"[host]\ntimestamp=0x%08X\nsize_of_image=0x%08X\n[net.login_argv_call]\nkind=site\nrva=0x%08X\nexpect=%s\n",
        nt->FileHeader.TimeDateStamp,nt->OptionalHeader.SizeOfImage,static_cast<unsigned>(callCode+8-base),reject?"90 90 90 90 90 90":"FF 15 ?? ?? ?? ??");
    CreateDirectoryW((root+L"\\sdgo\\symbols").c_str(),nullptr);
    Write(root+L"\\sdgo\\symbols\\"+key+L".ini",symbols);
    char settings[1024];sprintf_s(settings,"[settings]\nLoginIPv4=%s\nLoginPort=%u\nRouteCount=1\nTrace=1\nRoute1=tcp,127.0.0.1,%u,127.0.0.1,%u\n",invalid?"192.0.2.9":"127.0.0.1",logical,logical,external);
    Write(root+L"\\sdgo\\mods\\net_route\\config.ini",settings);
    const auto core=LoadLibraryW((root+L"\\sdgo\\core.dll").c_str());
    const auto init=core?reinterpret_cast<SdgoCoreInitFn>(GetProcAddress(core,"SdgoCoreInit")):nullptr;
    Check(init!=nullptr,"load real framework core"); if(!init)return 2;
    SdgoBoot boot={sizeof(boot),"login_test",host};Check(init(&boot)!=0,"start real mod discovery");
    int count=0;const auto fixture=reinterpret_cast<ParseFn>(&callCode[0]);
    LPWSTR* parsed=fixture(command,&count);
    const bool active=!reject && !invalid;
    if(active) {
        Check(parsed && count>=3 && !wcscmp(parsed[1],L"127.0.0.1") && static_cast<unsigned>(_wtoi(parsed[2]))==logical,"native parser uses configured login tuple");
        Check(parsed && !wcscmp(parsed[0],original[0]) && count==(originalCount<3?3:originalCount),"executable and argument count preserved");
        for(int i=3;i<originalCount;++i)Check(parsed && count>i && !wcscmp(parsed[i],original[i]),"trailing launcher argument survives quoting");
    } else Check(parsed && count==originalCount && !wcscmp(parsed[1],L"old-ip"),"bad symbol or login installs no parser hook");
    if(parsed)LocalFree(parsed);
    parsed=fixture(L"\"alternate launcher.exe\"",&count);
    Check(parsed && (active ? count==3 && !wcscmp(parsed[0],L"alternate launcher.exe") && !wcscmp(parsed[1],L"127.0.0.1") : count==1),"native command without login arguments receives only the configured tuple");
    if(parsed)LocalFree(parsed);
    parsed=CommandLineToArgvW(L"unrelated.exe preserve-this 99",&count);
    Check(parsed && count==3 && !wcscmp(parsed[1],L"preserve-this"),"unrelated parsing remains unchanged");if(parsed)LocalFree(parsed);
    SOCKET client=socket(AF_INET,SOCK_STREAM,0);auto a=Address(logical);
    const int connected=connect(client,reinterpret_cast<sockaddr*>(&a),sizeof(a));
    Check(active?connected==0:connected==SOCKET_ERROR,"login validation and route activation are atomic");
    Check(sendto(INVALID_SOCKET,"",0,0,nullptr,0)==SOCKET_ERROR && recvfrom(INVALID_SOCKET,nullptr,0,0,nullptr,nullptr)==SOCKET_ERROR,"all required Winsock imports retain error handling");
    if(original)LocalFree(original);closesocket(client);closesocket(listener);closesocket(guard);WSACleanup();
    puts(failures?"NET LOGIN CHECKS FAILED":"ALL NET LOGIN CHECKS PASSED");return failures?1:0;
}
