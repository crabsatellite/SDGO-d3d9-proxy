#pragma once
#include <algorithm>
#include <cmath>
#include <limits>

namespace battle_ai {
struct Vec3 { float x=0, y=0, z=0; };
inline Vec3 operator+(Vec3 a,Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
inline Vec3 operator-(Vec3 a,Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
inline Vec3 operator*(Vec3 a,float n) { return {a.x*n,a.y*n,a.z*n}; }
inline double Dot(Vec3 a,Vec3 b) { return double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z; }
inline double Length(Vec3 a) { return std::sqrt(Dot(a,a)); }
inline bool Finite(Vec3 a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }
inline Vec3 Normalize(Vec3 a) {
    double n=Length(a);
    return n>1e-9 && std::isfinite(n) ? a*static_cast<float>(1/n) : Vec3{};
}
struct AimSolution { Vec3 point, direction; double seconds=0; bool intercept=false; };
// Constant-velocity intercept in world units/second. No hit/damage compensation.
// A zero speed represents an instantaneous ray; an unreachable intercept falls
// back to direct aim instead of inventing a negative flight time.
inline AimSolution Aim(Vec3 origin,Vec3 target,Vec3 velocity,double speed,double maxLeadSeconds) {
    AimSolution result={target,Normalize(target-origin),0,false};
    if(!Finite(origin)||!Finite(target)||!Finite(velocity)||!std::isfinite(speed)||
       speed<=0||!std::isfinite(maxLeadSeconds)||maxLeadSeconds<=0) return result;
    Vec3 r=target-origin;
    double a=Dot(velocity,velocity)-speed*speed,b=2*Dot(r,velocity),c=Dot(r,r);
    double time=std::numeric_limits<double>::infinity();
    if(c<1e-12) { result.intercept=true; return result; }
    if(std::abs(a)<1e-8*speed*speed) {
        if(std::abs(b)>1e-12) { double t=-c/b; if(t>0) time=t; }
    } else {
        double disc=b*b-4*a*c;
        if(disc>=0 && std::isfinite(disc)) {
            // Stable quadratic roots when b and sqrt(discriminant) nearly cancel.
            double q=-0.5*(b+std::copysign(std::sqrt(disc),b));
            double t1=q/a, t2=std::abs(q)>1e-12 ? c/q : -1;
            if(t1>0) time=t1;
            if(t2>0) time=(std::min)(time,t2);
        }
    }
    if(!std::isfinite(time)||time>maxLeadSeconds) return result;
    result.point=target+velocity*static_cast<float>(time);
    result.direction=Normalize(result.point-origin);
    result.seconds=time; result.intercept=Finite(result.point);
    return result;
}
}
