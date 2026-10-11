// Direct native request-queue regression. Own WARP fixture only; no game hooks
// or Tauri runtime. Only the unused UI-host wrapper is stubbed, never the queue.
#include "support/render_capture.h"
#ifdef small
#undef small
#endif
#include "ui/overlay.cpp"
#include "ui/gui_bridge.h"
#include "features/visuals/effects/config.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

namespace {std::atomic<unsigned> embeddedCalls{};}
namespace gui_embedded {
bool Start(){++embeddedCalls;return false;}
bool Stop(unsigned long){++embeddedCalls;return true;}
void Toggle(){++embeddedCalls;}
bool Running(){++embeddedCalls;return false;}
bool Stopped(){++embeddedCalls;return true;}
}

namespace {
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
struct ComScope {HRESULT value=CoInitializeEx(nullptr,COINIT_MULTITHREADED);~ComScope(){if(SUCCEEDED(value))CoUninitialize();}};
struct Fixture {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ImGuiContext* base{};
    Fixture(){
        D3D_FEATURE_LEVEL level{};
        Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context)),"WARP device");
        base=ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;io.DisplaySize={1280,900};
        unsigned char* pixels{};int width{},height{};io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
        g_device=device.Get();g_deviceContext=context.Get();g_nextSchemaRead=std::numeric_limits<ULONGLONG>::max();
    }
    ~Fixture(){gui_bridge::Stop();gui_bridge::DestroyCapture();catalog_ui::Shutdown();agent_preview::Shutdown();g_device=nullptr;g_deviceContext=nullptr;ImGui::DestroyContext(base);}
    void Process(){gui_bridge::Process(DrawPage,device.Get(),context.Get());}
    Json Immediate(Json request){return gui_bridge::Execute(request,DrawPage,device.Get(),context.Get());}
    Json Pump(std::future<Json>& response){
        const auto deadline=Clock::now()+5s;
        while(response.wait_for(0ms)!=std::future_status::ready&&Clock::now()<deadline){Process();std::this_thread::sleep_for(1ms);}
        Check(response.wait_for(0ms)==std::future_status::ready,"Request did not finish while render queue was pumped");return response.get();
    }
};
Json Request(Json request){return Json::parse(gui_bridge::Request(request.dump()));}
std::future<Json> Async(Json request){return std::async(std::launch::async,[request=std::move(request)]{return Request(request);});}
void Ok(const Json& response){if(!response.value("ok",false))throw std::runtime_error("Request rejected: "+response.dump());}
const Json* Find(const Json& value,const char* label){
    if(value.is_object()){
        if(value.value("type",std::string{})=="bool"&&value.value("label",std::string{})==label)return &value;
        for(const auto& item:value.items())if(const auto* found=Find(item.value(),label))return found;
    }else if(value.is_array())for(const auto& item:value)if(const auto* found=Find(item,label))return found;
    return nullptr;
}
Json Toggle(const Json& state){const auto* node=Find(state,"Hide default crosshair");Check(node!=nullptr,"Production viewmodel control missing");return *node;}
long long Milliseconds(Clock::time_point start){return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count();}
const Json* FindTyped(const Json& value,const char* label,const char* type){
    if(value.is_object()){
        if(value.value("type",std::string{})==type&&value.value("label",std::string{})==label)return &value;
        for(const auto& item:value.items())if(const auto* found=FindTyped(item.value(),label,type))return found;
    }else if(value.is_array())for(const auto& item:value)if(const auto* found=FindTyped(item,label,type))return found;
    return nullptr;
}
bool HasReadOnlyText(const Json& value){
    if(value.is_object()){
        if(value.value("type",std::string{})=="text"&&value.value("disabled",false))return true;
        for(const auto& item:value.items())if(HasReadOnlyText(item.value()))return true;
    }else if(value.is_array())for(const auto& item:value)if(HasReadOnlyText(item))return true;
    return false;
}
void NavigationAndEnableSettings(Fixture& fixture){
    const auto read=[&](int page){auto pending=Async({{"op","state"},{"page",page}});auto reply=fixture.Pump(pending);Ok(reply);return reply;};
    const auto set=[&](int page,const Json& control,bool value){
        auto pending=Async({{"op","set"},{"page",page},{"id",control.at("id")},{"value",value}});
        auto reply=fixture.Pump(pending);Ok(reply);return reply;
    };
    const auto state=read(23);
    const Json groups=Json::array({
        {{"id",0},{"label","Combat"}},{{"id",1},{"label","Skins"}},
        {{"id",2},{"label","Players"}},{{"id",3},{"label","Effects"}},
        {{"id",4},{"label","World"}},{{"id",5},{"label","View"}},
        {{"id",6},{"label","Local"}},{{"id",7},{"label","Movement"}}
    });
    Check(state.at("groups")==groups,"Native navigation group names or stable IDs changed");
    struct Page {int id,group;const char* label;};
    constexpr Page pages[]{
        {23,0,"Rage"},{24,0,"Anti Aim"},{25,7,"Assists"},
        {6,2,"ESP"},{9,2,"Chams"},{22,2,"Models"},{28,6,"Player"},
        {0,1,"Weapons"},{1,1,"Knives"},{2,1,"Gloves"},{3,1,"Agents"},{4,1,"Custom Agents"},
        {7,4,"Dropped items"},{20,4,"Throwables"},{12,4,"Sky"},{13,4,"Lighting"},{14,4,"Atmosphere"},
        {15,4,"Particles"},{17,4,"Physics"},
        {30,3,"Weapon Camo"},{10,3,"Particles"},{11,3,"Impact Effects"},{21,3,"Images"},
        {5,5,"Camera"},{16,5,"Crosshair"},{8,5,"Widgets"}
    };
    Json expected=Json::array();
    for(const auto& page:pages)expected.push_back({{"id",page.id},{"group",page.group},{"label",page.label}});
    Check(state.at("pages")==expected,"Native navigation order, group assignment or visible labels differ from the intended hierarchy");
    for(const auto& page:state.at("pages"))Check(page.at("id")!=29,"Unavailable Legit page appeared in navigation");
    for(int page:{23,24,25})Check(!HasReadOnlyText(read(page).at("page")),"Combat or movement diagnostics leaked into product controls");
    struct Feature {int page;const char* master;const char* setting;};
    constexpr Feature features[]{
        {23,"Enable ragebot","Automatic fire"},
        {25,"Enable movement assists","Directional steering"}
    };
    for(const auto& feature:features){
        const auto original=read(feature.page);
        const auto* masterNode=Find(original,feature.master);
        const auto* settingNode=Find(original,feature.setting);
        Check(masterNode&&settingNode,"Feature enable switch or editable subordinate setting missing");
        const Json master=*masterNode,setting=*settingNode;
        const auto disabled=set(feature.page,master,false);
        const auto* committed=Find(disabled,feature.master);
        Check(committed&&!committed->at("value").get<bool>(),"Disabling a feature was undone by its native menu draw");
        for(int poll=0;poll<4;++poll){
            const auto current=read(feature.page);const auto* enabled=Find(current,feature.master);
            Check(enabled&&!enabled->at("value").get<bool>(),"A state poll silently re-enabled a disabled feature");
        }
        const auto changed=set(feature.page,setting,!setting.at("value").get<bool>());
        const auto* enabled=Find(changed,feature.master);const auto* edited=Find(changed,feature.setting);
        Check(enabled&&!enabled->at("value").get<bool>(),"Editing a feature setting silently re-enabled the feature");
        Check(edited&&edited->at("value")==!setting.at("value").get<bool>(),"Subordinate setting edit was not retained while the feature was disabled");
        set(feature.page,setting,setting.at("value").get<bool>());
        set(feature.page,master,master.at("value").get<bool>());
        const auto restored=read(feature.page);
        const auto* restoredMaster=Find(restored,feature.master);const auto* restoredSetting=Find(restored,feature.setting);
        Check(restoredMaster&&restoredSetting&&restoredMaster->at("value")==master.at("value")&&
              restoredSetting->at("value")==setting.at("value"),"Feature test did not restore original settings");
    }
}
void TracerSettings(Fixture& fixture){
    const auto original=custom::get_configuration();
    const auto state=fixture.Immediate({{"op","state"},{"page",11}});Ok(state);
    const auto* popup=FindTyped(state,"Bullet Tracers","popup");
    const auto* toggle=FindTyped(state,"Bullet Tracers","bool");
    Check(popup&&toggle,"Bullet Tracers did not expose its independent toggle and three-dot popup");
    const auto set=[&](const Json& node,Json value){
        auto pending=Async({{"op","set"},{"page",11},{"id",node.at("id")},{"value",value}});
        const auto reply=fixture.Pump(pending);Ok(reply);
        const auto* read=FindTyped(reply,node.at("label").get<std::string>().c_str(),node.at("type").get<std::string>().c_str());
        Check(read&&read->at("value")==value,"Tracer appearance failed native read-set-read");
    };
    set(*toggle,true);
    for(const auto& setting:std::array<std::pair<const char*,const char*>,3>{{{"Width","number"},{"Duration","number"},{"Fade","bool"}}})
        Check(FindTyped(*popup,setting.first,setting.second)!=nullptr,"Tracer options were incomplete");
    set(*FindTyped(*popup,"Width","number"),4.5f);set(*FindTyped(*popup,"Duration","number"),2.5f);
    set(*FindTyped(*popup,"Fade","bool"),false);
    const auto* style=FindTyped(*popup,"Style","select");Check(style!=nullptr,"Tracer style selector missing");
    Json dashed;for(const auto& option:style->at("options"))if(option.at("label")=="Dashed")dashed=option.at("value");
    Check(!dashed.is_null(),"Dashed tracer style missing");set(*style,dashed);
    const auto* color=FindTyped(*popup,"Color","color");Check(color!=nullptr,"Tracer color picker missing");
    set(*color,Json::array({.25f,.5f,.75f,.5f}));
    const auto applied=custom::get_configuration();
    Check(applied.bullet_tracers&&applied.tracer_width==4.5f&&applied.tracer_lifetime==2.5f&&
        applied.tracer_style==2&&!applied.tracer_fade&&applied.tracer_color[2]==.75f,
        "Tracer GUI mutations did not reach the effect renderer's configuration");
    custom::set_configuration(original);
}
Json Run(Fixture& fixture){
    Json report={{"implementation","production gui_bridge::Request -> Process -> DrawPage on WARP"},{"checks",Json::array()}};
    auto& checks=report["checks"];
    Check(gui_bridge::Start(true),"Start loading bridge without renderer");
    Check(!gui_bridge::GuiReady(),"Features were allowed before the GUI mounted");
    auto initialLoading=Async({{"op","state"},{"page",23}});
    Check(initialLoading.wait_for(250ms)==std::future_status::ready,"Loading state depended on the render thread");
    auto splash=initialLoading.get();Ok(splash);
    Check(splash.at("startup").at("phase")=="loading"&&!splash.contains("page"),"Feature controls appeared before initialization");
    Ok(Request({{"op","gui_ready"}}));Check(gui_bridge::GuiReady(),"GUI-ready acknowledgement was lost");
    gui_bridge::Loading(70,"Loading visuals");
    Check(Request({{"op","startup"}}).at("startup").at("progress")==70,"Startup progress was not published");
    Check(!Request({{"op","set"},{"page",23},{"id","uninitialized"},{"value",true}}).value("ok",false),"A feature mutation was accepted while loading");
    gui_bridge::Stop();Check(!gui_bridge::GuiReady(),"Stop retained an old GUI-ready acknowledgement");
    Check(!Request({{"op","gui_ready"}}).value("ok",false),"Stopped bridge accepted a GUI-ready acknowledgement");
    Check(gui_bridge::Start(true),"Restart for native-first bootstrap");
    gui_bridge::FinishLoading();
    Check(Request({{"op","state"},{"page",23}}).at("startup").at("phase")=="loading", "Native completion released controls before the GUI mounted");
    Check(!Request({{"op","action"},{"page",23},{"id","uninitialized"}}).value("ok",false),"Native-first bootstrap accepted an early action");
    auto nativeFirst=Request({{"op","gui_ready"}});Ok(nativeFirst);
    Check(nativeFirst.at("startup").at("phase")=="ready", "GUI handshake did not immediately release completed native startup");
    gui_bridge::Stop();
    Check(gui_bridge::Start(true),"Restart for GUI-first bootstrap");
    Check(Request({{"op","gui_ready"}}).at("startup").at("phase")=="loading", "GUI mount released controls before native completion");
    gui_bridge::FinishLoading();
    Check(Request({{"op","startup"}}).at("startup").at("phase")=="ready", "Native completion did not release mounted GUI");
    gui_bridge::Stop();
    Check(gui_bridge::Start(true),"Restart for cancelled bootstrap");
    gui_bridge::FinishLoading();gui_bridge::Stop();
    Check(!Request({{"op","gui_ready"}}).value("ok",false), "Cancelled native-first bootstrap reopened the menu");
    checks.push_back("Loading replies bypass Present; GUI-first/native-first bootstrap releases controls only after both ready, and stop cancels pending readiness");
    Check(gui_bridge::Start(),"Start fixture bridge");
    auto read=Async({{"op","state"},{"page",19}});auto state=fixture.Pump(read);Ok(state);
    Check(state.at("pages").size()==std::size(menu_shell::navigationPages),"Actual production page map was not returned");
    const auto control=Toggle(state);const auto original=control.at("value").get<bool>();
    const auto mutation=[&](bool value){return Json{{"op","set"},{"page",19},{"id",control.at("id")},{"value",value}};};
    auto write=Async(mutation(!original));auto changed=fixture.Pump(write);Ok(changed);
    Check(Toggle(changed).at("value")==!original,"Mutation ACK did not contain committed production value");
    Check(Toggle(fixture.Immediate({{"op","state"},{"page",19}})).at("value")==!original,"Queue mutation did not reach production settings");
    auto restore=Async(mutation(original));Ok(fixture.Pump(restore));
    checks.push_back("State and boolean mutation roundtrip through real request queue and production DrawPage");
    NavigationAndEnableSettings(fixture);
    checks.push_back("Native navigation hierarchy excludes unavailable pages and readonly diagnostics; disabled rage/movement masters survive polls and subordinate edits, then restore");
    TracerSettings(fixture);
    checks.push_back("Bullet Tracers three-dot popup: toggle, color, width, duration, style and fade roundtrip into renderer configuration");

    const auto timeoutStart=Clock::now();auto timedOut=Async(mutation(!original));
    Check(timedOut.wait_for(3s)==std::future_status::ready,"Unprocessed mutation did not reach its renderer timeout");
    auto timeoutResponse=timedOut.get();report["timeoutMilliseconds"]=Milliseconds(timeoutStart);
    Check(!timeoutResponse.value("ok",false)&&timeoutResponse.value("error",std::string{}).find("renderer")!=std::string::npos,"Expected renderer timeout response");
    fixture.Process();
    Check(Toggle(fixture.Immediate({{"op","state"},{"page",19}})).at("value")==original,"Timed-out queued mutation applied after cancellation");
    checks.push_back("Two-second renderer timeout cancels a queued mutation before later Process");

    Ok(Request({{"op","session"},{"active",true},{"visible",true}}));
    Check(gui_bridge::SessionActive()&&gui_bridge::MenuVisible(),"Fixture session did not acquire its menu lease");
    auto stoppedMutation=Async(mutation(!original));auto stoppedState=Async({{"op","state"},{"page",19}});
    Check(stoppedMutation.wait_for(60ms)==std::future_status::timeout,"Mutation unexpectedly completed without Process");
    Check(stoppedState.wait_for(0ms)==std::future_status::timeout,"State unexpectedly completed without Process");
    const auto stopStart=Clock::now();gui_bridge::Stop();
    const auto stopDeadline=stopStart+750ms;
    Check(stoppedMutation.wait_until(stopDeadline)==std::future_status::ready&&stoppedState.wait_until(stopDeadline)==std::future_status::ready,"Stop did not promptly wake queued callers");
    Check(!stoppedMutation.get().value("ok",false)&&!stoppedState.get().value("ok",false),"Stopped queue returned successful work");
    report["stopWakeMilliseconds"]=Milliseconds(stopStart);
    fixture.Process();
    Check(Toggle(fixture.Immediate({{"op","state"},{"page",19}})).at("value")==original,"Stop-canceled mutation later changed production state");
    Check(!Request({{"op","session"},{"active",true},{"visible",true}}).value("ok",false),"Post-stop session request was accepted");
    Check(!gui_bridge::SessionActive()&&!gui_bridge::MenuVisible(),"Post-stop session reacquired input ownership");
    checks.push_back("Stop wakes state/set callers, discards their queued mutation and rejects renewed input lease");

    constexpr unsigned Rounds=16,Workers=4,AfterStop=48;
    std::uint64_t totalRequests{};
    for(unsigned round=0;round<Rounds;++round){
        Check(gui_bridge::Start(),"Restart fixture bridge for session/stop race");
        std::atomic<bool> stopReturned{};std::atomic<unsigned> calls{},failures{};
        std::vector<std::jthread> workers;
        for(unsigned worker=0;worker<Workers;++worker)workers.emplace_back([&](std::stop_token token){
            try{
                while(!token.stop_requested()&&!stopReturned.load(std::memory_order_acquire)){
                    Request({{"op","session"},{"active",true},{"visible",true}});++calls;
                }
                for(unsigned attempt=0;attempt<AfterStop&&!token.stop_requested();++attempt){
                    if(Request({{"op","session"},{"active",true},{"visible",true}}).value("ok",false))++failures;
                    ++calls;
                }
            }catch(...){++failures;}
        });
        const auto readyUntil=Clock::now()+2s;
        while(calls.load()<32&&Clock::now()<readyUntil)std::this_thread::yield();
        Check(calls.load()>=32,"Session race workers did not start");
        gui_bridge::Stop();stopReturned.store(true,std::memory_order_release);
        for(auto& worker:workers)worker.join();
        Check(failures.load()==0,"A concurrent/post-stop session was accepted after Stop returned");
        Check(!gui_bridge::SessionActive()&&!gui_bridge::MenuVisible(),"Session/Stop race left input lease active");
        totalRequests+=calls.load();
    }
    report["sessionRaceRounds"]=Rounds;report["sessionRaceRequests"]=totalRequests;
    checks.push_back("Sixteen four-worker session/Stop races leave input ownership released; all post-stop renewals rejected");
    Check(embeddedCalls.load()==0,"Queue fixture unexpectedly invoked the embedded UI wrapper");
    report["ok"]=true;return report;
}
}
int wmain(int argc,wchar_t** argv){
    const ComScope com;Json report;
    try{
        Check(SUCCEEDED(com.value),"Fixture COM initialization");
        {Fixture fixture;report=Run(fixture);}
        const std::filesystem::path output=argc>1?argv[1]:L"tests/artifacts/gui-queue-regression.json";
        std::ofstream(output)<<report.dump(2)<<'\n';std::cout<<report.dump(2)<<std::endl;return 0;
    }catch(const std::exception& error){std::cerr<<Json{{"ok",false},{"error",error.what()}}.dump()<<std::endl;return 1;}
}
