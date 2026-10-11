// Actual native lifecycle wrapper, with a controllable Rust ABI substitute.
// This test never loads a game, renderer, hooks, or WebView runtime.
#include "platform/windows.h"
#include "ui/gui_embedded.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using RequestFn=int32_t(__cdecl*)(void*,const uint8_t*,size_t,uint8_t*,size_t,size_t*);
HANDLE entered{},activate{},finish{};
enum class Mode { Normal, Fail, Requests };
std::atomic<Mode> mode{Mode::Normal};
std::atomic<bool> active{},ignoreStop{},throwRequest{},requestsChecked{};
std::atomic<unsigned> runs{},stops{},toggles{};
void Check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void Await(const auto& condition,const char* message){
    const auto deadline=GetTickCount64()+2000;
    while(!condition()&&GetTickCount64()<deadline)Sleep(2);
    Check(condition(),message);
}
void Configure(Mode next,bool startImmediately=true){
    Check(gui_embedded::Stopped(),"Previous wrapper thread still exists");
    ResetEvent(entered);ResetEvent(activate);ResetEvent(finish);
    active=false;ignoreStop=false;throwRequest=false;requestsChecked=false;mode=next;stops=0;
    if(startImmediately)SetEvent(activate);
}
}

namespace gui_bridge {
std::string Request(const std::string& input){
    if(throwRequest)throw std::runtime_error("fixture request failure");
    return input;
}
}

extern "C" int32_t velora_gui_run(RequestFn request,void* context){
    ++runs;SetEvent(entered);WaitForSingleObject(activate,INFINITE);
    if(mode==Mode::Fail)return -3;
    active=true;
    if(mode==Mode::Requests){
        uint8_t output[64]{};size_t length=0;
        const auto* input=reinterpret_cast<const uint8_t*>("{\"op\":\"ping\"}");
        constexpr size_t size=13;
        bool passed=request(context,input,size,output,sizeof(output),&length)==0&&length==size&&!std::memcmp(input,output,size);
        passed=passed&&request(context,input,8u*1024u*1024u,output,sizeof(output),&length)==-1;
        passed=passed&&request(context,nullptr,size,output,sizeof(output),&length)==-1;
        output[1]=0xA5;
        passed=passed&&request(context,input,size,output,1,&length)==-2&&length==0&&output[1]==0xA5;
        throwRequest=true;
        passed=passed&&request(context,input,size,output,sizeof(output),&length)==-3&&length==0;
        throwRequest=false;requestsChecked=passed;
    }
    WaitForSingleObject(finish,INFINITE);active=false;return 0;
}
extern "C" void velora_gui_stop(){++stops;if(!ignoreStop)SetEvent(finish);}
extern "C" void velora_gui_toggle(){++toggles;}
extern "C" int32_t velora_gui_running(){return active?1:0;}
extern "C" size_t velora_gui_last_error(uint8_t* output,size_t capacity){
    constexpr char error[]="Expected fixture startup failure";
    if(output&&capacity){const auto count=(sizeof(error)-1)<(capacity-1)?sizeof(error)-1:capacity-1;std::memcpy(output,error,count);output[count]=0;}
    return sizeof(error)-1;
}

int main(){
    entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    activate=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    finish=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    try{
        Check(entered&&activate&&finish,"Fixture events");
        Check(gui_embedded::Stopped()&&gui_embedded::Stop(0),"Initially stopped");

        Configure(Mode::Normal,false);const auto before=runs.load();
        Check(gui_embedded::Start(),"Start before ACTIVE");
        Check(WaitForSingleObject(entered,2000)==WAIT_OBJECT_0,"Rust entry reached");
        Check(!gui_embedded::Running()&&!gui_embedded::Stopped(),"Pre-ACTIVE thread must remain owned");
        Check(gui_embedded::Start()&&runs==before+1,"Duplicate start must reuse pending thread");
        std::thread activation([]{Sleep(60);SetEvent(activate);});
        const bool earlyStopped=gui_embedded::Stop(1000);activation.join();
        Check(earlyStopped&&stops==1&&gui_embedded::Stopped(),"Stop before ACTIVE must latch and drain");

        Configure(Mode::Fail);Check(gui_embedded::Start(),"Failed-start thread creation");
        Await([]{return gui_embedded::Stopped();},"Failed startup must terminate its thread");
        Check(gui_embedded::Stop(0),"Failed startup handle cleanup");

        Configure(Mode::Normal,false);Check(gui_embedded::Start(),"Start delayed runtime");
        Check(!gui_embedded::Stop(20)&&!gui_embedded::Stopped(),"Pre-ACTIVE timeout retains handle");
        SetEvent(activate);Check(gui_embedded::Stop(1000),"Retry can stop retained pre-ACTIVE thread");

        Configure(Mode::Normal);ignoreStop=true;Check(gui_embedded::Start(),"Start unresponsive runtime");
        Await([]{return active.load();},"Runtime ACTIVE");
        const auto started=GetTickCount64();
        Check(!gui_embedded::Stop(30),"Unresponsive stop must report timeout");
        Check(GetTickCount64()-started<300&&!gui_embedded::Stopped(),"Timeout must be bounded and retain ownership");
        const auto pendingRuns=runs.load();Check(gui_embedded::Start()&&runs==pendingRuns,"Timeout must not orphan or duplicate thread");
        SetEvent(finish);Check(gui_embedded::Stop(1000)&&gui_embedded::Stopped(),"Later retry reclaims completed thread");

        Configure(Mode::Requests);Check(gui_embedded::Start(),"Restart after timeout");
        Await([]{return requestsChecked.load();},"Request adapter bounds, copy and exceptions");
        gui_embedded::Toggle();Check(toggles==1,"Toggle forwards to runtime");
        Check(gui_embedded::Stop(1000)&&!gui_embedded::Running()&&gui_embedded::Stopped(),"Normal final shutdown");
        Check(gui_embedded::Stop(0),"Repeated stop is harmless");
        CloseHandle(entered);CloseHandle(activate);CloseHandle(finish);
        std::cout<<"PASS embedded wrapper: early stop, duplicate start, failed startup, bounded timeout, retained handles, restart, request bounds/copy/exceptions\n";
        return 0;
    }catch(const std::exception& error){
        SetEvent(activate);SetEvent(finish);gui_embedded::Stop(2000);
        std::cerr<<"FAIL embedded wrapper: "<<error.what()<<'\n';return 1;
    }
}
