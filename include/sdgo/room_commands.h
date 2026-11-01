// Shared room-title grammar used by core and mods, including older-core fallback.
#pragma once
#include "sdgo_api.h"

namespace sdgo::room_commands {
inline char Lower(char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a'-'A')) : ch; }
inline bool Space(char ch) { return ch == ' ' || (ch >= '\t' && ch <= '\r'); }
inline uint32_t Length(const char* text, uint32_t bound) {
    uint32_t n=0;
    if(text) while(n<bound && text[n]) ++n;
    return n;
}
struct Token { uint32_t offset=0, length=0; };
inline bool Next(const char* text, uint32_t end, uint32_t& cursor, Token& token) {
    if(!text) return false;
    while(cursor<end && Space(text[cursor])) ++cursor;
    token.offset=cursor;
    while(cursor<end && text[cursor] && !Space(text[cursor])) ++cursor;
    token.length=cursor-token.offset;
    return token.length!=0;
}
inline bool Number(const char* text, uint32_t length, uint32_t& out) {
    if(!text || !length) return false;
    uint32_t value=0;
    for(uint32_t i=0;i<length;++i) {
        if(text[i]<'0' || text[i]>'9') return false;
        const uint32_t digit=static_cast<uint32_t>(text[i]-'0');
        value=value>(UINT32_MAX-digit)/10 ? UINT32_MAX : value*10+digit;
    }
    out=value; return true;
}
inline int Parse(const char* title, uint32_t titleLength, const char* name,
                 uint32_t flags, SdgoRoomCommand* out) {
    if(!out) return 0;
    *out={};
    if(!title || !name || !name[0] || (flags & ~SDGO_ROOM_COMMAND_ALLOW_COUNT)) return 0;
    uint32_t nameLength=0;
    for(;name[nameLength];++nameLength) {
        const char ch=Lower(name[nameLength]);
        if((ch<'a' || ch>'z') && ch!='_') return 0;
    }
    const uint32_t end=Length(title,titleLength);
    uint32_t cursor=0;
    Token token;
    while(Next(title,end,cursor,token)) {
        if(title[token.offset]!='!' || token.length<=nameLength) continue;
        bool same=true;
        for(uint32_t i=0;i<nameLength;++i)
            if(Lower(title[token.offset+1+i])!=Lower(name[i])) { same=false; break; }
        if(!same) continue;
        const uint32_t suffix=token.length-1-nameLength;
        uint32_t count=0;
        if(suffix && (!(flags & SDGO_ROOM_COMMAND_ALLOW_COUNT)
           || !Number(title+token.offset+1+nameLength,suffix,count))) continue;
        out->token_offset=token.offset; out->token_length=token.length;
        out->has_count=suffix?1u:0u; out->count=count;
        const uint32_t argumentStart=cursor;
        uint32_t argumentEnd=end;
        while(Next(title,end,cursor,token)) {
            if(title[token.offset]=='!') { argumentEnd=token.offset; break; }
        }
        out->arguments_offset=argumentStart;
        out->arguments_length=argumentEnd-argumentStart;
        return 1;
    }
    return 0;
}
inline int Find(const SdgoApi* api, const char* title, uint32_t titleLength,
                const char* name, uint32_t flags, SdgoRoomCommand* out) {
    if(api && api->size>=offsetof(SdgoApi,FindRoomCommand)+sizeof(api->FindRoomCommand)
       && api->FindRoomCommand) return api->FindRoomCommand(title,titleLength,name,flags,out);
    return Parse(title,titleLength,name,flags,out);
}
} // namespace sdgo::room_commands
