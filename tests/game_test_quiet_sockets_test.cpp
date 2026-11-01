#include "../mods/game_test_client/quiet_sockets.h"
#include <cstdio>

namespace {
QuietSockets::BindFn detour=nullptr;
int __cdecl Hook(SdgoMod*,const char* library,const char* name,void* replacement,void** original) {
    if (std::strcmp(library,"ws2_32.dll") || std::strcmp(name,"bind"))return 0;
    detour=reinterpret_cast<QuietSockets::BindFn>(replacement);*original=reinterpret_cast<void*>(&::bind);return 1;
}
}
int main() {
    WSADATA data{};if (WSAStartup(MAKEWORD(2,2),&data))return 1;
    SdgoApi api{};api.HookImport=&Hook;
    QuietSockets guard;int failed=0;
    if (!guard.Install(&api,reinterpret_cast<SdgoMod*>(1)) || !detour)return 1;
    // These are real Windows UDP sockets. Public/wildcard addresses never reach
    // the real bind; no game, database or desktop is opened by this test.
    const uint32_t addresses[]={INADDR_ANY,0xC0000201u,0x7F00005Au};
    for (uint32_t requested:addresses) {
        SOCKET handle=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        sockaddr_in input{};input.sin_family=AF_INET;input.sin_addr.s_addr=htonl(requested);
        const auto saved=input;
        if (handle==INVALID_SOCKET || detour(handle,reinterpret_cast<sockaddr*>(&input),sizeof(input)))++failed;
        sockaddr_in actual{};int size=sizeof(actual);
        if (getsockname(handle,reinterpret_cast<sockaddr*>(&actual),&size) ||
            (ntohl(actual.sin_addr.s_addr)>>24)!=127 || !actual.sin_port || std::memcmp(&saved,&input,sizeof(input)))++failed;
        closesocket(handle);
    }
    if (detour(INVALID_SOCKET,nullptr,0)!=SOCKET_ERROR || WSAGetLastError()!=WSAEFAULT)++failed;
    sockaddr_in input{};input.sin_family=AF_INET;
    if (detour(INVALID_SOCKET,reinterpret_cast<sockaddr*>(&input),sizeof(input))!=SOCKET_ERROR ||
        WSAGetLastError()!=WSAENOTSOCK)++failed;
    const auto observed=guard.State();
    if (!observed.active || observed.binds!=5 || observed.redirected!=3 || observed.failed!=2 || observed.escaped)++failed;
    WSACleanup();std::printf("Native loopback bind guard: %d failures\n",failed);return failed ? 1:0;
}
