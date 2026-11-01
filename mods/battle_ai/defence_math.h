#pragma once
#include <cstddef>
#include "combat_math.h"

namespace battle_ai {
constexpr double kPi=3.14159265358979323846;
inline Vec3 Horizontal(Vec3 value) {value.y=0;return Normalize(value);}
inline double Bearing(Vec3 value) {return std::atan2(value.x,value.z);}
inline Vec3 Heading(double radians) {return {static_cast<float>(std::sin(radians)),0,static_cast<float>(std::cos(radians))};}
inline double WrapAngle(double value) {
    if(!std::isfinite(value)) return 0;
    value=std::fmod(value+kPi,2*kPi);
    if(value<0) value+=2*kPi;
    return value-kPi;
}
// The caller submits this bounded delta to a native, legally admitted turn.
// No frame can become a snap after a long pause or an invalid clock sample.
inline float TurnStepDegrees(Vec3 forward,Vec3 desired,double rate,double elapsedMs) {
    if(!Finite(forward)||!Finite(desired)||Length(Horizontal(forward))<0.5||
       Length(Horizontal(desired))<0.5||!std::isfinite(rate)||!std::isfinite(elapsedMs)||rate<=0||elapsedMs<=0) return 0;
    double delta=WrapAngle(Bearing(desired)-Bearing(forward))*180/kPi;
    double limit=(std::min)(15.0,rate*(std::min)(elapsedMs,50.0)/1000.0);
    return static_cast<float>((std::max)(-limit,(std::min)(limit,delta)));
}
struct DefenceThreat {Vec3 point;float weight=1;};
struct DefenceFacing {Vec3 direction;double rearRisk=0;bool valid=false;};
// Minimize the worst weighted rear exposure rather than averaging opposing
// enemies to a zero vector. With enemies on both sides, side-on is preferable
// to presenting the entire rear to either one; relocation can improve further.
inline DefenceFacing DefensiveFacing(Vec3 position,Vec3 current,const DefenceThreat* threats,std::size_t count) {
    DefenceFacing best;best.direction=Horizontal(current);
    if(!threats||!Finite(position)||count==0||count>128) return best;
    double bestScore=1e30;
    auto consider=[&](double angle) {
        Vec3 direction=Heading(angle);double worst=0,total=0;std::size_t usable=0;
        for(std::size_t i=0;i<count;++i) {
            if(!Finite(threats[i].point)||!std::isfinite(threats[i].weight)||threats[i].weight<=0) continue;
            Vec3 enemy=Horizontal(threats[i].point-position);if(Length(enemy)<0.5) continue;
            double risk=(1-Dot(direction,enemy))*threats[i].weight;
            worst=(std::max)(worst,risk);total+=risk;++usable;
        }
        if(!usable) return;
        double turn=std::abs(WrapAngle(angle-Bearing(current)));
        double score=worst+total/(usable*100.0)+turn*0.0001;
        if(score<bestScore) {bestScore=score;best={direction,worst,true};}
    };
    consider(Bearing(current));
    for(int i=0;i<144;++i) consider(i*(2*kPi/144));
    return best;
}
struct DefenceMove {Vec3 destination;DefenceFacing facing;bool relocate=false;};
inline DefenceMove DefensivePosition(Vec3 position,Vec3 forward,const DefenceThreat* threats,std::size_t count,float step=240) {
    DefenceMove best{position,DefensiveFacing(position,forward,threats,count),false};
    if(!best.facing.valid||!std::isfinite(step)||step<=0) return best;
    for(int i=0;i<12;++i) {
        Vec3 candidate=position+Heading(i*2*kPi/12)*step;
        auto facing=DefensiveFacing(candidate,forward,threats,count);
        if(!facing.valid||facing.rearRisk>best.facing.rearRisk-0.08) continue;
        bool tooClose=false;
        for(std::size_t k=0;k<count;++k) {
            if(!Finite(threats[k].point)) continue;
            double prior=Length(threats[k].point-position),next=Length(threats[k].point-candidate);
            if(next<180&&next<prior) tooClose=true;
        }
        if(!tooClose) best={candidate,facing,true};
    }
    return best;
}
}
