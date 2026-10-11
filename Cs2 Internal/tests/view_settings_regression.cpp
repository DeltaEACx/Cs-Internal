// Compile the actual camera override so callable native bindings can be injected.
#include "features/view/view_settings.cpp"
#include "player_fixture.h"
#include <iostream>
#include <thread>

namespace
{
using namespace view_settings;
using Address=game_access::Address;
using Vector=view_settings::Vector;
struct CameraFixture;
CameraFixture* fixture{};
void __fastcall FakeOverride(Address,Address);
Address __fastcall FakeEye(Address,Vector*);
void __fastcall FakeFilter(void*,Address,std::uint64_t,std::uint8_t,int);
bool __fastcall FakeTrace(Address,const view_settings::Hull*,const Vector*,const Vector*,void*,view_settings::Hit*);
float __fastcall FakeAspect(void*,int,int);

bool Close(Vector a,Vector b,float tolerance=.0002f)
{
    const float x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;
    return x*x+y*y+z*z<=tolerance*tolerance;
}
Vector Add(Vector a,Vector b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Vector Delta(Vector a,Vector b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
Vector Scale(Vector a,float s){return {a.x*s,a.y*s,a.z*s};}
template<class T>T Get(Address address)
{
    T value{};Check(game_access::Read(address,value),"fixture native read failed");return value;
}
template<class T>void Put(Address address,const T& value)
{Check(game_access::Write(address,value),"fixture native write failed");}
template<class Block>Address At(const Block& block){return reinterpret_cast<Address>(block.data());}

struct CameraFixture:PlayerFixture
{
    std::vector<unsigned char> services=std::vector<unsigned char>(0x100),view=std::vector<unsigned char>(0x700);
    Vector eye{200,30,70},visible{0,0,2},cached{40,-80,7};
    std::array<unsigned char,0x700> nativeView{};
    unsigned originals{},eyes{},filters{},traces{},aspects{};
    bool observedThird{},restricted{true},invalidAngles{},invalidEye{},wrongEyeReturn{},raiseEye{},raiseFilter{},raiseTrace{};
    bool nativeTraceReturn{true},allSolid{},wrongEndpoint{},invalidEndpoint{},skipOriginalOrigins{};
    bool epochInsideOriginal{},pawnInsideOriginal{};
    std::vector<unsigned char> replacementPawn;
    float fraction{1};
    enum class Mutation{None,Death,Disable,Stop,InputReplacement,Epoch};
    Mutation mutation{};
    Address replacementInput{},manager{0xbeef};
    Vector tracedStart{},tracedEnd{};

    CameraFixture()
    {
        fixture=this;game_access::verifiedClient=Base();hvh_input::worldEpoch=1;
        ForgetCamera();running=true;thirdRequested=true;cameraReady=true;aspectReady=true;
        aspectRequested=false;aspect=4.f/3;cameraDistance=100;cameraHookCreated=aspectHookCreated=false;
        cameraCallbacks=aspectCallbacks=0;originalOverride=FakeOverride;shootPosition=FakeEye;
        cameraFilter=FakeFilter;cameraTrace=FakeTrace;originalAspect=FakeAspect;
        Set(pawn,game_access::field::C_BasePlayerPawn::m_pWeaponServices,At(services));
        Set(services,0x38,Pawn());Set(globals,view_bindings::kTraceManager,manager);
        Set(input,0x228,std::uint8_t{0x96});Set(input,0x22a,std::uint8_t{0x5a});
        Set(input,0x230,Vector{17,31,137});
        std::fill(view.begin(),view.end(),0x77);
    }
    ~CameraFixture()
    {
        running=false;thirdRequested=false;Tick();ForgetCamera();fixture=nullptr;
        game_access::verifiedClient=0;
    }
    Address CurrentInput()const{return Get<Address>(Base()+view_bindings::kInputPointer);}
    Address CurrentPawn()const{return game_access::ReadLocalPlayer(Base()).pawn;}
    bool Third(Address value=0)const{return Get<bool>((value?value:CurrentInput())+view_bindings::kThirdPerson);}
    void SetThird(bool value){Put(CurrentInput()+view_bindings::kThirdPerson,value);}
    void Call(Address target=0){OverrideHook(0xcafe,target?target:At(view));}
    void Origins(Vector expected,Address target=0)const
    {
        for(auto offset:view_bindings::kViewOrigins)
            Check(Close(Get<Vector>((target?target:At(view))+offset),expected),"camera origin mirror differs from expected position");
    }
    void NativeFieldsUnchanged(Address target=0)const
    {
        const auto* bytes=reinterpret_cast<const unsigned char*>(target?target:At(view));
        for(std::size_t i=0;i<nativeView.size();++i)
        {
            bool origin=false;for(auto offset:view_bindings::kViewOrigins){const auto first=static_cast<std::size_t>(offset);origin|=i>=first&&i<first+sizeof(Vector);}
            if(!origin)Check(bytes[i]==nativeView[i],"camera modified native view angles or unrelated view fields");
        }
        Check(input[0x228]==0x96&&input[0x22a]==0x5a&&Close(Get<Vector>(At(input)+0x230),{17,31,137}),
            "camera modified adjacent input flags or native cached mouse angles");
        Check(cameraCallbacks==0,"camera callback scope remained active");
    }
};

void __fastcall FakeOverride(Address self,Address view)
{
    Check(fixture&&self==0xcafe,"original OverrideView ABI changed");auto& f=*fixture;++f.originals;
    const auto input=f.CurrentInput();f.observedThird=input&&f.Third(input);
    // Emulate the live failure: third-person input consumes cached angles while
    // the restricted-server path clears the flag on each native view update.
    Vector angles=f.observedThird?f.cached:f.visible;
    if(!f.observedThird)f.cached=f.visible;
    if(f.invalidAngles)angles.x=std::numeric_limits<float>::quiet_NaN();
    Put(view+view_bindings::kViewAngles,angles);
    if(!f.skipOriginalOrigins)for(auto offset:view_bindings::kViewOrigins)Put(view+offset,f.eye);
    if(input&&f.restricted)Put(input+view_bindings::kThirdPerson,false);
    std::memcpy(f.nativeView.data(),reinterpret_cast<void*>(view),f.nativeView.size());
    if(f.epochInsideOriginal){f.epochInsideOriginal=false;hvh_input::LevelChanged();}
    if(f.pawnInsideOriginal){
        f.pawnInsideOriginal=false;
        f.Set(f.page,0xe0,At(f.replacementPawn));f.Set(f.page,0xf0,std::uint32_t{0x18002});
        f.Set(f.controller,game_access::field::CCSPlayerController::m_hPlayerPawn,std::uint32_t{0x18002});
        f.Set(f.services,0x38,At(f.replacementPawn));
    }
}
Address __fastcall FakeEye(Address services,Vector* out)
{
    auto& f=*fixture;++f.eyes;
    Check(services==At(f.services)&&Get<Address>(services+0x38)==f.CurrentPawn(),"shoot-position weapon-services ABI/owner changed");
    if(f.raiseEye)RaiseException(0xe000c001,0,0,nullptr);
    *out=f.eye;if(f.invalidEye)out->z=NAN;
    return f.wrongEyeReturn?0:reinterpret_cast<Address>(out);
}
void __fastcall FakeFilter(void* filter,Address pawn,std::uint64_t mask,std::uint8_t group,int type)
{
    auto& f=*fixture;++f.filters;
    Check(filter&&pawn==f.CurrentPawn()&&mask==0x80&&group==3&&type==15,"native camera filter arguments changed");
    if(f.raiseFilter)RaiseException(0xe000c002,0,0,nullptr);
    std::uint64_t exclusions=0x2000;std::memcpy(static_cast<std::byte*>(filter)+0x10,&exclusions,sizeof(exclusions));
}
bool __fastcall FakeTrace(Address manager,const view_settings::Hull* hull,const Vector* start,const Vector* end,void* filter,view_settings::Hit* hit)
{
    auto& f=*fixture;++f.traces;
    Check(manager==f.manager&&hull&&hull->type==2&&Close(hull->mins,{-6,-6,-6})&&Close(hull->maxs,{6,6,6}),
        "camera native hull/manager ABI changed");
    Check(start&&end&&hit&&Close(*start,f.eye),"camera trace did not originate at native shooting eye");
    std::uint64_t exclusions{};std::memcpy(&exclusions,static_cast<std::byte*>(filter)+0x10,sizeof(exclusions));
    Check(exclusions==0xc2000,"native camera collision exclusions lost generic filter bits");
    f.tracedStart=*start;f.tracedEnd=*end;
    if(f.raiseTrace)RaiseException(0xe000c003,0,0,nullptr);
    hit->fraction=f.fraction;hit->end=Add(*start,Scale(Delta(*end,*start),f.fraction));hit->allSolid=f.allSolid;
    if(f.wrongEndpoint)hit->end.x+=4;if(f.invalidEndpoint)hit->end.y=NAN;
    switch(f.mutation)
    {
    case CameraFixture::Mutation::Death:f.Health(0);break;
    case CameraFixture::Mutation::Disable:thirdRequested=false;break;
    case CameraFixture::Mutation::Stop:running=false;break;
    case CameraFixture::Mutation::InputReplacement:f.Set(f.globals,view_bindings::kInputPointer,f.replacementInput);break;
    case CameraFixture::Mutation::Epoch:hvh_input::LevelChanged();break;
    default:break;
    }
    return f.nativeTraceReturn;
}
float __fastcall FakeAspect(void*,int width,int height)
{++fixture->aspects;return float(width)/height;}

void FreshMouseAndRestrictedServer()
{
    CameraFixture f;Tick();Check(cameraOwned&&!f.Third()&&f.originals==0,"Tick used the restricted native camera command");
    f.Call();Check(!f.observedThird&&f.Third()&&cameraOwned,"native first-person preparation or local-body flag failed");
    f.Origins({100,30,70});f.NativeFieldsUnchanged();
    f.visible={0,90,5};f.Call();
    Check(!f.observedThird&&f.Third()&&f.originals==2,"restricted-server reset disabled local third person");
    f.Origins({200,-70,70});Check(Close(Get<Vector>(At(f.view)+view_bindings::kViewAngles),f.visible),"mouse turn used stale cached third-person angles");
    f.NativeFieldsUnchanged();
    f.visible={90,0,-4};f.Call();f.Origins({200,30,170});f.NativeFieldsUnchanged();
    Check(f.eyes==3&&f.filters==3&&f.traces==3,"fresh camera updates skipped native eye/collision callbacks");
    thirdRequested=false;f.Call();
    Check(!f.observedThird&&!cameraOwned&&!f.Third(),"disable did not restore mode before calling native view");f.Origins(f.eye);
}
void CollisionAndMalformedNative()
{
    CameraFixture f;f.fraction=.5f;f.Call();f.Origins({152,30,70});Check(f.Third(),"wall-clipped camera lost local-body visibility");
    f.fraction=.06f;f.Call();f.Origins({196,30,70});Check(!f.Third()&&cameraOwned,"close obstacle did not pull camera into first person");
    f.allSolid=true;f.fraction=.5f;f.wrongEndpoint=true;f.Call();f.Origins(f.eye);Check(!f.Third(),"all-solid hull escaped native eye");
    f.allSolid=false;f.wrongEndpoint=false;f.fraction=1;f.nativeTraceReturn=false;f.Call();
    f.Origins({100,30,70});Check(f.Third(),"false native clear trace return was treated as a failed trace");
    for(int failure=0;failure<11;++failure)
    {
        f.invalidAngles=f.invalidEye=f.wrongEyeReturn=f.raiseEye=f.raiseFilter=f.raiseTrace=f.wrongEndpoint=f.invalidEndpoint=false;
        f.fraction=1;f.nativeTraceReturn=true;
        if(failure==0)f.invalidAngles=true;if(failure==1)f.invalidEye=true;if(failure==2)f.wrongEyeReturn=true;
        if(failure==3)f.raiseEye=true;if(failure==4)f.raiseFilter=true;if(failure==5)f.raiseTrace=true;
        if(failure==6)f.fraction=NAN;if(failure==7)f.fraction=-.1f;if(failure==8)f.fraction=1.1f;
        if(failure==9)f.wrongEndpoint=true;if(failure==10)f.invalidEndpoint=true;
        f.Call();Check(!cameraOwned&&!f.Third(),"invalid/faulting native camera retained ownership/body mode");f.Origins(f.eye);
    }
    f.invalidEndpoint=false;f.Set(f.services,0x38,Address{0x12345});const auto before=f.eyes;
    f.Call();Check(f.eyes==before&&!cameraOwned,"foreign weapon-services owner reached native eye callback");
    f.Set(f.services,0x38,f.Pawn());f.Set(f.globals,view_bindings::kTraceManager,Address{});
    f.Call();Check(!cameraOwned&&!f.Third(),"missing trace manager retained owned camera");
}
void DistanceAndModeOwnership()
{
    CameraFixture f;
    for(auto [configured,expected]:std::array<std::array<float,2>,4>{{{0,35},{10000,200},{NAN,100},{75,75}}})
    {
        cameraDistance=configured;f.Call();f.Origins({f.eye.x-expected,f.eye.y,f.eye.z});
    }
    thirdRequested=false;Tick();Check(!cameraOwned&&!f.Third(),"disable did not restore originally first-person input");
    f.SetThird(true);thirdRequested=true;Tick();f.visible={0,90,0};f.Call();
    Check(!f.observedThird&&f.Third(),"preexisting third-person mode prevented fresh native view");
    thirdRequested=false;Tick();Check(f.Third()&&!cameraOwned,"preexisting third-person mode was not restored");
    f.restricted=false;f.SetThird(false);thirdRequested=true;f.Call();f.Health(0);f.Call();
    Check(!f.observedThird&&!f.Third()&&!cameraOwned,"death did not restore mode before native view");
    f.Health(100);f.Call();Check(cameraOwned&&f.Third(),"respawn did not reacquire camera");
}
void NativeMutationAndReplacement()
{
    for(auto mutation:{CameraFixture::Mutation::Death,CameraFixture::Mutation::Disable,CameraFixture::Mutation::Stop,
                      CameraFixture::Mutation::InputReplacement,CameraFixture::Mutation::Epoch})
    {
        CameraFixture f;std::vector<unsigned char> replacement(0x1000);f.replacementInput=At(replacement);
        Put(At(replacement)+view_bindings::kThirdPerson,true);f.mutation=mutation;f.Call();
        Check(!cameraOwned,"native callback identity/request change retained camera ownership");f.Origins(f.eye);
        if(mutation==CameraFixture::Mutation::InputReplacement)Check(f.Third(At(replacement)),"old camera mode overwritten into replacement input");
        else Check(!f.Third(),"stale native callback enabled local body");
    }
    CameraFixture f;f.Call();std::vector<unsigned char> newPawn(0x4000);std::memcpy(newPawn.data(),f.pawn.data(),newPawn.size());
    f.Set(f.page,0xe0,At(newPawn));f.Set(f.page,0xf0,std::uint32_t{0x18002});
    f.Set(f.controller,game_access::field::CCSPlayerController::m_hPlayerPawn,std::uint32_t{0x18002});
    f.Set(f.services,0x38,At(newPawn));Tick();
    Check(cameraOwned&&ownedPawn==At(newPawn)&&ownedHandle==0x18002&&!f.Third(),"pawn/full-serial change retained old camera ownership/mode");
    f.Call();Check(f.Third(),"replacement pawn did not get a current local camera");
    std::vector<unsigned char> replacement(0x1000);Put(At(replacement)+view_bindings::kThirdPerson,false);
    f.Set(f.globals,view_bindings::kInputPointer,At(replacement));Tick();
    Check(cameraOwned&&ownedInput==At(replacement)&&!f.Third(),"new input inherited old third-person mode");
    f.Call();Check(f.Third(),"replacement input did not reacquire local camera");
    f.Set(f.globals,view_bindings::kInputPointer,Address{});thirdRequested=false;Tick();
    Check(!cameraOwned,"null input teardown kept stale camera ownership");
}
void LevelAndOriginRollback()
{
    CameraFixture f;f.Call();OnLevelShutdown();
    Check(!cameraOwned&&!f.Third()&&thirdRequested,"level teardown did not restore mode/preserve requested setting");
    hvh_input::LevelChanged();f.Call();Check(cameraOwned&&f.Third()&&ownedEpoch==2,"new map did not reacquire camera under current epoch");
    thirdRequested=false;Tick();f.SetThird(true);thirdRequested=true;f.Call();OnLevelShutdown();
    Check(!cameraOwned&&f.Third()&&thirdRequested,"level teardown lost preexisting native third-person mode");
    f.SetThird(false);thirdRequested=true;
    auto* allocation=static_cast<unsigned char*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    Check(allocation!=nullptr,"rollback page allocation failed");
    struct Free{unsigned char* pointer;~Free(){VirtualFree(pointer,0,MEM_RELEASE);}} free{allocation};
    const Address view=reinterpret_cast<Address>(allocation+0xb00);
    const Vector original{11,22,33};for(auto offset:view_bindings::kViewOrigins)Put(view+offset,original);
    DWORD previous{};Check(VirtualProtect(allocation+4096,4096,PAGE_READONLY,&previous)!=0,"rollback protection failed");
    f.skipOriginalOrigins=true;f.Call(view);
    Check(!cameraOwned&&!f.Third(),"failed origin commit did not restore owned input mode");f.Origins(original,view);
    Check(VirtualProtect(allocation+4096,4096,previous,&previous)!=0,"rollback protection restore failed");
}
void OriginalViewIdentityWindow()
{
    for(const bool replacePawn:{false,true})
    {
        CameraFixture f;f.Call();
        const auto eyes=f.eyes,filters=f.filters,traces=f.traces;
        f.visible={0,90,5};
        if(replacePawn){f.replacementPawn=f.pawn;f.pawnInsideOriginal=true;}
        else f.epochInsideOriginal=true;
        f.Call();
        Check(f.eyes==eyes&&f.filters==filters&&f.traces==traces,
            "stale native view reached eye/collision callbacks after pawn/map changed inside original");
        Check(!cameraOwned&&!f.Third(),"old native view reacquired camera ownership after original changed identity");
        f.Origins(f.eye);f.NativeFieldsUnchanged();
        // Reacquire only after the next original view was prepared for the new
        // pawn/map. Its independent fresh mouse angle must determine the offset.
        f.visible={0,-90,3};f.Call();
        Check(cameraOwned&&f.Third()&&f.eyes==eyes+1&&f.traces==traces+1,
            "current next-frame native view failed to reacquire camera after identity change");
        if(replacePawn)Check(ownedPawn==At(f.replacementPawn)&&ownedHandle==0x18002,"camera did not adopt current full pawn identity");
        else Check(ownedEpoch==2,"camera did not adopt the new map epoch");
        f.Origins({200,130,70});f.NativeFieldsUnchanged();
    }
}
void ShutdownAndAspect()
{
    CameraFixture f;f.Call();Check(cameraOwned&&f.Third(),"shutdown fixture camera not active");
    std::thread frame([]{while(running.load())std::this_thread::yield();Tick();});
    const bool stopped=Stop();frame.join();
    Check(stopped&&!cameraOwned&&!f.Third()&&!cameraReady&&cameraCallbacks==0,"stop failed to wait for game-thread camera restoration");
    running=true;aspectRequested=false;
    Check(AspectHook(nullptr,1600,1000)==1.6f,"native aspect changed while disabled");
    aspectRequested=true;aspect=4.f/3;
    Check(AspectHook(nullptr,1600,1000)==4.f/3,"requested aspect override not applied");
    running=false;Check(AspectHook(nullptr,1600,1000)==1.6f&&aspectCallbacks==0,"aspect/callback scope not restored on stop");
    Check(f.aspects==3,"aspect override skipped the original native callback");
}
}

int main()
{
    try
    {
        FreshMouseAndRestrictedServer();CollisionAndMalformedNative();DistanceAndModeOwnership();
        NativeMutationAndReplacement();OriginalViewIdentityWindow();LevelAndOriginRollback();ShutdownAndAspect();
        std::cout<<"PASS: local traced third person, fresh mouse/restricted-server views, hull/filter ABI, collision, native faults, rollback, ownership/map lifecycle and aspect\n";
        return 0;
    }
    catch(const std::exception& e){std::cerr<<"camera regression failed: "<<e.what()<<'\n';return 1;}
}
