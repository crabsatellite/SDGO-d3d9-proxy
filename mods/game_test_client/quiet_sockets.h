#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "sdgo/sdgo_api.h"

// Local GameTest uses real sockets and real server packets, but has no reason
// to publish a listener on a LAN/public interface. This changes only bind calls
// imported by the owned game process; it does not modify Windows Firewall.
class QuietSockets {
public:
    using BindFn=int(WSAAPI*)(SOCKET,const sockaddr*,int);
    struct Observation { uint32_t active,binds,redirected,failed,escaped; };
    bool Install(const SdgoApi* api,SdgoMod* mod) {
        if (!api || !api->HookImport || (active_ && active_!=this))return false;
        active_=this;
        if (!api->HookImport(mod,"ws2_32.dll","bind",reinterpret_cast<void*>(&Bind),
                            reinterpret_cast<void**>(&original_)) || !original_) {active_=nullptr;return false;}
        installed_=true;return true;
    }
    Observation State() const {
        return {installed_ ? 1u:0u,Read(binds_),Read(redirected_),Read(failed_),Read(escaped_)};
    }
private:
    static uint32_t Read(volatile LONG& value) {
        return static_cast<uint32_t>(InterlockedCompareExchange(&value,0,0));
    }
    static int WSAAPI Bind(SOCKET socket,const sockaddr* address,int length) {
        QuietSockets* self=active_;
        if (!self || !self->original_) {WSASetLastError(WSAEACCES);return SOCKET_ERROR;}
        InterlockedIncrement(&self->binds_);
        if (!address || length<static_cast<int>(sizeof(address->sa_family))) {
            InterlockedIncrement(&self->failed_);WSASetLastError(WSAEFAULT);return SOCKET_ERROR;
        }
        sockaddr_storage local{};
        int localSize=0;
        if (address->sa_family==AF_INET && length>=static_cast<int>(sizeof(sockaddr_in))) {
            auto* ipv4=reinterpret_cast<sockaddr_in*>(&local);
            std::memcpy(ipv4,address,sizeof(*ipv4));localSize=sizeof(*ipv4);
            if ((ntohl(ipv4->sin_addr.s_addr)>>24)!=127) {
                ipv4->sin_addr.s_addr=htonl(INADDR_LOOPBACK);InterlockedIncrement(&self->redirected_);
            }
        } else if (address->sa_family==AF_INET6 && length>=static_cast<int>(sizeof(sockaddr_in6))) {
            auto* ipv6=reinterpret_cast<sockaddr_in6*>(&local);
            std::memcpy(ipv6,address,sizeof(*ipv6));localSize=sizeof(*ipv6);
            const IN6_ADDR loopback=IN6ADDR_LOOPBACK_INIT;
            if (std::memcmp(&ipv6->sin6_addr,&loopback,sizeof(loopback))) {
                ipv6->sin6_addr=loopback;ipv6->sin6_scope_id=0;InterlockedIncrement(&self->redirected_);
            }
        } else {
            InterlockedIncrement(&self->failed_);WSASetLastError(WSAEAFNOSUPPORT);return SOCKET_ERROR;
        }
        const int result=self->original_(socket,reinterpret_cast<const sockaddr*>(&local),localSize);
        if (result==SOCKET_ERROR) {InterlockedIncrement(&self->failed_);return result;}
        sockaddr_storage actual{};int actualSize=sizeof(actual);
        bool loopback=false;
        if (getsockname(socket,reinterpret_cast<sockaddr*>(&actual),&actualSize)==0) {
            if (actual.ss_family==AF_INET)
                loopback=(ntohl(reinterpret_cast<sockaddr_in*>(&actual)->sin_addr.s_addr)>>24)==127;
            else if (actual.ss_family==AF_INET6) {
                const IN6_ADDR expected=IN6ADDR_LOOPBACK_INIT;
                loopback=std::memcmp(&reinterpret_cast<sockaddr_in6*>(&actual)->sin6_addr,&expected,sizeof(expected))==0;
            }
        }
        if (!loopback) {
            InterlockedIncrement(&self->escaped_);closesocket(socket);WSASetLastError(WSAEACCES);return SOCKET_ERROR;
        }
        return result;
    }
    inline static QuietSockets* active_=nullptr;
    BindFn original_=nullptr;
    bool installed_=false;
    mutable volatile LONG binds_=0,redirected_=0,failed_=0,escaped_=0;
};
