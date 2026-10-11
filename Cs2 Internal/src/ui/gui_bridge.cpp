#include "ui/gui_bridge.h"
#include "ui/gui_capture.h"
#include "ui/agent_preview.h"
#include "ui/catalog_ui.h"
#ifdef small
#undef small
#endif
#include "ui/menu_shell.h"
#include "imgui/imgui_internal.h"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#ifdef small
#undef small
#endif
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>

namespace gui_bridge {
namespace {
using Json = nlohmann::json;
ImGuiContext* captureContext{};
std::atomic<ULONGLONG> leaseUntil{};
std::atomic<bool> visible{}, stopping{};
std::uint64_t revision{};
struct Work {
    Json request, response;
    std::mutex mutex;
    std::condition_variable ready;
    bool complete{}, cancelled{}, started{};
};
std::mutex queueMutex;
std::deque<std::shared_ptr<Work>> queue;
std::atomic<bool> guiReady{};
bool initializing{}, featuresReady{};
unsigned startupProgress=100;
std::string startupMessage;

// Also served before Present hooks exist, so opening the GUI never depends on features.
Json StartupUnlocked() {
    return {{"ok",true},{"connected",true},{"startup",{{"phase",initializing?"loading":"ready"},
        {"progress",startupProgress},{"message",startupMessage}}}};
}
Json Startup(bool mounted=false) {
    std::lock_guard lock(queueMutex);
    if(stopping)return {{"ok",false},{"error","Menu is stopping"}};
    if(mounted) {
        guiReady=true;
        if(featuresReady){initializing=false;startupProgress=100;startupMessage.clear();}
    }
    return StartupUnlocked();
}

Json Error(const std::string& message) { return {{"ok",false},{"error",message}}; }
std::string Label(const char* text) {
    std::string result = text ? text : "";
    if (auto suffix = result.find("##"); suffix != std::string::npos) result.resize(suffix);
    return result;
}
bool Disabled() { return (ImGui::GetCurrentWindow()->DC.ItemFlags & ImGuiItemFlags_Disabled) != 0; }
std::string Hex(unsigned value) { char result[16]; sprintf_s(result,"%08x",value); return result; }
double ReadNumber(int type, const void* value) {
    switch(type) {
    case ImGuiDataType_S8:return *static_cast<const ImS8*>(value);
    case ImGuiDataType_U8:return *static_cast<const ImU8*>(value);
    case ImGuiDataType_S16:return *static_cast<const ImS16*>(value);
    case ImGuiDataType_U16:return *static_cast<const ImU16*>(value);
    case ImGuiDataType_S32:return *static_cast<const ImS32*>(value);
    case ImGuiDataType_U32:return *static_cast<const ImU32*>(value);
    case ImGuiDataType_S64:return static_cast<double>(*static_cast<const ImS64*>(value));
    case ImGuiDataType_U64:return static_cast<double>(*static_cast<const ImU64*>(value));
    case ImGuiDataType_Float:return *static_cast<const float*>(value);
    case ImGuiDataType_Double:return *static_cast<const double*>(value);
    default:throw std::runtime_error("Unsupported number type");
    }
}
template<class T> void Assign(void* target,double value) {
    if constexpr(std::is_integral_v<T> && sizeof(T)==8) {
        if(value < -9007199254740991.0 || value > 9007199254740991.0)
            throw std::runtime_error("Integer exceeds the exact GUI number range");
    }
    if(value<static_cast<double>((std::numeric_limits<T>::lowest)()) || value>static_cast<double>((std::numeric_limits<T>::max)()))
        throw std::runtime_error("Number is outside its storage range");
    *static_cast<T*>(target)=static_cast<T>(value);
}
void WriteNumber(int type,void* target,double value) {
    switch(type) {
    case ImGuiDataType_S8:Assign<ImS8>(target,value);break;
    case ImGuiDataType_U8:Assign<ImU8>(target,value);break;
    case ImGuiDataType_S16:Assign<ImS16>(target,value);break;
    case ImGuiDataType_U16:Assign<ImU16>(target,value);break;
    case ImGuiDataType_S32:Assign<ImS32>(target,value);break;
    case ImGuiDataType_U32:Assign<ImU32>(target,value);break;
    case ImGuiDataType_S64:Assign<ImS64>(target,value);break;
    case ImGuiDataType_U64:Assign<ImU64>(target,value);break;
    case ImGuiDataType_Float:Assign<float>(target,value);break;
    case ImGuiDataType_Double:Assign<double>(target,value);break;
    }
}
std::string Base64(const std::string& bytes) {
    static constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;out.reserve((bytes.size()+2)/3*4);
    for(size_t i=0;i<bytes.size();i+=3) {
        unsigned value=static_cast<unsigned char>(bytes[i])<<16;
        if(i+1<bytes.size())value|=static_cast<unsigned char>(bytes[i+1])<<8;
        if(i+2<bytes.size())value|=static_cast<unsigned char>(bytes[i+2]);
        out+=alphabet[(value>>18)&63];out+=alphabet[(value>>12)&63];
        out+=i+1<bytes.size()?alphabet[(value>>6)&63]:'=';out+=i+2<bytes.size()?alphabet[value&63]:'=';
    }
    return out;
}
std::string Thumbnail(const char* filename) {
    // Paths originate exclusively in the native catalog, never from IPC.
    if(!filename||!*filename)return {};
    static std::map<std::string,std::string> cache;
    auto found=cache.find(filename);if(found!=cache.end())return found->second;
    std::error_code ec;auto path=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(filename)));
    auto size=std::filesystem::file_size(path,ec);
    if(ec||size==0||size>256*1024||path.extension()!=L".png")return {};
    std::ifstream file(path,std::ios::binary);std::string bytes(static_cast<size_t>(size),'\0');
    if(!file.read(bytes.data(),bytes.size())||bytes.compare(0,8,"\x89PNG\r\n\x1a\n",8)!=0)return {};
    auto data="data:image/png;base64,"+Base64(bytes);
    if(cache.size()>=128)cache.clear();cache.emplace(filename,data);return data;
}

std::string EncodeFrame(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11ShaderResourceView* view) {
    using Microsoft::WRL::ComPtr;
    if(!view)return {};
    const auto check=[](HRESULT result){if(FAILED(result))throw std::runtime_error("Could not read the preview image.");};
    struct ComScope {HRESULT result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);~ComScope(){if(SUCCEEDED(result))CoUninitialize();}} com;
    if(FAILED(com.result)&&com.result!=RPC_E_CHANGED_MODE)check(com.result);
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;check(resource.As(&texture));D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    if(desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM||desc.Width>1024||desc.Height>1024)throw std::runtime_error("Unsupported preview image.");
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    ComPtr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&desc,nullptr,&staging));context->CopyResource(staging.Get(),texture.Get());
    ComPtr<IWICImagingFactory> factory;check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IStream> stream;check(CreateStreamOnHGlobal(nullptr,TRUE,&stream));
    ComPtr<IWICBitmapEncoder> encoder;check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;check(encoder->CreateNewFrame(&frame,nullptr));check(frame->Initialize(nullptr));check(frame->SetSize(desc.Width,desc.Height));
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;check(frame->SetPixelFormat(&format));
    if(!IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA))throw std::runtime_error("Unsupported preview encoder.");
    std::vector<BYTE> bgra(static_cast<size_t>(desc.Width)*desc.Height*4);
    D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
    for(UINT y=0;y<desc.Height;++y){
        const auto* source=static_cast<const BYTE*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch;
        auto* target=bgra.data()+static_cast<size_t>(y)*desc.Width*4;
        for(UINT x=0;x<desc.Width;++x){target[x*4]=source[x*4+2];target[x*4+1]=source[x*4+1];target[x*4+2]=source[x*4];target[x*4+3]=source[x*4+3];}
    }
    context->Unmap(staging.Get(),0);
    check(frame->WritePixels(desc.Height,desc.Width*4,static_cast<UINT>(bgra.size()),bgra.data()));check(frame->Commit());check(encoder->Commit());
    STATSTG stat{};check(stream->Stat(&stat,STATFLAG_NONAME));
    if(stat.cbSize.QuadPart>2*1024*1024)throw std::runtime_error("Preview image is too large.");
    LARGE_INTEGER start{};check(stream->Seek(start,STREAM_SEEK_SET,nullptr));
    std::string bytes(static_cast<size_t>(stat.cbSize.QuadPart),'\0');ULONG read{};check(stream->Read(bytes.data(),static_cast<ULONG>(bytes.size()),&read));
    if(read!=bytes.size())throw std::runtime_error("Preview image was incomplete.");
    return "data:image/png;base64,"+Base64(bytes);
}
Json Preview(const Json& request,ID3D11Device* device,ID3D11DeviceContext* context) {
    const auto action=request.value("action",std::string{});agent_preview::WebAction command=agent_preview::WebAction::Frame;
    float x{},y{};bool enabled{};
    const auto number=[&](const char* key,float limit) {
        if(!request.contains(key)||!request[key].is_number())throw std::runtime_error("Invalid preview input");
        const double value=request[key].get<double>();
        if(!std::isfinite(value)||std::abs(value)>limit)throw std::runtime_error("Invalid preview input");return static_cast<float>(value);
    };
    if(action=="rotate") {command=agent_preview::WebAction::Rotate;if(!request.contains("value")||!request["value"].is_boolean())return Error("Invalid rotation state");enabled=request["value"].get<bool>();}
    else if(action=="reset")command=agent_preview::WebAction::Reset;
    else if(action=="reload")command=agent_preview::WebAction::Reload;
    else if(action=="equipped")command=agent_preview::WebAction::Equipped;
    else if(action=="drag"){command=agent_preview::WebAction::Drag;x=number("dx",2000);y=number("dy",2000);}
    else if(action=="zoom"){command=agent_preview::WebAction::Zoom;x=number("value",20);}
    else if(!action.empty())return Error("Unknown preview action");
    const auto dimension=[&](const char* key,unsigned fallback){
        if(!request.contains(key))return fallback;
        if(!request[key].is_number_integer())throw std::runtime_error("Invalid preview size");
        const auto value=request[key].get<double>();
        if(value<128||value>1024)throw std::runtime_error("Invalid preview size");
        return static_cast<unsigned>(value);
    };
    auto frame=agent_preview::RenderWeb(device,context,command,x,y,enabled,dimension("width",480),dimension("height",960));
    return {{"ok",true},{"connected",true},{"image",EncodeFrame(device,context,frame.texture)},
        {"name",frame.name},{"weapon",frame.weapon},{"rotate",frame.rotate},{"status",frame.status},{"message",frame.message}};
}

class Capture final : public gui_capture::Sink {
    struct Frame {Json* base;Json* current;std::string id;std::string section;};
    std::vector<Frame> frames;
    Json* selection{};
    const Json& request;
    std::string target,op;
    std::set<std::string> ids;
    int page;
    size_t imageBytes{};
    std::string Id(const char* label,unsigned overrideId=0) {
        const auto& frame=frames.back();
        return (frame.current==frame.base?frame.id:frame.section)+":"+Hex(overrideId?overrideId:ImGui::GetID(label));
    }
    Json& Add(const char* label,const char* type,bool disabled=false,unsigned id=0) {
        auto key=Id(label,id);
        // Native headers sometimes repeat label IDs in independent sections.
        if(!ids.insert(key).second)throw std::runtime_error("Duplicate control identity: "+Label(label));
        frames.back().current->push_back({{"id",key},{"type",type},{"label",Label(label)},{"disabled",disabled||Disabled()}});
        return frames.back().current->back();
    }
    bool Match(const Json& node,const char* expected="set") {
        if(target.empty()||node.at("id")!=target)return false;
        found=true;
        if(node.value("disabled",false)){failure="This control is currently disabled";return false;}
        if(op!=expected){failure="This control does not support that operation";return false;}
        if(applied){failure="A request can change only one control";return false;}
        return true;
    }
    const Json& Value() const {return request.at("value");}
public:
    Json controls=Json::array();
    bool found{},applied{};
    std::string failure;
    Capture(int p,const Json& req):request(req),target(req.value("id",std::string{})),op(req.value("op",std::string{})),page(p) {
        frames.push_back({&controls,&controls,"p"+std::to_string(page),{}});
    }
    bool Boolean(const char* label,bool* value) override {
        auto& node=Add(label,"bool");node["value"]=*value;
        if(!Match(node))return false;
        if(!Value().is_boolean()){failure="Expected a boolean";return false;}
        *value=Value().get<bool>();applied=true;return true;
    }
    bool Number(const char* label,int type,void* data,int count,const void* low,const void* high,const char* format,double step) override {
        auto& node=Add(label,count>1?"vector":"number");
        const auto stride=ImGui::DataTypeGetInfo(type)->Size;
        Json values=Json::array();for(int i=0;i<count;++i)values.push_back(ReadNumber(type,static_cast<char*>(data)+stride*i));
        node["value"]=count==1?values[0]:values;
        const bool integer=type<ImGuiDataType_Float;
        if(low)node["min"]=ReadNumber(type,low);if(high)node["max"]=ReadNumber(type,high);
        if(integer)step=1;
        else if(step<=0) {
            step=.01;const char* dot=format?strchr(format,'.'):nullptr;
            if(dot&&dot[1]>='0'&&dot[1]<='6')step=std::pow(10.0,-(dot[1]-'0'));
        }
        node["step"]=step;node["format"]=format?format:"";
        if(!Match(node))return false;
        const auto& input=Value();
        if(count>1&&(!input.is_array()||input.size()!=count)){failure="Wrong vector size";return false;}
        double changed[16]{};
        if(count<1||count>16){failure="Unsupported vector size";return false;}
        for(int i=0;i<count;++i) {
            const auto& number=count==1?input:input[i];
            if(!number.is_number()){failure="Expected a number";return false;}
            changed[i]=number.get<double>();
            if(!std::isfinite(changed[i])||(integer&&std::floor(changed[i])!=changed[i])||
                (low&&changed[i]<ReadNumber(type,low))||(high&&changed[i]>ReadNumber(type,high))) {
                failure="Number is outside the allowed range";return false;
            }
        }
        // Validate each component's storage range before changing any component.
        alignas(8) char scratch[8]{};for(int i=0;i<count;++i)WriteNumber(type,scratch,changed[i]);
        for(int i=0;i<count;++i)WriteNumber(type,static_cast<char*>(data)+stride*i,changed[i]);
        applied=true;return true;
    }
    bool Color(const char* label,float* value,int count) override {
        auto& node=Add(label,"color");node["value"]=Json::array();for(int i=0;i<count;++i)node["value"].push_back(value[i]);
        if(!Match(node))return false;
        if(!Value().is_array()||Value().size()!=count){failure="Wrong color size";return false;}
        for(const auto& component:Value())if(!component.is_number()||!std::isfinite(component.get<double>())||component.get<double>()<0||component.get<double>()>1){failure="Color components must be between zero and one";return false;}
        for(int i=0;i<count;++i)value[i]=Value()[i].get<float>();applied=true;return true;
    }
    bool Text(const char* label,char* value,size_t capacity,bool readOnly) override {
        auto& node=Add(label,"text",readOnly);node["value"]=std::string(value,strnlen(value,capacity));node["maxLength"]=capacity?capacity-1:0;
        if(!Match(node))return false;
        if(!Value().is_string()){failure="Expected text";return false;}
        const auto text=Value().get<std::string>();if(text.size()>=capacity||text.find('\0')!=std::string::npos){failure="Text is too long";return false;}
        memcpy(value,text.c_str(),text.size()+1);applied=true;return true;
    }
    bool Action(const char* label,bool disabled=false) override {
        if(Label(label).empty())return false;
        auto& node=Add(label,"action",disabled);
        if(!Match(node,"action"))return false;applied=true;return true;
    }
    bool Radio(const char* label,bool selected) override {
        auto& node=Add(label,"action");node["selected"]=selected;
        if(!Match(node,"action"))return false;applied=true;return true;
    }
    void BeginSelect(const char* label,const char* preview) override {
        selection=&Add(label,"select");(*selection)["options"]=Json::array();(*selection)["value"]="";(*selection)["displayValue"]=Label(preview);
    }
    bool Option(const char* label,bool selected,bool disabled) override {
        if(!selection) {
            auto& node=Add(label,"catalog",disabled);node["selected"]=selected;
            if(!Match(node,"action"))return false;applied=true;return true;
        }
        const auto key=Hex(ImGui::GetID(label));
        (*selection)["options"].push_back({{"value",key},{"label",Label(label)},{"disabled",disabled||Disabled()}});
        if(selected)(*selection)["value"]=key;
        if(target.empty()||selection->at("id")!=target||!Value().is_string()||Value()!=key)return false;
        if(!Match(*selection))return false;
        if(disabled||Disabled()){failure="This option is disabled";return false;}
        applied=true;return true;
    }
    void EndSelect() override {
        if(selection&&selection->at("id")==target&&!applied){found=true;if(failure.empty())failure="Unknown selection";}
        selection=nullptr;
    }
    void Section(const char* label) override {
        frames.back().current=frames.back().base;
        auto& node=Add(label,"group");node["children"]=Json::array();frames.back().section=node["id"].get<std::string>();frames.back().current=&node["children"];
    }
    void BeginGroup(const char* label,const char* type,unsigned id=0) override {
        auto& node=Add(label,type,false,id);node["children"]=Json::array();auto key=node["id"].get<std::string>();
        frames.push_back({&node["children"],&node["children"],key,{}});
    }
    void EndGroup() override {if(frames.size()>1)frames.pop_back();}
    bool Keybind(const char* label,int* value) override {
        auto& node=Add(label,"keybind");node["value"]=*value;node["zeroLabel"]=page==25?"Always":"Unbound";
        if(!Match(node))return false;
        if(!Value().is_number_integer()||Value().get<double>()<0||Value().get<double>()>255){failure="Invalid key";return false;}
        if(Value()==VK_INSERT||Value()==VK_END){failure="Insert and End are reserved for menu controls";return false;}
        *value=Value().get<int>();applied=true;return true;
    }
    bool Card(const char* label,const char* image,bool selected,bool disabled) override {
        auto& node=Add(label,"catalog",disabled);node["selected"]=selected;
        auto thumbnail=Thumbnail(image);if(imageBytes+thumbnail.size()<4*1024*1024){imageBytes+=thumbnail.size();node["image"]=std::move(thumbnail);}
        if(!Match(node,"action"))return false;applied=true;return true;
    }
    void ReportError(const char* message) override {if(applied&&message&&*message)failure=message;}
};

Json CapturePage(int page,const Json& request,const DrawPage& draw,bool& applied) {
    auto* previous=ImGui::GetCurrentContext();
    if(!previous)throw std::runtime_error("The game renderer is not ready");
    struct RestoreContext {
        ImGuiContext* previous;
        bool frameComplete{};
        ~RestoreContext(){
            gui_capture::active=nullptr;
            if(!frameComplete&&captureContext){ImGui::DestroyContext(captureContext);captureContext=nullptr;}
            ImGui::SetCurrentContext(previous);
        }
    } restore{previous};
    if(!captureContext) {
        captureContext=ImGui::CreateContext(previous->IO.Fonts);
        captureContext->IO.IniFilename=nullptr;captureContext->IO.LogFilename=nullptr;
        captureContext->Style=previous->Style;
    }
    ImGui::SetCurrentContext(captureContext);
    auto& io=ImGui::GetIO();io.DisplaySize={1280,10000};io.DeltaTime=1.f/60;io.MousePos={-10000,-10000};
    Capture sink(page,request);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({1120,9800});
    ImGui::Begin("##GuiCapture",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoTitleBar);
    ImGui::PushID(page);
    gui_capture::active=&sink;
    draw(page);
    gui_capture::active=nullptr;
    ImGui::PopID();ImGui::End();ImGui::EndFrame();restore.frameComplete=true;
    applied=sink.applied;
    if(!sink.failure.empty())throw std::runtime_error(sink.failure);
    if(request.contains("id")&&!sink.found)throw std::runtime_error("This control is no longer available; refresh the page");
    return {{"id",page},{"controls",std::move(sink.controls)}};
}
Json Envelope(Json page) {
    Json groups=Json::array(),pages=Json::array();
    for(int i=0;i<std::size(menu_shell::groups);++i)groups.push_back({{"id",i},{"label",menu_shell::groups[i]}});
    for(int i:menu_shell::navigationPages)pages.push_back({{"id",i},{"group",menu_shell::pages[i].group},{"label",menu_shell::pages[i].label}});
    return {{"ok",true},{"connected",true},{"revision",revision},{"groups",groups},{"pages",pages},{"page",std::move(page)},{"menuVisible",MenuVisible()},
        {"previewRequested",catalog_ui::ConsumeAgentPreviewRequest()}};
}
std::string Handle(const std::string& line) {
    try {
        if(line.empty()||line.size()>=8u*1024u*1024u)return Error("Invalid request size").dump();
        if(stopping)return Error("Menu is stopping").dump();
        auto request=Json::parse(line);
        if(!request.is_object())return Error("Expected an object").dump();
        const auto op=request.value("op",std::string{});
        if(op=="startup"||op=="gui_ready")return Startup(op=="gui_ready").dump();
        if(op=="session") {
            if(!request.contains("active")||!request["active"].is_boolean()||!request.contains("visible")||!request["visible"].is_boolean())return Error("Invalid session").dump();
            std::lock_guard lock(queueMutex);
            if(stopping)return Error("Menu is stopping").dump();
            visible=request["visible"].get<bool>();leaseUntil=request["active"].get<bool>()?GetTickCount64()+3000:0;
            return Json{{"ok",true},{"connected",true},{"menuVisible",MenuVisible()}}.dump();
        }
        if(op=="ping")return Json{{"ok",true},{"connected",true},{"menuVisible",MenuVisible()}}.dump();
        if(op!="state"&&op!="set"&&op!="action"&&op!="preview")return Error("Unknown operation").dump();
        auto work=std::make_shared<Work>();work->request=std::move(request);
        {std::lock_guard lock(queueMutex);if(stopping)return Error("Menu is stopping").dump();
            if(initializing)return (op=="state"||op=="preview"?StartupUnlocked():Error("Features are loading")).dump();
            if(queue.size()>=8)return Error("Menu is busy").dump();queue.push_back(work);}
        std::unique_lock lock(work->mutex);
        if(!work->ready.wait_for(lock,std::chrono::seconds(2),[&]{return work->complete;})) {
            if(!work->started)work->cancelled=true;
            return Error("The game renderer is not responding").dump();
        }
        return work->response.dump(-1,' ',false,Json::error_handler_t::replace);
    } catch(const std::exception& error){return Error(error.what()).dump();}
}
}

bool SessionActive(){return leaseUntil.load()>GetTickCount64();}
bool MenuVisible(){return SessionActive()&&visible.load();}
bool Start(bool loading){
    {std::lock_guard lock(queueMutex);stopping=false;leaseUntil=0;visible=false;guiReady=false;
        initializing=loading;featuresReady=!loading;startupProgress=loading?5u:100u;startupMessage=loading?"Opening GUI":"";}
    return true;
}
void Loading(unsigned progress,const char* message){std::lock_guard lock(queueMutex);if(stopping)return;initializing=true;featuresReady=false;startupProgress=std::min(progress,100u);startupMessage=message?message:"";}
void FinishLoading(){std::lock_guard lock(queueMutex);if(stopping)return;featuresReady=true;if(guiReady){initializing=false;startupProgress=100;startupMessage.clear();}}
bool GuiReady(){return !stopping&&guiReady.load();}
std::string Request(const std::string& request){return Handle(request);}
void Stop(){
    {std::lock_guard lock(queueMutex);stopping=true;leaseUntil=0;visible=false;guiReady=false;for(auto& work:queue){std::lock_guard workLock(work->mutex);work->response=Error("Menu stopped");work->complete=true;work->ready.notify_all();}queue.clear();}
}
void DestroyCapture(){if(captureContext){auto* previous=ImGui::GetCurrentContext();ImGui::DestroyContext(captureContext);captureContext=nullptr;ImGui::SetCurrentContext(previous);} }
Json Execute(const Json& request,const DrawPage& draw,ID3D11Device* device,ID3D11DeviceContext* context){
    try {
        if(request.is_object()){
            const auto op=request.value("op",std::string{});
            if(op=="startup"||op=="gui_ready")return Startup(op=="gui_ready");
            std::lock_guard lock(queueMutex);if(initializing)return op=="state"||op=="preview"?StartupUnlocked():Error("Features are loading");
        }
        if(request.is_object()&&request.value("op",std::string{})=="preview")return Preview(request,device,context);
        if(!request.is_object()||!request.contains("page")||!request["page"].is_number_integer())return Error("Invalid page");
        const double pageNumber=request["page"].get<double>();
        if(pageNumber<0||pageNumber>=std::size(menu_shell::pages))return Error("Unknown page");
        const int page=request["page"].get<int>();
        const auto op=request.value("op",std::string{});
        if(op!="state"&&op!="set"&&op!="action")return Error("Unknown operation");
        if(op!="state"&&(!request.contains("id")||!request["id"].is_string()))return Error("Missing control identity");
        if(op=="set"&&!request.contains("value"))return Error("Missing control value");
        if(op=="state"&&request.contains("id"))return Error("State cannot change controls");
        bool applied=false;auto result=CapturePage(page,request,draw,applied);
        if(op!="state"){
            if(!applied)return Error("The control did not accept the change");
            ++revision;bool ignored=false;result=CapturePage(page,Json{{"op","state"}},draw,ignored);
        }
        return Envelope(std::move(result));
    }catch(const std::exception& error){return Error(error.what());}
}
void Process(const DrawPage& draw,ID3D11Device* device,ID3D11DeviceContext* context){
    std::shared_ptr<Work> work;
    {std::lock_guard lock(queueMutex);if(queue.empty())return;work=queue.front();queue.pop_front();}
    {std::lock_guard lock(work->mutex);if(work->cancelled)return;work->started=true;}
    auto result=Execute(work->request,draw,device,context);
    {std::lock_guard lock(work->mutex);work->response=std::move(result);work->complete=true;work->ready.notify_all();}
}
}
