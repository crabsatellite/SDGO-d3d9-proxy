#pragma once
#include <string_view>
namespace battle_ai {
enum class Difficulty { Easy=0, Medium=1, Hard=2 };
template<class C> inline C Lower(C c) {return c>=C('A')&&c<=C('Z')?C(c+C('a'-'A')):c;}
template<class C> inline bool Space(C c) {return c==C(' ')||(c>=C('\t')&&c<=C('\r'));}
template<class C> inline bool Word(std::basic_string_view<C> text,const char* expected) {
 size_t n=0;while(expected[n]) ++n;if(text.size()!=n) return false;
 for(size_t i=0;i<n;++i) if(Lower(text[i])!=C(expected[i])) return false;return true;
}
template<class C> inline int RoomDifficultyText(std::basic_string_view<C> title) {
 size_t p=0;
 while(p<title.size()) {
  while(p<title.size()&&Space(title[p])) ++p;
  size_t begin=p;while(p<title.size()&&!Space(title[p])) ++p;
  auto command=title.substr(begin,p-begin);
  if(command.size()<3||command[0]!=C('!')||Lower(command[1])!=C('a')||Lower(command[2])!=C('i')) continue;
  bool valid=true;for(size_t i=3;i<command.size();++i) if(command[i]<C('0')||command[i]>C('9')) valid=false;
  if(!valid) continue;
  int result=-1;
  while(p<title.size()) {
   while(p<title.size()&&Space(title[p])) ++p;
   begin=p;while(p<title.size()&&!Space(title[p])) ++p;
   auto token=title.substr(begin,p-begin);if(!token.empty()&&token[0]==C('!')) break;
   if(Word(token,"easy")) result=0;else if(Word(token,"medium")) result=1;else if(Word(token,"hard")) result=2;
  }
  return result;
 }
 return -1;
}
inline int RoomDifficulty(std::wstring_view title) {return RoomDifficultyText(title);}
inline int RoomDifficulty(std::string_view title) {return RoomDifficultyText(title);}
}
