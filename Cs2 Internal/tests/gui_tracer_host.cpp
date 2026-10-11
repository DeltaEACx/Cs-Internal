// Fresh isolated GUI host: real native DrawPage/request queue, WARP, no game hooks.
#define wmain queue_fixture_main
#include "gui_queue_regression.cpp"
#undef wmain
#include <deque>
#include <mutex>

namespace tracer_host {
using RequestFn=int32_t(__cdecl*)(void*,const uint8_t*,size_t,uint8_t*,size_t,size_t*);
using RunFn=int32_t(__cdecl*)(RequestFn,void*);
using ControlFn=void(__cdecl*)();
using RunningFn=int32_t(__cdecl*)();
int32_t __cdecl NativeRequest(void*,const uint8_t* bytes,size_t length,uint8_t* output,size_t capacity,size_t* written){
    try{
        const auto response=gui_bridge::Request(std::string(reinterpret_cast<const char*>(bytes),length));
        if(!output||!written||response.size()>capacity)return -1;
        std::memcpy(output,response.data(),response.size());*written=response.size();return 0;
    }catch(...){return -1;}
}
HWND Window(){
    HWND found{};
    EnumWindows(+[](HWND window,LPARAM context)->BOOL{
        DWORD process{};GetWindowThreadProcessId(window,&process);if(process!=GetCurrentProcessId())return TRUE;
        wchar_t title[128]{};GetWindowTextW(window,title,128);
        if(std::wstring_view(title)==L"DeltaEAC"){*reinterpret_cast<HWND*>(context)=window;return FALSE;}return TRUE;
    },reinterpret_cast<LPARAM>(&found));return found;
}
Json RawInput(){
    UINT count{};Check(GetRegisteredRawInputDevices(nullptr,&count,sizeof(RAWINPUTDEVICE))==0,"Raw-input count");
    std::vector<RAWINPUTDEVICE> devices(count);
    if(count)Check(GetRegisteredRawInputDevices(devices.data(),&count,sizeof(RAWINPUTDEVICE))!=UINT(-1),"Raw-input read");
    Json result=Json::array();for(const auto& device:devices)result.push_back({device.usUsagePage,device.usUsage,device.dwFlags,reinterpret_cast<uintptr_t>(device.hwndTarget)});
    return result;
}
void Emit(const Json& value){std::cout<<value.dump()<<std::endl;}
}
int wmain(int argc,wchar_t** argv){
    using namespace tracer_host;
    const ComScope com;
    try{
        Check(argc>1&&SUCCEEDED(com.value),"Probe DLL argument / COM");
        Fixture fixture;const auto baseline=RawInput();
        const auto module=LoadLibraryW(argv[1]);Check(module!=nullptr,"Probe DLL load");
        const auto run=reinterpret_cast<RunFn>(GetProcAddress(module,"velora_gui_run"));
        const auto stop=reinterpret_cast<ControlFn>(GetProcAddress(module,"velora_gui_stop"));
        const auto toggle=reinterpret_cast<ControlFn>(GetProcAddress(module,"velora_gui_toggle"));
        const auto running=reinterpret_cast<RunningFn>(GetProcAddress(module,"velora_gui_running"));
        Check(run&&stop&&toggle&&running,"Embedded GUI exports");
        Check(gui_bridge::Start(true),"Native request queue start");gui_bridge::Loading(5,"Opening GUI");
        std::atomic<bool> finished{};std::atomic<int> result{};
        std::thread ui([&]{result=run(NativeRequest,nullptr);finished=true;});
        std::mutex mutex;std::deque<Json> commands;std::atomic<bool> inputEnded{};
        std::thread reader([&]{std::string line;while(std::getline(std::cin,line)){
            try{auto request=Json::parse(line);std::lock_guard lock(mutex);commands.push_back(std::move(request));}catch(...){}
        }inputEnded=true;});
        const auto state=[&]{const auto window=Window();return Json{
            {"running",running()},{"finished",finished.load()},{"runResult",result.load()},
            {"window",reinterpret_cast<uintptr_t>(window)},{"visible",window&&IsWindowVisible(window)&&!IsIconic(window)},
            {"minimized",window&&IsIconic(window)},{"guiReady",gui_bridge::GuiReady()},
            {"sessionVisible",gui_bridge::MenuVisible()},{"rawInputPreserved",RawInput()==baseline}};};
        Emit({{"event","ready"},{"pid",GetCurrentProcessId()}});
        bool quit{};
        while(!quit){
            fixture.Process();MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            std::deque<Json> pending;{std::lock_guard lock(mutex);pending.swap(commands);}
            for(const auto& request:pending){const auto op=request.value("op",std::string{});
                if(op=="toggle")toggle();
                if(op=="close")if(const auto window=Window())PostMessageW(window,WM_CLOSE,0,0);
                if(op=="features_stage")gui_bridge::Loading(request.value("progress",50u),request.value("message",std::string{"Loading features"}).c_str());
                if(op=="features_ready")gui_bridge::FinishLoading();
                if(op=="stop"||op=="quit"){gui_bridge::Stop();stop();}
                if(op=="quit")quit=true;
                Emit({{"id",request.value("id",0)},{"state",state()}});
            }
            if(inputEnded.load()){gui_bridge::Stop();stop();quit=true;}
            Sleep(1);
        }
        ui.join();reader.join();Emit({{"event","finished"},{"state",state()}});
        return result.load()==0?0:1;
    }catch(const std::exception& error){Emit({{"error",error.what()}});return 1;}
}
