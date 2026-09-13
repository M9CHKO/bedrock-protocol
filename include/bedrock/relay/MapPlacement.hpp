#pragma once
#include <bedrock/relay/PlatformBuilder.hpp>
#include <array>
#include <cmath>
#include <limits>

namespace bedrock {
// Local placement only: this queue does not walk to collect a distant drop.
// Unknown world cells must never be interpreted as empty space.
struct MapPlacement {
    using Pos=platform::Pos;
    using Camera=PlatformBuilder::Camera;
    using World=PlatformBuilder::World;
    enum class Check { Ready, PlayerOverlap, OutOfRange, Unknown, Occupied, NoSupport, Occluded, Count };
    struct Search {
        std::optional<Pos> position;
        std::array<unsigned,size_t(Check::Count)> counts{};
        unsigned count(Check c)const{return counts[size_t(c)];}
    };
    static bool validCamera(Camera c){
        return c.known&&std::isfinite(c.x)&&std::isfinite(c.y)&&std::isfinite(c.z)&&
            std::abs(c.x)<=30000000&&std::abs(c.z)<=30000000&&c.y>=-64&&c.y<=1024;
    }
    static Check check(Camera c,Pos p,const World& world){
        if(!validCamera(c))return Check::Unknown;
        const double feet=c.y-1.62,dx=p.x+.5-c.x,dz=p.z+.5-c.z;
        // Include diagonals and fractional player positions, but keep the
        // dropped shulker nearby. Reserve the player's actual body, not merely
        // the block containing the camera (important on negative coordinates).
        if(p.x<c.x+.3&&p.x+1>c.x-.3&&p.z<c.z+.3&&p.z+1>c.z-.3&&
            p.y<feet+1.8&&p.y+1>feet+.01)return Check::PlayerOverlap;
        if(dx*dx+dz*dz>1.75*1.75)return Check::OutOfRange;
        for(double y:{double(p.y),double(p.y+1)})
            if(dx*dx+dz*dz+(y-c.y)*(y-c.y)>4.25*4.25)return Check::OutOfRange;
        const auto block=world(p),above=world({p.x,p.y+1,p.z}),floor=world({p.x,p.y-1,p.z});
        if((block.known&&!block.air)||(above.known&&!above.air))return Check::Occupied;
        if(floor.known&&!floor.safeFloor)return Check::NoSupport;
        if(!block.known||!above.known||!floor.known)return Check::Unknown;
        // Check both the placement face and the future opening face. Do not
        // pick an apparently free cell behind a chest/workbench or a wall.
        for(double y:{double(p.y),double(p.y+1)}){
            const double dy=y-c.y;
            const int steps=int(std::ceil(std::sqrt(dx*dx+dy*dy+dz*dz)*32));
            for(int i=1;i<steps;++i){const double t=double(i)/steps;
                const auto b=world({int(std::floor(c.x+dx*t)),int(std::floor(c.y+dy*t)),int(std::floor(c.z+dz*t))});
                if(!b.known)return Check::Unknown;
                if(!b.air)return Check::Occluded;
            }
        }
        return Check::Ready;
    }
    static Search find(Camera c,const World& world){
        Search result;
        if(!validCamera(c)){++result.counts[size_t(Check::Unknown)];return result;}
        const int px=int(std::floor(c.x)),py=int(std::floor(c.y-1.62+.01)),pz=int(std::floor(c.z));
        double best=std::numeric_limits<double>::max();
        for(int dy:{0,-1,1})for(int dx=-2;dx<=2;++dx)for(int dz=-2;dz<=2;++dz){
            const Pos p{px+dx,py+dy,pz+dz};const auto reason=check(c,p,world);++result.counts[size_t(reason)];
            if(reason!=Check::Ready)continue;
            const double x=p.x+.5-c.x,z=p.z+.5-c.z,score=x*x+z*z+dy*dy*4;
            if(score<best){best=score;result.position=p;}
        }
        return result;
    }
    static const char* message(Check c){
        switch(c){
        case Check::Unknown:return "Реле ещё не получило блоки рядом. Дождитесь загрузки; если не помогает, переподключитесь через реле";
        case Check::NoSupport:return "Не найдена подходящая опора для шалкера: нужен цельный блок, например кварц или светокамень";
        case Check::Occluded:return "Место для шалкера загорожено. Подойдите к свободной стороне площадки";
        default:return "Нет доступного места рядом: нужны свободный блок и воздух над ним, вне тела игрока";
        }
    }
    static const char* message(const Search& r){
        if(r.count(Check::Unknown))return message(Check::Unknown);
        if(r.count(Check::Occluded))return message(Check::Occluded);
        if(r.count(Check::NoSupport))return message(Check::NoSupport);
        return message(Check::Occupied);
    }
};
}
