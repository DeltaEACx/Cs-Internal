#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace bullet_tracers {
struct Point {float x{},y{},z{};};
struct Line {Point from{},to{};std::uint64_t born{};float lifetime{1.2f};};
inline bool Valid(Point p){
    return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&
        std::abs(p.x)<=131072&&std::abs(p.y)<=131072&&std::abs(p.z)<=131072;
}
inline float Alpha(const Line& line,std::uint64_t now,bool fade){
    if(now<line.born||!std::isfinite(line.lifetime)||line.lifetime<=0)return 0;
    const float age=static_cast<float>(now-line.born)/(line.lifetime*1000);
    return age>=1?0:fade?1-age:1;
}
class Store {
    std::array<Line,128> lines_{};
    std::size_t count_{};
    Point origin_{};
    std::uint32_t owner_{};
    std::uint64_t shotTime_{};
    bool shot_{};
public:
    void Clear(){*this={};}
    void Shot(Point origin,std::uint32_t owner,std::uint64_t now){
        if(!Valid(origin)||!owner||owner==0xffffffffu)return;
        if(owner_!=owner)Clear();
        origin_=origin;owner_=owner;shotTime_=now;shot_=true;
    }
    bool Impact(Point fallback,Point end,std::uint32_t owner,std::uint64_t now,float lifetime){
        if(!Valid(fallback)||!Valid(end)||!owner||owner==0xffffffffu)return false;
        if(owner_!=owner){Clear();owner_=owner;}
        const auto start=shot_&&now>=shotTime_&&now-shotTime_<=250?origin_:fallback;
        const double length=std::hypot(double(end.x)-start.x,double(end.y)-start.y,double(end.z)-start.z);
        if(!std::isfinite(length)||length<1)return false;
        Items(now);
        if(count_==lines_.size()){std::move(lines_.begin()+1,lines_.end(),lines_.begin());--count_;}
        lines_[count_++]={start,end,now,std::isfinite(lifetime)?std::clamp(lifetime,.1f,5.f):1.2f};
        return true;
    }
    std::span<const Line> Items(std::uint64_t now){
        std::size_t retained{};
        for(std::size_t i=0;i<count_;++i)if(Alpha(lines_[i],now,false)>0)lines_[retained++]=lines_[i];
        count_=retained;return {lines_.data(),count_};
    }
};
struct ScreenPoint {float x{},y{};};
struct ScreenLine {ScreenPoint from{},to{};};
// Clip the segment before perspective division. This includes the near plane,
// so a muzzle behind the camera cannot produce unbounded draw coordinates.
inline std::optional<ScreenLine> Project(Point from,Point to,const float (&matrix)[4][4],float width,float height){
    if(!Valid(from)||!Valid(to)||!std::isfinite(width)||!std::isfinite(height)||width<=0||height<=0)return {};
    for(const auto& row:matrix)for(float v:row)if(!std::isfinite(v))return {};
    const auto transform=[&](Point p){
        std::array<double,3> clip{};
        for(unsigned i=0;i<3;++i){const unsigned r=i==2?3:i;
            clip[i]=double(matrix[r][0])*p.x+double(matrix[r][1])*p.y+double(matrix[r][2])*p.z+matrix[r][3];}
        return clip;
    };
    const auto a=transform(from),b=transform(to);double begin=0,end=1;
    const auto plane=[](const auto& p,unsigned i){
        if(i==0)return p[2]-.01;
        const auto axis=(i-1)/2;return p[2]+(i%2?p[axis]:-p[axis]);
    };
    for(unsigned i=0;i<5;++i){const double x=plane(a,i),y=plane(b,i);
        if(x<0&&y<0)return {};
        if((x<0)!=(y<0)){const double fraction=x/(x-y);
            if(x<0)begin=std::max(begin,fraction);else end=std::min(end,fraction);}
        if(begin>end)return {};
    }
    const auto screen=[&](double t){
        const double w=a[2]+(b[2]-a[2])*t;
        return ScreenPoint{static_cast<float>(std::clamp((1+(a[0]+(b[0]-a[0])*t)/w)*width*.5,0.,double(width))),
            static_cast<float>(std::clamp((1-(a[1]+(b[1]-a[1])*t)/w)*height*.5,0.,double(height)))};
    };
    const ScreenLine line{screen(begin),screen(end)};
    if(!std::isfinite(line.from.x)||!std::isfinite(line.from.y)||!std::isfinite(line.to.x)||!std::isfinite(line.to.y))return {};
    return line;
}
}
