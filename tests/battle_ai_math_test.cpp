#include <cstdio>
#include <cmath>
#include "../mods/battle_ai/combat_math.h"
using namespace battle_ai;
int main() {
    int failures=0;
    auto check=[&](bool ok,const char* name) { std::printf("%s %s\n",ok?"PASS":"FAIL",name); if(!ok) ++failures; };
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
