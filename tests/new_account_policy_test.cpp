#include <cstdio>
#include <string>
#include <stdexcept>
#include "../mods/new_account_profile/policy.h"
using namespace sdgo::starter;
struct State { int account=0,gp=0,cash=0,rank=0,step=0,xp=0,items=0,units=0;bool user=false; };
struct Fake final:Database {
 State state,saved;bool transaction=false;std::string fail;bool raceAccount=false,raceUser=false;
 bool Account(const char*,bool lock,int& id)override{if(fail=="read")return false;if(lock&&raceAccount){state.account=7;state.gp=314;state.cash=27;}id=state.account;return true;}
 bool User(int,bool lock,bool& exists)override{if(fail=="read")return false;if(lock&&raceUser){state.user=true;state.rank=9;state.gp=271;}exists=state.user;return true;}
 bool Begin()override{if(fail=="begin")return false;saved=state;transaction=true;return true;}
 bool Active()override{return transaction;}
 bool Commit()override{if(fail=="commit")return false;transaction=false;return true;}
 void Abort()override{state=saved;transaction=false;}
 bool Wallet(int,const Settings& s)override{if(fail=="wallet")return false;state.gp=s.gp;state.cash=s.cash;return true;}
 bool Rank(const Settings&,RankStart& r)override{r.experience=25700;return fail!="rank";}
 bool UnitSnapshot(int,int64_t& last)override{last=state.units;return fail!="unitread";}
 bool LimitUnits(int,int64_t last,const Settings& s)override{if(fail=="units")return false;state.units=static_cast<int>(last)+(state.units-last>s.unitLimit?s.unitLimit:state.units-static_cast<int>(last));return true;}
 bool Profile(int,const Settings& s,const RankStart& r)override{if(fail=="profile")return false;state.user=true;state.rank=s.rank;state.step=s.step;state.xp=r.experience;return true;}
};
int failures=0;
void Check(bool value,const char* label){printf("%s %s\n",value?"ok  ":"FAIL",label);if(!value)++failures;}
int Signup(Fake& db,LoginInput& input){db.state.account=7;db.state.gp=db.state.cash=9999999;input.result=1;return 1003;}
int Create(Fake& db,UserInput& input,UserOutput& out){db.state.user=true;db.state.rank=15;db.state.items=3;db.state.units+=100;input.result=0;input.tutorial=0;out.rank=5;return 0;}
int main(){
 Settings settings;LoginInput login={};UserInput input={};input.account=7;UserOutput output={};
 {
  Fake db;int result=Login(db,settings,login,[&](LoginInput& in){return Signup(db,in);});
  Check(result==1003&&login.result==1,"preserve native first-registration retry result");
  Check(db.state.gp==5000&&db.state.cash==0&&!db.transaction,"new signup commits small GP and zero cash");
 }
 {
  Fake db;db.state.account=7;db.state.gp=123456;db.state.cash=789;bool called=false;
  Login(db,settings,login,[&](LoginInput&){called=true;return 77;});
  Check(called&&db.state.gp==123456&&db.state.cash==789,"existing account delegates without balance resets");
 }
 {
  Fake db;db.raceAccount=true;Login(db,settings,login,[&](LoginInput&){return 0;});
  Check(db.state.gp==314&&db.state.cash==27,"concurrent signup winner is not initialized twice");
 }
 for(auto failure:{"read","begin","wallet","commit"}){
  Fake db;db.fail=failure;login={};
  int result=Login(db,settings,login,[&](LoginInput& in){return Signup(db,in);});
  Check(result==kDatabaseError&&login.result==4,"signup failure is reported as database error");
  Check(!db.state.account&&!db.state.gp&&!db.transaction,"signup failure leaves no partial account or funds");
 }
 {
  Fake db;db.state.account=7;db.state.gp=db.state.cash=9999999;
  bool patched=false;input.rank=0;input.step=0;output.experience=-123;
  int result=User(db,settings,input,output,[&](UserInput& in,UserOutput& out){patched=in.rank==5&&in.step==1;return Create(db,in,out);});
  Check(result==0&&patched&&input.rank==0&&input.step==0,"only staged creation inputs get the configured rank");
  Check(db.state.rank==5&&db.state.step==1&&db.state.xp==25700,"persist Corporal rank/step and matching start experience");
  Check(output.rank==5&&output.step==1&&output.experience==25700,"publish corrected native profile result after commit");
  Check(db.state.gp==5000&&db.state.cash==0&&db.state.items==3,"normalize first profile balance and preserve native starter grants");
  Check(db.state.units==1,"default first-profile unit gifts are limited to one");
 }
 {
  Fake db;db.state.account=7;db.state.user=true;db.state.rank=9;db.state.xp=100000;db.state.gp=12345;db.state.cash=87;
  User(db,settings,input,output,[&](UserInput&,UserOutput& out){db.state.xp+=100;out.experience=db.state.xp;out.rank=9;return 0;});
  Check(db.state.rank==9&&db.state.xp==100100&&db.state.gp==12345&&db.state.cash==87,"existing profile retains rank, balances and legitimate progression");
 }
 {
  Fake db;db.raceUser=true;
  User(db,settings,input,output,[&](UserInput&,UserOutput&){return 0;});
  Check(db.state.rank==9&&db.state.gp==271,"concurrent profile creator is preserved");
 }
 for(auto failure:{"read","begin","rank","profile","wallet","commit","unitread","units"}){
  Fake db;db.state.account=7;db.state.gp=81;db.state.cash=42;db.fail=failure;output.experience=-123;output.rank=19;
  int result=User(db,settings,input,output,[&](UserInput& in,UserOutput& out){return Create(db,in,out);});
  Check(result==kDatabaseError&&input.result==1,"profile failure is reported without a success result");
  Check(!db.state.user&&db.state.gp==81&&db.state.cash==42&&db.state.items==0&&db.state.units==0&&!db.transaction,"failed profile creation rolls back grants, rank and funds");
  Check(output.experience==-123&&output.rank==19,"failed creation leaves caller output unchanged");
 }
 {
  Fake db;db.fail="exception";try{User(db,settings,input,output,[&](UserInput&,UserOutput&)->int{db.state.user=true;if(db.fail=="exception")throw std::runtime_error("fixture");return 0;});}catch(const std::runtime_error&){}
  Check(!db.state.user&&!db.transaction,"exception unwinding rolls back incomplete native creation");
 }
 {
  Fake db;LoginInput in={};UserInput user={};UserOutput out={};Signup(db,in);Create(db,user,out);
  Check(db.state.gp==9999999&&db.state.cash==9999999&&db.state.rank==15,"without the policy native future-account defaults are restored");
 }
 {
  Fake db;db.state.account=7;db.state.units=4;
  User(db,settings,input,output,[&](UserInput& in,UserOutput& out){return Create(db,in,out);});
  Check(db.state.units==5,"pre-profile ownership is preserved while new bulk gifts are capped");
  db.state.units+=2;
  User(db,settings,input,output,[&](UserInput&,UserOutput&){return 0;});
  Check(db.state.units==7,"later earned and purchased units are never capped on login");
 }
 {
  Fake db;Settings custom=settings;custom.unitLimit=0;input={};input.account=7;
  User(db,custom,input,output,[&](UserInput& in,UserOutput& out){return Create(db,in,out);});
  Check(db.state.units==0,"owners can disable automatic profile unit gifts entirely");
 }
 printf("%s\n",failures?"STARTER POLICY CHECKS FAILED":"ALL STARTER POLICY CHECKS PASSED");return failures?1:0;
}
