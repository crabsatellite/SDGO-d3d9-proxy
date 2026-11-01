// Caller-supplied GOnline code in an owned executable slice at its original VAs.
// No CRT in the fixture: its tiny bootstrap stays below the native routines, and
// its reserved image prevents the OS heap from occupying the game's address range.
// The real shipped addon DLL supplies the production registration implementation.
#include <windows.h>
#include <shellapi.h>
#include <cstring>
#include "sdgo/sdgo_api.h"
#pragma runtime_checks("", off)
#pragma section(".text$AAA", read, execute)
__declspec(allocate(".text$AAA")) __declspec(align(16)) unsigned char nativeSlice[0xC97000]{};
extern "C" int _fltused = 0;
#pragma function(memset)
extern "C" void* __cdecl memset(void* target, int value, size_t size) {
    auto* bytes = static_cast<volatile unsigned char*>(target);
    for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<unsigned char>(value);
    return target;
}
namespace {
unsigned char grid[0x58]{};
wchar_t directory[4096]{};
wchar_t catalogPath[4096]{};
unsigned checks = 0, allocations = 0;
using Find = bool(__thiscall*)(void*, const unsigned char**, unsigned, int, char);
using Make = void*(__thiscall*)(void*, const void*);
using Insert = void(__thiscall*)(void*, void*, void*, char);
using MapFind = void(__thiscall*)(void*, void**, const unsigned*);
using Load = void(__fastcall*)(void*, void*, const wchar_t*, unsigned);
using Create = unsigned char*(__fastcall*)(void*, void*, unsigned, float,float,float,int,float,float,float);
Load loadHook = nullptr;
Create createHook = nullptr;
struct Value { unsigned type; unsigned char* bytes; };
struct Pair { unsigned key; Value* value; };
struct Result { void* node; unsigned inserted; };
void Out(const char* text) { DWORD n = 0; unsigned length = 0; while (text[length]) ++length; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &n, nullptr); }
void Number(unsigned value) { char text[16]; unsigned n = 0; do { text[n++] = char('0' + value % 10); value /= 10; } while(value); for(unsigned i=0;i<n/2;++i){char c=text[i];text[i]=text[n-i-1];text[n-i-1]=c;} text[n]=0;Out(text); }
void Check(bool ok, const char* text) { ++checks; if (!ok) { Out("FAIL: ");Out(text);Out("\n");ExitProcess(2); } }
unsigned U32(const void* value) { return *static_cast<const unsigned*>(value); }
void Put(void* p, unsigned value) { *static_cast<unsigned*>(p) = value; }
bool Equal(const void* left, const void* right, unsigned size) { const auto* a=static_cast<const unsigned char*>(left);const auto* b=static_cast<const unsigned char*>(right);for(unsigned i=0;i<size;++i)if(a[i]!=b[i])return false;return true; }
void Copy(void* output, const void* input, unsigned size) { volatile auto* d=static_cast<volatile unsigned char*>(output);const auto* s=static_cast<const unsigned char*>(input);for(unsigned i=0;i<size;++i)d[i]=s[i]; }
bool Name(const char* a,const char* b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
void* __cdecl Alloc(size_t size) { auto* p=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,size);if(p)++allocations;return p; }
void __cdecl Free(void* p) { if(p){Check(HeapValidate(GetProcessHeap(),0,p)!=0,"native cleanup uses owned allocations");Check(HeapFree(GetProcessHeap(),0,p)!=0,"native heap free");--allocations;} }
void* __cdecl Data(){return grid;}
void __fastcall OriginalLoad(void*,void*,const wchar_t*,unsigned){}
unsigned char* __fastcall OriginalCreate(void*,void*,unsigned,float,float,float,int,float,float,float){return reinterpret_cast<unsigned char*>(1);}
void __cdecl Log(SdgoMod*,int level,const char* text,...){if(level>=SDGO_LOG_ERROR){Out("diagnostic: ");Out(text);Out("\n");}}
const wchar_t* __cdecl Directory(SdgoMod*){return directory;}
void* __cdecl Symbol(SdgoMod*,const char* name){
 if(Name(name,"grid.data"))return reinterpret_cast<void*>(&Data);
 if(Name(name,"grid.find"))return reinterpret_cast<void*>(0x59d2b0);
 if(Name(name,"grid.map_find"))return reinterpret_cast<void*>(0x62ba10);
 if(Name(name,"grid.make_node"))return reinterpret_cast<void*>(0x414540);
 if(Name(name,"grid.insert_node"))return reinterpret_cast<void*>(0x414250);
 if(Name(name,"grid.register_record"))return reinterpret_cast<void*>(0x414d70);
 if(Name(name,"crt.operator_new"))return reinterpret_cast<void*>(&Alloc);
 if(Name(name,"crt.operator_delete"))return reinterpret_cast<void*>(&Free);
 return nullptr;
}
int __cdecl Hook(SdgoMod*,const char* name,void* detour,void** original){
 if(Name(name,"grid.load_npc_table")){loadHook=reinterpret_cast<Load>(detour);*original=reinterpret_cast<void*>(&OriginalLoad);return 1;}
 if(Name(name,"mode.base_create_npc")){createHook=reinterpret_cast<Create>(detour);*original=reinterpret_cast<void*>(&OriginalCreate);return 1;}
 if(Name(name,"mode.create_npc")||Name(name,"mode.create_remote_npc")){*original=reinterpret_cast<void*>(&OriginalCreate);return 1;}
 return 0;
}
unsigned char* Read(const wchar_t* path,unsigned* length){
 HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);Check(h!=INVALID_HANDLE_VALUE,"open supplied fixture");
 *length=GetFileSize(h,nullptr);Check(*length&&*length<64*1024*1024,"bounded fixture size");
 auto* data=static_cast<unsigned char*>(HeapAlloc(GetProcessHeap(),0,*length));Check(data!=nullptr,"file buffer allocated");
 DWORD got=0;Check(ReadFile(h,data,*length,&got,nullptr)&&got==*length,"complete fixture read");CloseHandle(h);return data;
}
void Tree(unsigned char* object){auto* head=static_cast<unsigned char*>(Alloc(24));Put(head,unsigned(head));Put(head+4,unsigned(head));Put(head+8,unsigned(head));head[0x14]=1;head[0x15]=1;Put(object+4,unsigned(head));}
void Jump(unsigned address,void* target){auto* code=reinterpret_cast<unsigned char*>(address);code[0]=0xe9;Put(code+1,unsigned(target)-address-5);}
}
extern "C" void __cdecl Entry(){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 int argc=0;wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
 Check(argc==5,"GOnline, addon DLL, catalog directory and unit fixture supplied");
 const unsigned first=unsigned(nativeSlice),last=first+sizeof(nativeSlice);
 Out("owned native slice offset=");Number(first-0x400000);Out("\n");
 Check(GetModuleHandleW(nullptr)==reinterpret_cast<HMODULE>(0x400000)&&first<=0x414000&&last>=0x1097000,"owned section covers all tested native routines");
 DWORD protection=0;Check(VirtualProtect(nativeSlice,sizeof(nativeSlice),PAGE_EXECUTE_READWRITE,&protection)!=0,"owned native slice writable");
 unsigned length=0;auto* file=Read(argv[1],&length);auto* nt=reinterpret_cast<IMAGE_NT_HEADERS*>(file+reinterpret_cast<IMAGE_DOS_HEADER*>(file)->e_lfanew);
 Check(nt->FileHeader.TimeDateStamp==0x54be1568&&nt->OptionalHeader.SizeOfImage==0xc97000,"supported supplied GOnline image");
 auto* sections=IMAGE_FIRST_SECTION(nt);
 for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){const auto& s=sections[i];unsigned start=0x400000+s.VirtualAddress,end=start+s.SizeOfRawData;unsigned begin=start>first?start:first;end=end<last?end:last;if(end>begin)Copy(reinterpret_cast<void*>(begin),file+s.PointerToRawData+begin-start,end-begin);}
 HeapFree(GetProcessHeap(),0,file);
 Jump(0xaf481c,reinterpret_cast<void*>(&Alloc));Jump(0xaf4856,reinterpret_cast<void*>(&Free));FlushInstructionCache(GetCurrentProcess(),nativeSlice,sizeof(nativeSlice));
 for(unsigned i=0;i<4;++i)Tree(grid+i*0x10);
 auto make=reinterpret_cast<Make>(0x414540);auto insert=reinterpret_cast<Insert>(0x414250);auto find=reinterpret_cast<Find>(0x59d2b0);auto mapFind=reinterpret_cast<MapFind>(0x62ba10);
 auto* units=Read(argv[4],&length);Check(length>=12&&Equal(units,"SDGOUN1\0",8),"owner source unit header");unsigned count=U32(units+8);Check(count&&count<=3000&&length==12+count*4976,"owner source count");
 for(unsigned i=0;i<count;++i){const auto* raw=units+12+i*4976;auto* copy=static_cast<unsigned char*>(Alloc(4976));Copy(copy,raw,4976);auto* value=static_cast<Value*>(Alloc(sizeof(Value)));value->type=2;value->bytes=copy;Pair pair{U32(raw),value};Result result{};insert(grid,&result,make(grid,&pair),0);Check(result.inserted!=0,"native unit insertion");}
 lstrcpynW(directory,argv[3],4096);lstrcpynW(catalogPath,directory,4096);lstrcatW(catalogPath,L"\\catalog.bin");
 auto* catalog=Read(catalogPath,&length);Check(length>=24&&U32(catalog+16)==count&&length==24+count*753,"paired catalog count");
 HMODULE addon=LoadLibraryW(argv[2]);Check(addon!=nullptr,"actual client addon DLL loaded");auto entry=reinterpret_cast<SdgoModLoadFn>(GetProcAddress(addon,"SdgoModLoad"));Check(entry!=nullptr,"actual addon entry");
 SdgoApi api{};api.size=sizeof(api);api.version=1;api.Symbol=&Symbol;api.HookCode=&Hook;api.ModDirectory=&Directory;api.Log=&Log;
 Check(entry(&api,nullptr)!=0&&loadHook&&createHook,"production DLL bound to actual native routines");
 unsigned npc=U32(catalog+24);const unsigned char* record=nullptr;void* node=nullptr;
 int initial=find(grid,&record,npc,41,0);Out("initial query id=");Number(npc);Out(" result=");Number(unsigned(initial));Out(" record=");Number(unsigned(record));Out(" tree-size=");Number(U32(grid+8));Out("\n");
 Check(!initial,"native API misses unregistered NPC");mapFind(grid,&node,&npc);Check(node==*reinterpret_cast<void**>(grid+4),"inline lookup misses the same NPC");
 Check(!createHook(nullptr,nullptr,npc,0,0,0,1,0,0,1),"missing creator guarded without null dereference");
 loadHook(grid,nullptr,L"gdrs\\0198.gdt",41);Check(!find(grid,&record,npc,41,0),"wait for final native table");
 loadHook(grid,nullptr,L"gdrs\\0222.gdt",41);Check(U32(grid+8)==count*2&&U32(grid+0x18)==count,"native main and integrity tables contain every NPC");
 for(unsigned i=0;i<count;++i){auto* row=catalog+24+i*753;npc=U32(row);Check(find(grid,&record,npc,41,0)!=0,"native Find resolves NPC");mapFind(grid,&node,&npc);Check(node!=*reinterpret_cast<void**>(grid+4),"inline native map lookup resolves NPC");auto* value=*reinterpret_cast<Value**>(static_cast<unsigned char*>(node)+0x10);Check(value->type==41&&value->bytes==record&&U32(record+0x69)==U32(row+4)&&U32(record+0xe5)==U32(row+117+8),"inline creator obtains correct unit and HP");Check(createHook(nullptr,nullptr,npc,0,0,0,1,0,0,1)!=nullptr,"registered creator guard passes");}
 loadHook(grid,nullptr,L"gdrs\\0222.gdt",41);Check(U32(grid+8)==count*2&&U32(grid+0x18)==count,"registration idempotent");
 for(unsigned i=0;i<count;++i){auto* raw=units+12+i*4976;Check(find(grid,&record,U32(raw),2,0)&&Equal(record,raw,4976),"original unit bytes unchanged");}
 auto destroy=reinterpret_cast<void(__thiscall*)(void*)>(0x431d60);destroy(grid);Check(!allocations,"native destructor frees all owned records, wrappers, tree nodes and checksum entries");
 HeapFree(GetProcessHeap(),0,units);HeapFree(GetProcessHeap(),0,catalog);LocalFree(argv);
 Out("ALL NATIVE CLIENT MAP CHECKS PASSED: ");Number(count);Out(" units, ");Number(checks);Out(" checks; real addon, inline lookup, integrity registry and native cleanup\n");ExitProcess(0);
}
