#include <cstdio>
#include <cmath>
#include "../mods/battle_ai/combat_math.h"
#include "../mods/battle_ai/defence_math.h"
#include "../mods/battle_ai/difficulty.h"
using namespace battle_ai;
int main() {
    int failures=0;
    auto check=[&](bool ok,const char* name) { std::printf("%s %s\n",ok?"PASS":"FAIL",name); if(!ok) ++failures; };
    check(RoomDifficulty(std::string_view("a full descriptive title !AI3 HARD !rand easy"))==2 &&
          RoomDifficulty(std::wstring_view(L"a full descriptive title !AI3 HARD !rand easy"))==2,
          "UTF-8 and wide title readers agree on difficulty beyond the native projection");
    check(RoomDifficulty(std::string_view("!ai !rand hard"))==-1 &&
          RoomDifficulty(std::wstring_view(L"!ai\tMEDIUM\n!rand hard"))==1,
          "difficulty stops at a command boundary and accepts native whitespace");
    check(RoomDifficulty(std::string_view("!aim hard room!ai3 hard !ai3x hard"))==-1 &&
          RoomDifficulty(std::wstring_view(L"!ai easy !ai hard"))==0,
          "invalid prefixes never enable difficulty and the first valid command wins");
    DefenceThreat rear[]={{{0,0,-1000},1}};
    auto front=DefensiveFacing({},{0,0,1},rear,1);
    check(front.valid&&front.direction.z<-0.99,"a rear threat selects a front-facing response");
    DefenceThreat crossfire[]={{{0,0,1000},1},{{0,0,-1000},1}};
    auto sides=DefensiveFacing({},{0,0,1},crossfire,2);
    check(sides.valid&&std::abs(sides.direction.x)>0.99&&sides.rearRisk<1.01,
          "opposing attackers choose side-on defence instead of a zero-vector or full rear exposure");
    auto reposition=DefensivePosition({},{0,0,1},crossfire,2);
    check(reposition.relocate&&reposition.facing.rearRisk<sides.rearRisk-0.08,
          "crossfire proposes a finite short relocation that reduces worst rear exposure");
    DefenceThreat uneven[]={{{0,0,1000},1},{{0,0,-1000},1000000}};
    auto protectedSides=DefensiveFacing({},{0,0,1},uneven,2);
    check(protectedSides.valid&&protectedSides.rearExposure==0&&std::abs(protectedSides.direction.x)>0.99,
          "moving defence never trades a weaker attacker's back exposure for threat weight");
    DefenceThreat narrow[]={{{0,0,1000},1},{{1,0,-1000},1}};
    auto tight=DefensiveFacing({},{0,0,-1},narrow,2);
    check(tight.valid&&tight.rearExposure==0,
          "exact threat boundaries find a safe facing even inside a sub-grid front hemisphere");
    check(std::abs(TurnStepDegrees({0,0,1},{0,0,-1},900,16))<=14.41&&
          std::abs(TurnStepDegrees({0,0,1},{0,0,-1},900,5000))<=15,
          "fast defensive turn remains visibly incremental even after a long stalled frame");
    Vec3 turning={0,0,1};double rotation=0;
    for(int i=0;i<15;++i) {float step=TurnStepDegrees(turning,{0,0,-1},900,16);rotation+=step*kPi/180;turning=Heading(rotation);}
    check(turning.z<-0.99,"incremental high-skill turn reaches a rear threat quickly without one-frame teleportation");
    check(!DefensiveFacing({},{0,0,1},nullptr,0).valid&&TurnStepDegrees({},{0,0,1},900,16)==0,
          "missing threats and invalid facing never invent a defensive turn");
    auto a=Aim({0,0,0},{1000,0,0},{0,0,100},500,5);
    check(a.intercept && std::abs(a.seconds-2.041241452)<1e-6 && std::abs(a.direction.z-0.2)<1e-5,
          "perpendicular target gets analytic intercept lead");
    a=Aim({0,0,0},{1000,0,0},{100,0,0},500,5);
    check(a.intercept && std::abs(a.seconds-2.5)<1e-6,"receding target intercept");
    a=Aim({0,0,0},{1000,0,0},{-100,0,0},500,5);
    check(a.intercept && std::abs(a.seconds-1000.0/600)<1e-6,"approaching target intercept");
    a=Aim({0,0,0},{1000,0,0},{500,0,0},500,5);
    check(!a.intercept && a.point.x==1000,"equal-speed unreachable target falls back without infinity");
    a=Aim({0,0,0},{1000,0,0},{700,0,0},500,5);
    check(!a.intercept && Finite(a.direction),"faster unreachable target has finite direct aim");
    a=Aim({0,0,0},{1000,0,0},{0,0,100},0,5);
    check(!a.intercept && a.direction.x==1 && a.direction.z==0,"instantaneous ray has no projectile lead");
    a=Aim({10,20,30},{10,20,30},{0,0,0},500,5);
    check(a.intercept && Length(a.direction)==0,"coincident origin and target has no NaN");
    a=Aim({0,0,0},{10000,0,0},{0,0,10},100,2);
    check(!a.intercept && a.point.z==0,"unreasonably long flight is not extrapolated");
    // Independent discrete trajectory oracle: the projectile travels at fixed
    // speed, target at fixed velocity. Measure closest physical separation.
    int hits=0,total=0;
    double worst=0;
    for(int side=-4;side<=4;++side) for(int up=-2;up<=2;++up) for(int distance=500;distance<=2500;distance+=500) {
        Vec3 origin={37,63,-19},target={static_cast<float>(distance),120,200};
        Vec3 velocity={static_cast<float>(side*45),static_cast<float>(up*30),static_cast<float>(side*35)};
        auto shot=Aim(origin,target,velocity,1800,4);
        double closest=1e20;
        for(int step=0;step<=960;++step) {
            float t=step/240.0f;
            Vec3 bullet=origin+shot.direction*(1800*t),moving=target+velocity*t;
            closest=(std::min)(closest,Length(bullet-moving));
        }
        ++total; if(closest<=8) ++hits; worst=(std::max)(worst,closest);
    }
    std::printf("Fixed-velocity ballistic benchmark: %d/%d physical intersections within radius 8; worst miss %.3f\n",hits,total,worst);
    check(hits*100>=total*90,"ballistic fixture exceeds 90 percent target without damage compensation");
    return failures?1:0;
}
