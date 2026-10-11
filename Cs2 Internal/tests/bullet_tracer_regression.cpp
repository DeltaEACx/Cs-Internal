#include "features/visuals/detail/bullet_tracers.hpp"
#include <iostream>
#include <stdexcept>
#include <limits>
using namespace bullet_tracers;
void Check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
bool Near(float a,float b){return std::abs(a-b)<.01f;}
int main(){try{
    Store store;
    store.Shot({0,0,64},42,1000);
    Check(store.Impact({8,0,64},{100,0,64},42,1010,1.2f),"Valid local impact was rejected");
    auto lines=store.Items(1010);Check(lines.size()==1&&lines[0].from.x==0,"Shot-time origin was not preserved");
    Check(Near(Alpha(lines[0],1610,true),.5f)&&Alpha(lines[0],1610,false)==1,"Fade or persistent duration is incorrect");
    Check(store.Impact({8,0,64},{200,0,64},42,1300,1.2f)&&store.Items(1300)[1].from.x==8,
        "Expired shot origin did not fall back to the current muzzle");
    Check(store.Impact({12,0,64},{100,0,64},43,1300,1.2f)&&store.Items(1300).size()==1&&
        store.Items(1300)[0].from.x==12,"New pawn reused the previous actor's shot origin or tracers");
    Check(!store.Impact({0,0,64},{NAN,0,64},43,1301,1)&&!store.Impact({0,0,64},{0,0,64},43,1301,1),
        "Malformed or zero-length impact entered the draw queue");
    store.Clear();Check(store.Items(1300).empty(),"Session clear retained tracers");
    for(unsigned i=0;i<5000;++i)Check(store.Impact({0,0,64},{100+float(i%100),0,64},42,2000+i,5),"Bounded rapid-fire capture failed");
    Check(store.Items(7000).size()==128,"Rapid fire exceeded the tracer queue bound");
    Check(store.Items(12000).empty(),"Expired rapid-fire tracers were retained");
    Check(store.Impact({0,0,0},{100,0,0},42,13000,NAN)&&Near(store.Items(13000)[0].lifetime,1.2f),
        "Invalid duration did not use the safe default");
    Check(store.Items(12999).empty(),"Clock rollback retained a future tracer");
    const float matrix[4][4]{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,1,0}};
    const auto center=Project({-.5f,0,1},{.5f,0,1},matrix,1000,600);
    Check(center&&Near(center->from.x,250)&&Near(center->to.x,750)&&Near(center->from.y,300),
        "Perspective projection is incorrect");
    const auto crossing=Project({-.5f,0,-1},{.5f,0,1},matrix,1000,600);
    Check(crossing&&std::isfinite(crossing->from.x)&&crossing->from.x>=0&&crossing->from.x<=1000,
        "Near-plane crossing produced invalid or unbounded coordinates");
    Check(!Project({0,0,-1},{1,0,-2},matrix,1000,600),"Behind-camera tracer was projected");
    const auto clipped=Project({-10,0,1},{10,0,1},matrix,1000,600);
    Check(clipped&&Near(clipped->from.x,0)&&Near(clipped->to.x,1000),"Viewport clipping lost a crossing tracer");
    Check(!Project({10,0,1},{20,0,1},matrix,1000,600),"Offscreen tracer was submitted");
    float invalid[4][4]{};invalid[0][0]=NAN;
    Check(!Project({0,0,1},{1,0,1},invalid,1000,600)&&!Project({0,0,1},{1,0,1},matrix,0,600),
        "Invalid camera or screen dimensions were accepted");
    std::cout<<"PASS: shot origins, pawn/session resets, fade/duration, 5000 impacts, queue bounds, near-plane and viewport clipping\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
