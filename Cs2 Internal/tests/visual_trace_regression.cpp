// Exercise production gameplay traces with replaceable native objects and faults.
// This executable loads no game and installs no hooks.
#include "features/visuals/engine.hpp"
#include <iostream>
#include <stdexcept>
namespace fixture { std::uintptr_t Resolve(std::string_view); }
#undef PATTERN
#define PATTERN(expression) fixture::Resolve(expression)
#include "features/visuals/engine.cpp"

namespace fixture {
using Address=std::uintptr_t;
Address manager=0x123000,expected=manager,seen{};
unsigned filterCalls{},rayCalls{};
bool faultFilter{},faultRay{},replaceDuringRay{},invalidFraction{},missingOutput{},allSolid{},targetHit{};
bool abiCorrect=true,missingBindings{};
void __fastcall Filter(systems::tracing::filter* value,Address skip,std::uint64_t mask,int layer){
    ++filterCalls;abiCorrect&=skip==0x444000&&mask==0x1c3003&&layer==4;
    if(faultFilter)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    value->vtable=0x456000;
}
bool __fastcall Ray(Address object,systems::tracing::ray* ray,const math::vector3* start,
                    const math::vector3* end,const systems::tracing::filter* filter,systems::tracing::result* hit){
    ++rayCalls;seen=object;abiCorrect&=object==expected&&ray->type==0&&filter->vtable==0x456000;
    if(faultRay)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    if(missingOutput)return false;
    hit->start_pos=*start;hit->end_pos=*end;hit->fraction=invalidFraction?NAN:1.f;hit->all_solid=allSolid;
    if(targetHit){hit->hit_entity=0x777000;hit->fraction=.5f;}
    if(replaceDuringRay)manager+=0x1000;
    return false; // Native clear rays legitimately return false.
}
Address Resolve(std::string_view expression){
    if(missingBindings)return 0;
    if(expression==patterns::trace_ray)return reinterpret_cast<Address>(&Ray);
    if(expression==patterns::trace_filter_init)return reinterpret_cast<Address>(&Filter);
    return 0;
}
void Check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
bool Visible(){return systems::g_tracing.is_visible({0,0,64},{100,20,64},0x777000,0x444000);}
}
int main(){
    using namespace fixture;
    try{
        addresses::globals::game_trace_manager_slot=reinterpret_cast<Address>(&manager);
        Check(Visible()&&abiCorrect,"Clear ray / native ABI rejected");
        for(unsigned i=0;i<10000;++i){manager=expected=0x123000+i*0x1000;Check(Visible()&&seen==manager,"Stale manager retained");}
        manager=0;const auto calls=rayCalls;Check(!Visible()&&rayCalls==calls,"Null manager called native ray");
        manager=expected=0x123000;
        faultFilter=true;Check(!Visible(),"Filter access violation escaped");faultFilter=false;
        faultRay=true;Check(!Visible(),"Trace access violation escaped");faultRay=false;
        missingOutput=true;Check(!Visible(),"Unwritten ray result accepted");missingOutput=false;
        invalidFraction=true;Check(!Visible(),"Non-finite ray result accepted");invalidFraction=false;
        replaceDuringRay=true;Check(!Visible(),"Result from replaced manager accepted");replaceDuringRay=false;expected=manager;
        allSolid=true;Check(!Visible(),"All-solid ray marked visible");allSolid=false;
        targetHit=true;Check(Visible(),"Valid target hit rejected");targetHit=false;
        missingBindings=true;Check(!Visible(),"Unavailable functions invoked");missingBindings=false;
        Check(Visible()&&abiCorrect,"Trace did not recover after failed calls");
        std::cout<<"PASS: 10000 manager replacements, null/missing bindings, filter/trace access violations, malformed output, concurrent manager replacement and subsequent recovery\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
