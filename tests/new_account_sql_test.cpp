#define DBINITCONSTANTS
#include <windows.h>
#include <msdasc.h>
#include <oledb.h>
#include <cstdio>
#include <cstring>
#include <array>
#include <string>
#include "sdgo/sdgo_api.h"
#include "../mods/new_account_profile/oledb_session.h"
using namespace sdgo::starter;
namespace {
int failed=0;bool failProfile=false,failWallet=false;std::wstring billing;
struct NativeConnection { BYTE unused[8]={};ICommandText* command=nullptr;BYTE padding[0x240-12]={};int state=0; };
static_assert(offsetof(NativeConnection,command)==8&&offsetof(NativeConnection,state)==0x240);
struct Session {
 Com<IDBInitialize> source;Com<IDBCreateCommand> factory;Com<ICommandText> command;NativeConnection native;
 bool Open(const wchar_t* name){
  wchar_t value[4096]={};if(!GetEnvironmentVariableW(name,value,4096))return false;
  Com<IDataInitialize> initialize;
  if(FAILED(CoCreateInstance(CLSID_MSDAINITIALIZE,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IDataInitialize),reinterpret_cast<void**>(initialize.put()))))return false;
  if(FAILED(initialize->GetDataSource(nullptr,CLSCTX_INPROC_SERVER,value,__uuidof(IDBInitialize),reinterpret_cast<IUnknown**>(source.put())))||FAILED(source->Initialize()))return false;
  Com<IDBCreateSession> sessions;if(FAILED(source->QueryInterface(__uuidof(IDBCreateSession),reinterpret_cast<void**>(sessions.put()))))return false;
  if(FAILED(sessions->CreateSession(nullptr,__uuidof(IDBCreateCommand),reinterpret_cast<IUnknown**>(factory.put()))))return false;
  if(FAILED(factory->CreateCommand(nullptr,__uuidof(ICommandText),reinterpret_cast<IUnknown**>(command.put()))))return false;
  native.command=command.get();return true;
 }
};
void Check(bool ok,const char* text){printf("%s %s\n",ok?"ok  ":"FAIL",text);if(!ok)++failed;}
std::wstring Quote(const char* value){std::wstring s=L"'";for(;*value;++value){s+=static_cast<unsigned char>(*value);if(*value=='\'')s+=L'\'';}return s+L'\'';}
bool Execute(NativeConnection* c,const std::wstring& sql){DBROWCOUNT n=0;return SUCCEEDED(c->command->SetCommandText(DBGUID_DBSQL,sql.c_str()))&&SUCCEEDED(c->command->Execute(nullptr,IID_NULL,nullptr,&n,nullptr));}
struct Native {
 __declspec(noinline) int Login(void* conn,LoginInput* input,void*){
  std::wstring sql=failWallet?L"INSERT dbo.tbl_Account(f_loginId) VALUES("+Quote(input->id)+L")":L"EXEC dbo.sp_TestRegister "+Quote(input->id);
  if(!Execute(static_cast<NativeConnection*>(conn),sql)){input->result=4;return 1000;}
  input->result=1;return 1003;
 }
 __declspec(noinline) int Profile(void* conn,UserInput* input,UserOutput* out){
  auto sql=L"EXEC dbo.sp_TestCreateProfile "+std::to_wstring(input->account);
  if(!Execute(static_cast<NativeConnection*>(conn),sql)){input->result=1;return 1000;}
  out->experience=0;out->rank=5;out->step=0;input->tutorial=0;input->result=failProfile?1:0;return failProfile?1000:0;
 }
};
uint32_t layouts[]={0x49,0x3b,8};
std::wstring Root(){wchar_t p[32768];GetModuleFileNameW(nullptr,p,32768);std::wstring s=p;return s.substr(0,s.find_last_of(L"\\/"));}
std::wstring Stem(){wchar_t p[32768];GetModuleFileNameW(nullptr,p,32768);std::wstring s=p;s=s.substr(s.find_last_of(L"\\/")+1);return s.substr(0,s.find_last_of(L'.'));}
void Write(const std::wstring& path,const std::string& text){HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);DWORD n=0;Check(f!=INVALID_HANDLE_VALUE&&WriteFile(f,text.data(),static_cast<DWORD>(text.size()),&n,nullptr)&&n==text.size(),"write fixture metadata");if(f!=INVALID_HANDLE_VALUE)CloseHandle(f);}
template<class F> uintptr_t Member(F f){uintptr_t value=0;static_assert(sizeof(f)==sizeof(value));memcpy(&value,&f,sizeof(value));return value;}
std::string NarrowDatabase(const std::wstring& value){std::string result;for(wchar_t c:value)result+=static_cast<char>(c);return result;}
void Symbols(const std::wstring& root){
 auto* base=reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));auto* nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
 char key[128],header[256];auto stem=Stem();std::string name;for(wchar_t c:stem)name+=static_cast<char>(c);
 sprintf_s(key,"%s-%08X-%08X",name.c_str(),nt->FileHeader.TimeDateStamp,nt->OptionalHeader.SizeOfImage);
 sprintf_s(header,"[host]\ntimestamp=0x%08X\nsize_of_image=0x%08X\n",nt->FileHeader.TimeDateStamp,nt->OptionalHeader.SizeOfImage);std::string result=header;
 auto add=[&](const char* id,uintptr_t address,const char* kind,unsigned size){char line[256];sprintf_s(line,"\n[%s]\nkind=%s\nrva=0x%08X\nexpect=",id,kind,static_cast<unsigned>(address-reinterpret_cast<uintptr_t>(base)));result+=line;for(unsigned i=0;i<size;++i){sprintf_s(line,"%02X ",reinterpret_cast<BYTE*>(address)[i]);result+=line;}result+='\n';};
 add("starter.login",Member(&Native::Login),"code",12);add("starter.profile",Member(&Native::Profile),"code",12);
 add("starter.login_layout",reinterpret_cast<uintptr_t>(&layouts[0]),"data",4);add("starter.profile_layout",reinterpret_cast<uintptr_t>(&layouts[1]),"data",4);add("starter.connection_command",reinterpret_cast<uintptr_t>(&layouts[2]),"data",4);
 std::wstring wkey(key,key+strlen(key));Write(root+L"\\sdgo\\symbols\\"+wkey+L".ini",result);
}
int Account(Session& s,const char* name){OleDatabase d(billing);int value=-1;Check(d.Open(&s.native)&&d.Account(name,false,value),"read account through real native OLE DB session");return value;}
int Run(bool enabled,bool configured){
 wchar_t bill[64];if(!GetEnvironmentVariableW(L"SDGO_STARTER_BILLING_NAME",bill,64))return 2;billing=bill;
 Session common,game;Check(common.Open(L"SDGO_STARTER_COMMON"),"open isolated common provider");Check(game.Open(L"SDGO_STARTER_GAME"),"open isolated game provider");if(failed)return 2;
 const auto root=Root();Symbols(root);Write(root+L"\\"+Stem()+L".ini","[DB StoreBilling]\nDBName="+NarrowDatabase(billing)+"\n");
 HMODULE core=LoadLibraryW((root+L"\\sdgo\\core.dll").c_str());auto init=core?reinterpret_cast<SdgoCoreInitFn>(GetProcAddress(core,"SdgoCoreInit")):nullptr;
 Check(init!=nullptr,"actual core export exists");if(!init)return 2;SdgoBoot boot={sizeof(boot),"starter_sql_test",GetModuleHandleW(nullptr)};Check(init(&boot)!=0,"load actual new-account Mod through core");
 Native native;LoginInput login={};strcpy_s(login.id,"starter-new");strcpy_s(login.password,"fixture-only");
 int code=native.Login(&common.native,&login,nullptr);Check(code==1003&&login.result==1,"native registration retry protocol preserved");int id=Account(common,login.id);Check(id>0,"native account created");
 const int gp=enabled?(configured?1200:5000):9999999;
 const int cash=enabled?0:9999999;
 const auto walletCheck=L"IF NOT EXISTS(SELECT 1 FROM "+Identifier(billing)+L".dbo.tbls_AccountPocket WHERE f_idx="+std::to_wstring(id)+L" AND f_GP="+std::to_wstring(gp)+L" AND f_GC="+std::to_wstring(cash)+L") RAISERROR('Unexpected starter balance',16,1)";
 Check(Execute(&common.native,walletCheck),"registration wallet is correct before profile creation");
 UserInput input={};input.account=id;strcpy_s(input.pilot,"Fixture");UserOutput out={};out.experience=-77;
 code=native.Profile(&game.native,&input,&out);Check(code==0,"native first profile succeeds");
 Check(enabled?(out.experience==(configured?32000:25700)&&out.rank==(configured?6:5)&&out.step==(configured?2:1)):(out.experience==0&&out.rank==5&&out.step==0),"caller sees configured rank and matching experience only when enabled");
 Check(Execute(&game.native,walletCheck),"first-profile wallet is consistent with registration");
 const int unitCount=enabled?(configured?3:1):100;
 Check(Execute(&game.native,L"IF (SELECT COUNT(*) FROM dbo.tbl_UserUnits WHERE AccountNumber="+std::to_wstring(id)+L")<>"+std::to_wstring(unitCount)+L" OR NOT EXISTS(SELECT 1 FROM dbo.tbl_Users u WHERE AccountNumber="+std::to_wstring(id)+L" AND UserUnitTotalCnt="+std::to_wstring(unitCount)+L" AND UserUnitKindCnt="+std::to_wstring(unitCount)+L" AND EXISTS(SELECT 1 FROM dbo.tbl_UserUnits n WHERE n.AccountNumber=u.AccountNumber AND n.UserUnitNumber=u.GoUserUnitNum)) RAISERROR('Unexpected starter units or dangling selection',16,1)"),"bulk gifts are limited and unit counts/selection remain consistent");
 OleDatabase live(billing);bool exists=false;Check(live.Open(&game.native)&&live.User(id,false,exists)&&exists,"profile persisted in real SQL");
 if(enabled){
  NativeConnection unavailable;
  LoginInput unavailableLogin={};strcpy_s(unavailableLogin.id,"no-session");
  code=native.Login(&unavailable,&unavailableLogin,nullptr);
  Check(code==1000&&unavailableLogin.result==4&&Account(common,unavailableLogin.id)==0,"unavailable session fails before native registration");
  UserInput unavailableUser={};unavailableUser.account=id;UserOutput unavailableOutput={};unavailableOutput.experience=-99;
  code=native.Profile(&unavailable,&unavailableUser,&unavailableOutput);
  Check(code==1000&&unavailableUser.result==1&&unavailableOutput.experience==-99,"unavailable session fails without publishing a profile");
  LoginInput bad={};strcpy_s(bad.id,"fail-wallet");failWallet=true;code=native.Login(&common.native,&bad,nullptr);failWallet=false;
  Check(code==1000&&bad.result==4&&Account(common,bad.id)==0,"failed wallet policy rolls back the whole native account");
  LoginInput other={};strcpy_s(other.id,"fail-profile");native.Login(&common.native,&other,nullptr);int otherId=Account(common,other.id);
  UserInput broken={};broken.account=otherId;UserOutput untouched={};untouched.experience=-88;failProfile=true;code=native.Profile(&game.native,&broken,&untouched);failProfile=false;
  exists=true;Check(live.User(otherId,false,exists)&&!exists&&code==1000&&untouched.experience==-88,"native profile error rolls back creation/grants before publishing output");
  Check(Execute(&game.native,L"IF EXISTS(SELECT 1 FROM dbo.tbl_UserUnits WHERE AccountNumber="+std::to_wstring(otherId)+L") RAISERROR('Unit gifts escaped rollback',16,1)"),"failed native profile also rolls back its bulk unit gifts");
  LoginInput quoted={};strcpy_s(quoted.id,"quote'x");native.Login(&common.native,&quoted,nullptr);Check(Account(common,quoted.id)>0,"username lookup uses native-style bound parameters");
  LoginInput owned={};strcpy_s(owned.id,"pre-owned");native.Login(&common.native,&owned,nullptr);int ownedId=Account(common,owned.id);
  Check(Execute(&game.native,L"INSERT dbo.tbl_UserUnits(AccountNumber,UserUnitSlotNumber,UnitNumber) VALUES("+std::to_wstring(ownedId)+L",201,77777)"),"seed legitimately owned unit before first profile");
  UserInput ownedInput={};ownedInput.account=ownedId;UserOutput ownedOutput={};
  Check(native.Profile(&game.native,&ownedInput,&ownedOutput)==0&&Execute(&game.native,L"IF NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnits WHERE AccountNumber="+std::to_wstring(ownedId)+L" AND UnitNumber=77777 AND UserUnitNumber>2147483647) OR (SELECT COUNT(*) FROM dbo.tbl_UserUnits WHERE AccountNumber="+std::to_wstring(ownedId)+L")<>"+std::to_wstring(unitCount+1)+L" RAISERROR('Pre-owned unit lost or 64-bit boundary truncated',16,1)"),"pre-profile units and 64-bit identities survive bulk-gift normalization");
  Check(Execute(&game.native,L"INSERT dbo.tbl_UserUnits(AccountNumber,UserUnitSlotNumber,UnitNumber) VALUES("+std::to_wstring(id)+L",202,88888)"),"simulate a later legitimately acquired unit");
  Check(Execute(&game.native,L"UPDATE dbo.tbl_Users SET UserLevelNumber=6,UserExp=30000 WHERE AccountNumber="+std::to_wstring(id)+L";UPDATE "+Identifier(billing)+L".dbo.tbls_AccountPocket SET f_GP=7000,f_GC=321 WHERE f_idx="+std::to_wstring(id)),"simulate legitimate later progression");
  native.Login(&common.native,&login,nullptr);native.Profile(&game.native,&input,&out);
  Check(out.experience==0&&out.rank==5&&out.step==0,"existing profile preserves native response instead of reinitializing");
  Check(Execute(&game.native,L"IF NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnits WHERE AccountNumber="+std::to_wstring(id)+L" AND UnitNumber=88888) RAISERROR('Earned unit reset on login',16,1)"),"later earned units survive repeat login");
 }
 return failed?1:0;
}
}
int wmain(int argc,wchar_t** argv){setvbuf(stdout,nullptr,_IONBF,0);const bool enabled=argc<2||wcscmp(argv[1],L"disabled");const bool configured=argc>1&&!wcscmp(argv[1],L"configured");HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(hr))return 3;int result=Run(enabled,configured);CoUninitialize();puts(result?"STARTER SQL CHECKS FAILED":"ALL STARTER SQL CHECKS PASSED");return result;}
