#pragma once
#include <algorithm>
#include <cwctype>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace sdgo::rank_fix {
inline constexpr const wchar_t* kSchema = L"sdgo_rank_progression_fix";
inline constexpr const wchar_t* kNames[] = {L"sp_SetBattleExp_V4", L"sp_SetGridBattleExp_V2"};
inline std::wstring Identifier(const std::wstring& value) {
    std::wstring out=L"["; for(wchar_t c:value){out+=c;if(c==L']')out+=c;} return out+L"]";
}
inline std::wstring Literal(const std::wstring& value) {
    std::wstring out=L"N'";for(wchar_t c:value){if(!c)throw std::runtime_error("embedded SQL null");out+=c;if(c==L'\'')out+=c;}return out+L"'";
}
inline std::wstring Lower(std::wstring value) {
    for(auto& c:value)if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';return value;
}
inline std::wstring Mask(const std::wstring& text) {
    std::wstring out=text;size_t i=0;
    while(i<text.size()) {
        size_t start=i;
        if(text.compare(i,2,L"--")==0){auto n=text.find(L'\n',i);i=n==std::wstring::npos?text.size():n;}
        else if(text.compare(i,2,L"/*")==0){
            int depth=1;i+=2;
            while(i<text.size()&&depth){if(text.compare(i,2,L"/*")==0){++depth;i+=2;}else if(text.compare(i,2,L"*/")==0){--depth;i+=2;}else ++i;}
            if(depth)throw std::runtime_error("unclosed SQL comment");
        }else if(text[i]==L'\''||text[i]==L'"'||text[i]==L'['){
            const wchar_t end=text[i]==L'['?L']':text[i];bool closed=false;++i;
            while(i<text.size()){if(text[i++]==end){if(i<text.size()&&text[i]==end)++i;else{closed=true;break;}}}
            if(!closed)throw std::runtime_error("unclosed SQL quote");
        }else{++i;continue;}
        for(size_t j=start;j<i;++j)if(out[j]!=L'\r'&&out[j]!=L'\n')out[j]=L' ';
    }
    return out;
}
struct Header { size_t action, actionLength, name, nameLength; };
inline Header ParseHeader(const std::wstring& sql,const std::wstring& name,const std::wstring& schema) {
    const auto code=Mask(sql);
    static const std::wregex h(LR"(\b(CREATE|ALTER)\s+(?:PROC|PROCEDURE)\b)",std::regex::icase);
    static const std::wregex n(LR"(\s*(?:(\[[A-Za-z_][A-Za-z_0-9]*\]|[A-Za-z_][A-Za-z_0-9]*)\s*\.\s*)?(\[[A-Za-z_][A-Za-z_0-9]*\]|[A-Za-z_][A-Za-z_0-9]*))");
    std::wsmatch match,object;
    if(!std::regex_search(code,match,h)||code.substr(0,match.position()).find_first_not_of(L" \t\r\n")!=std::wstring::npos)
        throw std::runtime_error("unknown procedure header");
    size_t after=static_cast<size_t>(match.position()+match.length());
    auto tail=sql.substr(after);
    if(!std::regex_search(tail,object,n,std::regex_constants::match_continuous))throw std::runtime_error("unknown procedure name");
    auto unquote=[](std::wstring s){if(!s.empty()&&s.front()==L'[')return s.substr(1,s.size()-2);return s;};
    const auto owner=object[1].matched?unquote(object[1].str()):L"dbo";
    if(Lower(unquote(object[2].str()))!=Lower(name)||Lower(owner)!=Lower(schema))throw std::runtime_error("procedure identity mismatch");
    size_t begin=after;while(begin<sql.size()&&iswspace(sql[begin]))++begin;
    return {static_cast<size_t>(match.position(1)),static_cast<size_t>(match.length(1)),begin,
            after+static_cast<size_t>(object.position()+object.length())-begin};
}
inline std::wstring Canonical(const std::wstring& sql,const std::wstring& name,const std::wstring& schema) {
    auto h=ParseHeader(sql,name,schema);auto out=sql;out.replace(h.action,h.actionLength,L"CREATE");return out;
}
inline std::wstring Clone(const std::wstring& source,const std::wstring& name,bool exists) {
    const auto code=Mask(source);
    static const std::wregex old(LR"(\bIF\s*\(\s*(@UserLevelNumber)\s*<\s*(@newlevel)\s+OR\s+(@UserLevelStep)\s*<\s*(@NewLevelStep)\s*\))",std::regex::icase);
    static const std::wregex fixed(LR"(\bIF\s*\(\s*@UserLevelNumber\s*<\s*@newlevel\s+OR\s*\(\s*@UserLevelNumber\s*=\s*@newlevel\s+AND\s*@UserLevelStep\s*<\s*@NewLevelStep\s*\)\s*\))",std::regex::icase);
    std::vector<std::wsmatch> olds,fixes;
    for(auto i=std::wsregex_iterator(code.begin(),code.end(),old);i!=std::wsregex_iterator();++i)olds.push_back(*i);
    for(auto i=std::wsregex_iterator(code.begin(),code.end(),fixed);i!=std::wsregex_iterator();++i)fixes.push_back(*i);
    auto result=source;
    if(olds.size()==1&&fixes.empty()){
        const auto& m=olds.front();
        const auto replacement=L"IF("+m[1].str()+L" < "+m[2].str()+L" OR ("+m[1].str()+L" = "+m[2].str()+L" AND "+m[3].str()+L" < "+m[4].str()+L"))";
        result.replace(static_cast<size_t>(m.position()),static_cast<size_t>(m.length()),replacement);
    }else if(!olds.empty()||fixes.size()!=1)throw std::runtime_error("unknown rank comparison");
    auto h=ParseHeader(result,name,L"dbo");
    result.replace(h.name,h.nameLength,Identifier(kSchema)+L"."+Identifier(name));
    result.replace(h.action,h.actionLength,exists?L"ALTER":L"CREATE");
    return result;
}
inline std::wstring Redirect(const std::wstring& command,const std::wstring& name) {
    const std::wstring prefix=L"{? = call "+name+L"(";
    if(command.compare(0,prefix.size(),prefix)!=0||command.size()<prefix.size()+2||command.substr(command.size()-2)!=L")}")
        throw std::runtime_error("unexpected native command");
    auto out=command;out.replace(10,name.size(),Identifier(kSchema)+L"."+Identifier(name));return out;
}
}
