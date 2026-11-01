#pragma once
#include <string_view>
namespace battle_ai {
enum class Difficulty { Easy=0, Medium=1, Hard=2 };
inline wchar_t Lower(wchar_t c) {return c>=L'A'&&c<=L'Z'?c+(L'a'-L'A'):c;}
inline bool Word(std::wstring_view text,std::wstring_view expected) {
    if(text.size()!=expected.size()) return false;
    for(size_t i=0;i<text.size();++i) if(Lower(text[i])!=expected[i]) return false;
    return true;
}
// Difficulty is an option on the existing !ai command. NPC pool lvN and AI
// counts retain their server-side meanings. Return -1 when no override exists.
inline int RoomDifficulty(std::wstring_view title) {
    for(size_t i=0;i+3<=title.size();++i) {
        if(title[i]!=L'!'||Lower(title[i+1])!=L'a'||Lower(title[i+2])!=L'i') continue;
        size_t p=i+3;
        while(p<title.size()&&title[p]>=L'0'&&title[p]<=L'9') ++p;
        if(p<title.size()&&title[p]!=L' '&&title[p]!=L'\t') continue;
        int result=-1;
        while(p<title.size()) {
            while(p<title.size()&&(title[p]==L' '||title[p]==L'\t')) ++p;
            size_t begin=p;
            while(p<title.size()&&title[p]!=L' '&&title[p]!=L'\t') ++p;
            auto token=title.substr(begin,p-begin);
            if(Word(token,L"easy")) result=0;
            else if(Word(token,L"medium")) result=1;
            else if(Word(token,L"hard")) result=2;
        }
        return result;
    }
    return -1;
}
}
