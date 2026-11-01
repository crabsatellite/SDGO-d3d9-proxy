#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace sdgo::hangar {
// Shipped CServicePaperSend request (00817360), routed through the existing
// authenticated Agent/World session. The reserved recipient is NOT a player.
inline constexpr char kSortRecipient[]="SDGOHangar";
inline constexpr char kSortPrefix[]="!sdgo-sort-v1 ";
inline std::array<uint8_t,0x159> ServerSortRequest(const char* sender,const std::string& request) {
    if(!sender || !*sender || request.size()!=36)throw std::invalid_argument("invalid sort request");
    size_t length=0;while(length<=16 && sender[length])++length;
    if(length>16)throw std::invalid_argument("invalid pilot name");
    for(size_t i=0;i<request.size();++i) {
        const char c=request[i];const bool separator=i==8 || i==13 || i==18 || i==23;
        if(separator?c!='-':!((c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F')))
            throw std::invalid_argument("invalid request identity");
    }
    std::array<uint8_t,0x159> bytes{};
    bytes[0]=0x91;bytes[1]=0x08;
    memcpy(bytes.data()+6,sender,length);
    memcpy(bytes.data()+0x1B,kSortRecipient,sizeof(kSortRecipient)-1);
    memcpy(bytes.data()+0x2C,kSortPrefix,sizeof(kSortPrefix)-1);
    memcpy(bytes.data()+0x2C+sizeof(kSortPrefix)-1,request.data(),request.size());
    return bytes;
}
}
