#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "hangar_model.h"

namespace sdgo::hangar {
// Shipped CServicePaperSend request (00817360), routed through the existing
// authenticated Agent/World session. The reserved recipient is NOT a player.
inline constexpr char kSortRecipient[]="SDGOHangar";
inline constexpr char kSortPrefix[]="!sdgo-sort-v1 ";
inline void ValidateRequestId(const std::string& request) {
    if(request.size()!=36)throw std::invalid_argument("invalid request identity");
    for(size_t i=0;i<request.size();++i) {
        const char c=request[i];const bool separator=i==8 || i==13 || i==18 || i==23;
        if(separator?c!='-':!((c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F')))
            throw std::invalid_argument("invalid request identity");
    }
}
inline std::array<uint8_t,0x159> HangarRequest(const char* sender,const std::string& body) {
    if(!sender || !*sender || body.empty() || body.size()>300)throw std::invalid_argument("invalid hangar request");
    size_t length=0;while(length<=16 && sender[length])++length;
    if(length>16)throw std::invalid_argument("invalid pilot name");
    std::array<uint8_t,0x159> bytes{};
    bytes[0]=0x91;bytes[1]=0x08;
    memcpy(bytes.data()+6,sender,length);
    memcpy(bytes.data()+0x1B,kSortRecipient,sizeof(kSortRecipient)-1);
    memcpy(bytes.data()+0x2C,body.data(),body.size());
    return bytes;
}
inline std::array<uint8_t,0x159> ServerSortRequest(const char* sender,const std::string& request) {
    ValidateRequestId(request);return HangarRequest(sender,std::string(kSortPrefix)+request);
}
inline std::vector<std::array<uint8_t,0x159>> ServerGBRequests(const char* sender,const std::string& request,const Plan& plan,int keep,bool sortAfter) {
    ValidateRequestId(request);
    if(!plan.serverBatch || plan.actions.empty() || plan.actions.size()>1200 || keep<1 || keep>20)throw std::invalid_argument("invalid GB batch");
    const std::string prefix="!sdgo-gb-v1 "+request+" ";
    std::vector<std::array<uint8_t,0x159>> result;
    result.push_back(HangarRequest(sender,prefix+"H "+std::to_string(keep)+" "+std::to_string(plan.actions.size())+" "+std::to_string(plan.expectedGB)+" "+(sortAfter?"1":"0")));
    const char* hex="0123456789ABCDEF";
    for(size_t first=0;first<plan.actions.size();first+=8) {
        std::string data;
        for(size_t i=first;i<std::min(first+8,plan.actions.size());++i) {
            const auto& a=plan.actions[i];
            if(a.kind!=Kind::GB || !a.first || a.first>INT64_MAX || !a.type || !a.from)throw std::invalid_argument("invalid selected unit");
            for(auto field:std::array<std::pair<uint64_t,int>,3>{{{a.first,8},{a.type,4},{a.from,2}}})
                for(int byte=field.second-1;byte>=0;--byte){const auto value=static_cast<uint8_t>(field.first>>(byte*8));data+=hex[value>>4];data+=hex[value&15];}
        }
        result.push_back(HangarRequest(sender,prefix+"D "+std::to_string(first/8)+" "+data));
    }
    result.push_back(HangarRequest(sender,prefix+"C"));
    return result;
}
}
