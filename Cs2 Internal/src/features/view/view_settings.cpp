#include "core/user_defaults.hpp"
#include "platform/windows.h"
#include "features/view/view_settings.h"
#include "core/module_digest.hpp"
#include "game/game_access.h"
#include "game/sdk/ViewBindings.h"
#include "features/combat/detail/hvh_input.hpp"
#include "imgui/imgui.h"
#include "minhook/include/MinHook.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <cmath>
#include <numbers>
#include <limits>

namespace view_settings
{
    namespace
    {
        using namespace game_access;
        struct Vector {float x{},y{},z{};};
        struct Hull {Vector mins,maxs;std::byte pad[0x10]{};std::uint8_t type{2};std::byte tail[7]{};};
        struct Hit {std::byte beforeEnd[0x84]{};Vector end;std::byte beforeFraction[0x1c]{};
            float fraction{NAN};std::byte beforeSolid[0xb]{};bool allSolid{};std::byte tail[4]{};};
        static_assert(sizeof(Vector)==12&&sizeof(Hull)==0x30&&sizeof(Hit)==0xc0);
        static_assert(offsetof(Hit,fraction)==0xac&&offsetof(Hit,allSolid)==0xbb);
        using OverrideFn = void(__fastcall *)(Address,Address);
        using EyeFn = Address(__fastcall *)(Address,Vector*);
        using FilterFn = void(__fastcall *)(void*,Address,std::uint64_t,std::uint8_t,int);
        using TraceFn = bool(__fastcall *)(Address,const Hull*,const Vector*,const Vector*,void*,Hit*);
        using AspectFn = float(__fastcall *)(void *, int, int);
        OverrideFn originalOverride = nullptr;
        EyeFn shootPosition = nullptr;
        FilterFn cameraFilter = nullptr;
        TraceFn cameraTrace = nullptr;
        void *cameraTarget = nullptr;
        bool cameraHookCreated = false;
        AspectFn originalAspect = nullptr;
        void *aspectTarget = nullptr;
        bool aspectHookCreated = false;
        std::atomic<bool> running{false}, thirdRequested{user_defaults::ThirdPerson}, aspectRequested{user_defaults::AspectEnabled}, cameraOwned{false};
        std::atomic<bool> cameraReady{false}, aspectReady{false};
        std::atomic<float> aspect{user_defaults::Aspect},cameraDistance{user_defaults::CameraDistance};
        std::atomic<unsigned> aspectCallbacks{0},cameraCallbacks{0};
        bool originalThird = false;
        Address ownedInput{},ownedList{},ownedPawn{};
        std::uint32_t ownedHandle{0xffffffffu};
        unsigned ownedEpoch{};
        std::mutex statusMutex,cameraMutex;
        std::string cameraError, aspectError, cameraStatus;
        void CameraStatus(const char *value)
        {
            std::lock_guard lock(statusMutex);
            cameraStatus = value;
        }
        bool EntryMatches(Address base, const skin_bindings::Function &binding)
        {
            std::array<std::uint8_t, 24> entry{};
            return base && Copy(base + binding.rva, entry.data(), entry.size()) && entry == binding.entry;
        }
        bool EngineMatches(HMODULE module)
        {
            return native_startup::MatchesModule(reinterpret_cast<Address>(module),view_bindings::kEngineSha256);
        }
        bool ThirdState(Address &input,bool &value)
        {
            input = 0;
            return Read(verifiedClient.load() + view_bindings::kInputPointer, input) && input &&
                   Read(input + view_bindings::kThirdPerson, value);
        }
        bool Finite(Vector value)
        {
            return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z)&&
                std::abs(value.x)<=1e7f&&std::abs(value.y)<=1e7f&&std::abs(value.z)<=1e7f;
        }
        void ForgetCamera()
        {
            cameraOwned=false;ownedInput=ownedList=ownedPawn=0;ownedHandle=0xffffffffu;ownedEpoch=0;
        }
        bool RestoreCamera()
        {
            if(!cameraOwned)return true;
            Address input{};bool current{};
            if(!Read(verifiedClient.load()+view_bindings::kInputPointer,input))return false;
            // An input replacement or map teardown ends ownership. Never write
            // an old address or copy its camera mode into a new session.
            if(!input||input!=ownedInput||ownedEpoch!=hvh_input::worldEpoch.load()) {ForgetCamera();return true;}
            if(!Read(input+view_bindings::kThirdPerson,current))return false;
            if(current!=originalThird&&!Write(input+view_bindings::kThirdPerson,originalThird))return false;
            ForgetCamera();return true;
        }
        bool OwnCamera(const LocalPlayer& player)
        {
            Address input{};bool current{};
            if(!ThirdState(input,current))return false;
            const auto epoch=hvh_input::worldEpoch.load();
            if(cameraOwned&&(input!=ownedInput||player.list!=ownedList||player.pawn!=ownedPawn||
               player.handle!=ownedHandle||epoch!=ownedEpoch)) {
                if(input==ownedInput&&epoch==ownedEpoch&&!RestoreCamera())return false;
                else ForgetCamera();
                if(!ThirdState(input,current))return false;
            }
            if(!cameraOwned){originalThird=current;ownedInput=input;ownedList=player.list;
                ownedPawn=player.pawn;ownedHandle=player.handle;ownedEpoch=epoch;cameraOwned=true;}
            return true;
        }
        bool CurrentPlayer(const LocalPlayer& before)
        {
            const auto after=ReadLocalPlayer(verifiedClient.load());
            return after.state==PlayerState::Alive&&after.list==before.list&&after.controller==before.controller&&
                after.pawn==before.pawn&&after.handle==before.handle&&after.team==before.team;
        }
        __declspec(noinline) bool ShootEye(Address pawn,Vector &eye)
        {
            Address services{},owner{};
            if(!shootPosition||!Read(pawn+field::C_BasePlayerPawn::m_pWeaponServices,services)||!services||
               !Read(services+0x38,owner)||owner!=pawn)return false;
            __try
            {
                Vector value{NAN,NAN,NAN};
                if(shootPosition(services,&value)!=reinterpret_cast<Address>(&value)||!Finite(value))return false;
                eye=value;return true;
            }
            __except(EXCEPTION_EXECUTE_HANDLER){return false;}
        }
        __declspec(noinline) bool TraceCamera(Address pawn,Vector eye,Vector desired,Vector &position)
        {
            Address manager{};
            if(!Finite(eye)||!Finite(desired)||!cameraFilter||!cameraTrace||
               !Read(verifiedClient.load()+view_bindings::kTraceManager,manager)||!manager)return false;
            __try
            {
                alignas(8) std::array<std::byte,0x48> filter{};
                cameraFilter(filter.data(),pawn,std::uint64_t{0x80},3,15);
                // Match native camera B63291..B6337D: exclude the same
                // pawn/hitbox collision categories after generic initialization.
                std::uint64_t exclusions{};
                std::memcpy(&exclusions,filter.data()+0x10,sizeof(exclusions));exclusions|=0xc0000;
                std::memcpy(filter.data()+0x10,&exclusions,sizeof(exclusions));
                Hull hull;hull.mins={-6,-6,-6};hull.maxs={6,6,6};
                Hit hit;
                cameraTrace(manager,&hull,&eye,&desired,filter.data(),&hit);
                if(!std::isfinite(hit.fraction)||hit.fraction<0||hit.fraction>1||!Finite(hit.end))return false;
                const float dx=desired.x-eye.x,dy=desired.y-eye.y,dz=desired.z-eye.z;
                const float length=std::sqrt(dx*dx+dy*dy+dz*dz);
                if(!std::isfinite(length)||length<.01f)return false;
                const Vector expected{eye.x+dx*hit.fraction,eye.y+dy*hit.fraction,eye.z+dz*hit.fraction};
                const float errorX=expected.x-hit.end.x,errorY=expected.y-hit.end.y,errorZ=expected.z-hit.end.z;
                if(!hit.allSolid&&errorX*errorX+errorY*errorY+errorZ*errorZ>4.f)return false;
                const float fraction=hit.allSolid?0.f:std::max(0.f,hit.fraction-(hit.fraction<1?2.f/length:0.f));
                position={eye.x+dx*fraction,eye.y+dy*fraction,eye.z+dz*fraction};return Finite(position);
            }
            __except(EXCEPTION_EXECUTE_HANDLER){return false;}
        }
        bool SetOrigin(Address view,const Vector &position)
        {
            std::array<Vector,view_bindings::kViewOrigins.size()> before{};
            for(std::size_t i=0;i<before.size();++i)
                if(!Read(view+view_bindings::kViewOrigins[i],before[i])||!Finite(before[i]))return false;
            for(std::size_t i=0;i<before.size();++i)
                if(!Write(view+view_bindings::kViewOrigins[i],position)){
                    for(std::size_t j=0;j<=i;++j)Write(view+view_bindings::kViewOrigins[j],before[j]);return false;}
            return true;
        }
        void __fastcall OverrideHook(Address self,Address view)
        {
            struct Scope {Scope(){++cameraCallbacks;}~Scope(){--cameraCallbacks;}} scope;
            Address preparedInput{};
            LocalPlayer preparedPlayer;
            unsigned preparedEpoch{};
            {
                std::lock_guard lock(cameraMutex);
                const auto player=ReadLocalPlayer(verifiedClient.load());
                if(running&&thirdRequested&&cameraReady&&player.state==PlayerState::Alive&&OwnCamera(player)) {
                    // Native third-person view calculation consumes cached input
                    // camera angles. They stop updating on restricted servers.
                    // Obtain the fresh native first-person view before applying
                    // our local offset; enable body visibility only afterwards.
                    if(Write(ownedInput+view_bindings::kThirdPerson,false)){
                        preparedInput=ownedInput;preparedPlayer=player;preparedEpoch=ownedEpoch;}
                } else RestoreCamera();
            }
            originalOverride(self,view);
            std::lock_guard lock(cameraMutex);
            const auto player=ReadLocalPlayer(verifiedClient.load());
            if(!running||!thirdRequested||!cameraReady||player.state!=PlayerState::Alive){RestoreCamera();return;}
            Vector eye,angles;
            if(!preparedInput||preparedEpoch!=hvh_input::worldEpoch.load()||!CurrentPlayer(preparedPlayer)||
               !OwnCamera(player)||preparedInput!=ownedInput||!ShootEye(player.pawn,eye)||
               !Read(view+view_bindings::kViewAngles,angles)||!Finite(angles)){
                RestoreCamera();CameraStatus("Waiting for the local camera view.");return;}
            const float configured=cameraDistance.load();
            const float distance=std::isfinite(configured)?std::clamp(configured,35.f,200.f):100.f;
            const double pitch=angles.x*std::numbers::pi/180.,yaw=angles.y*std::numbers::pi/180.;
            const Vector desired{eye.x-static_cast<float>(std::cos(pitch)*std::cos(yaw))*distance,
                eye.y-static_cast<float>(std::cos(pitch)*std::sin(yaw))*distance,
                eye.z+static_cast<float>(std::sin(pitch))*distance};
            Vector position;
            Address input{};bool state{};
            if(!TraceCamera(player.pawn,eye,desired,position)||!CurrentPlayer(player)||
               !ThirdState(input,state)||input!=ownedInput||ownedEpoch!=hvh_input::worldEpoch.load()||!running||!thirdRequested){
                RestoreCamera();CameraStatus("Camera collision trace unavailable.");return;}
            const auto delta=Vector{position.x-eye.x,position.y-eye.y,position.z-eye.z};
            const bool outside=delta.x*delta.x+delta.y*delta.y+delta.z*delta.z>64.f;
            if(!Write(ownedInput+view_bindings::kThirdPerson,outside)||!SetOrigin(view,position)){
                RestoreCamera();CameraStatus("Could not update the local camera view.");return;}
            CameraStatus(outside?"Third person active.":"Camera pulled forward by an obstacle.");
        }
        float __fastcall AspectHook(void *self, int width, int height)
        {
            struct Scope
            {
                Scope()
                {
                    ++aspectCallbacks;
                }
                ~Scope()
                {
                    --aspectCallbacks;
                }
            } scope;
            const float original = originalAspect(self, width, height);
            return running.load() && aspectRequested.load() ? aspect.load() : original;
        }
    } // namespace
    void Install()
    {
        const auto client = verifiedClient.load();
        std::lock_guard lock(statusMutex);
        cameraError.clear();
        aspectError.clear();
        cameraStatus.clear();
        cameraReady=false;
        if(client&&(cameraHookCreated||EntryMatches(client,view_bindings::OverrideView))&&
           EntryMatches(client,view_bindings::ShootPosition)&&EntryMatches(client,view_bindings::CameraTrace)&&
           EntryMatches(client,view_bindings::CameraFilter))
        {
            shootPosition=reinterpret_cast<EyeFn>(client+view_bindings::ShootPosition.rva);
            cameraFilter=reinterpret_cast<FilterFn>(client+view_bindings::CameraFilter.rva);
            cameraTrace=reinterpret_cast<TraceFn>(client+view_bindings::CameraTrace.rva);
            cameraTarget=reinterpret_cast<void*>(client+view_bindings::OverrideView.rva);
            if(!cameraHookCreated)
                cameraHookCreated=MH_CreateHook(cameraTarget,reinterpret_cast<void*>(OverrideHook),
                    reinterpret_cast<void**>(&originalOverride))==MH_OK;
            if(cameraHookCreated){const auto result=MH_EnableHook(cameraTarget);
                cameraReady=result==MH_OK||result==MH_ERROR_ENABLED;}
        }
        if(!cameraReady)
            cameraError = "Third person unavailable for this game build.";
        const auto engine = GetModuleHandleW(L"engine2.dll");
        if (EngineMatches(engine) &&
            (aspectHookCreated || EntryMatches(reinterpret_cast<Address>(engine), view_bindings::AspectRatio)))
        {
            aspectTarget = reinterpret_cast<void *>(reinterpret_cast<Address>(engine) + view_bindings::AspectRatio.rva);
            if (!aspectHookCreated)
                aspectHookCreated = MH_CreateHook(aspectTarget, reinterpret_cast<void *>(AspectHook),
                                                  reinterpret_cast<void **>(&originalAspect)) == MH_OK;
            if (aspectHookCreated)
            {
                const auto result = MH_EnableHook(aspectTarget);
                aspectReady = result == MH_OK || result == MH_ERROR_ENABLED;
            }
        }
        if (!aspectReady)
            aspectError = "Aspect ratio unavailable for this game build.";
        running = true;
    }
    void Tick()
    {
        if (!cameraReady)
            return;
        std::lock_guard lock(cameraMutex);
        const auto player = ReadLocalPlayer(verifiedClient.load());
        const bool requested = running && thirdRequested;
        if (requested && player.state == PlayerState::Alive)
        {
            if(!OwnCamera(player))CameraStatus("Camera unavailable.");
        }
        else
        {
            if(!RestoreCamera()){CameraStatus("Could not restore camera.");return;}
            CameraStatus(requested ? "Spawn to use third person." : "");
        }
    }
    void Draw()
    {
        bool third = thirdRequested;
        if (ImGui::Checkbox("Third person", &third))
            thirdRequested = third && cameraReady;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Camera with wall collision. Mouse aim stays unchanged.");
        if(third){float distance=cameraDistance;
            if(ImGui::SliderFloat("Distance",&distance,35.f,200.f,"%.0f units"))cameraDistance=distance;}
        bool overrideRatio = aspectRequested;
        if (ImGui::Checkbox("Aspect ratio", &overrideRatio))
            aspectRequested = overrideRatio && aspectReady;
        if (overrideRatio)
        {
            float ratio = aspect;
            ImGui::SetNextItemWidth(260);
            if (ImGui::SliderFloat("Ratio", &ratio, .5f, 3.f, "%.3f"))
                aspect = ratio;
            const char *names[] = {"4:3", "16:9", "16:10", "21:9"};
            const float values[] = {4.f / 3, 16.f / 9, 16.f / 10, 21.f / 9};
            for (int i = 0; i < 4; ++i)
            {
                if (i)
                    ImGui::SameLine();
                if (ImGui::Button(names[i]))
                    aspect = values[i];
            }
        }
        if (ImGui::Button("Reset camera"))
        {
            thirdRequested = false;
            aspectRequested = false;
            aspect = 4.f / 3.f;
            cameraDistance = 100.f;
        }
    }
    void OnLevelShutdown()
    {
        // Called before the shared map epoch advances, while the old input is
        // still alive. Preserve the user's requested setting for the next map.
        std::lock_guard lock(cameraMutex);
        RestoreCamera();
    }
    bool Stop()
    {
        running = false;
        thirdRequested = false;
        aspectRequested = false;
        const auto deadline = GetTickCount64() + 2000;
        while (cameraOwned && GetTickCount64() < deadline)
            Sleep(1);
        if (cameraOwned)
            return false;
        if(cameraHookCreated){const auto result=MH_DisableHook(cameraTarget);
            if(result!=MH_OK&&result!=MH_ERROR_DISABLED)return false;}
        if (aspectHookCreated)
        {
            const auto result = MH_DisableHook(aspectTarget);
            if (result != MH_OK && result != MH_ERROR_DISABLED)
                return false;
        }
        const auto callbackDeadline=GetTickCount64()+2000;
        while ((aspectCallbacks||cameraCallbacks)&&GetTickCount64()<callbackDeadline)
            Sleep(1);
        if(aspectCallbacks||cameraCallbacks)return false;
        aspectReady = false;
        cameraReady = false;
        return true;
    }
} // namespace view_settings
