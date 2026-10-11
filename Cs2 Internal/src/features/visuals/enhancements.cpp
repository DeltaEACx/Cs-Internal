#include "core/user_defaults.hpp"
#include "enhancements.hpp"
#include "game/combat_access.h"
#include "game/sdk/ViewBindings.h"
#include "game/sdk/VisualBindings.h"
#include <bit>
#include <deque>
#include <unordered_map>

namespace visuals::enhancements
{
    namespace
    {
        using Address = game_access::Address;
        using Vec3 = math::vector3;
        std::mutex mutex;
        Configuration config=user_defaults::Enhancements(Configuration{});
        std::atomic<bool> stopping{}, stopped{true};
        Address cvarSystem{},lightAlloc{},lightTime{},lightManager{},lightEntry{};
        constexpr unsigned lightKey=0x4445434c;
        std::atomic<Address> lightScene{};
        std::mutex lightMutex;
        Vec3 linearLight{};
        __declspec(noinline) Address AllocateLight(Address manager){
            __try{return memory::call<Address>(lightAlloc,manager,lightKey,0);}
            __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
        }
        __declspec(noinline) bool LightTime(float &time){
            __try{memory::call<float*>(lightTime,&time,0);return std::isfinite(time);}
            __except(EXCEPTION_EXECUTE_HANDLER){return false;}
        }
        void RetireLight(){
            const auto current=memory::read<Address>(game_access::verifiedClient.load()+visual_bindings::lightManager);
            if(current&&current==lightManager&&lightEntry&&memory::read<unsigned>(lightEntry+0x24)==lightKey&&lightAlloc)AllocateLight(current);
            lightEntry=lightManager=0;lightScene=0;
        }
        bool ready{};
        struct Ring { Vec3 origin; ULONGLONG born; };
        struct Snapshot
        {
            bool alive{}, scoped{}, third{}, accuracy{};
            float spread{}, inaccuracy{}, speed{};
            Vec3 head{};
            ULONGLONG time{};
            std::vector<Ring> rings;
            std::vector<std::string> spectators;
            std::vector<Vec3> fire, smoke, trajectory;
            int throwable{-1};
        };
        Snapshot snapshot;
        std::deque<float> speeds;
        float scopeAlpha{};
        std::unordered_map<std::uint32_t,bool> grounded;
        std::vector<Ring> rings;
        struct Patch
        {
            Address entity{}, address{};
            std::uint32_t handle{};
            std::array<std::byte,16> original{}, written{};
            unsigned size{}, frame{};
        };
        std::vector<Patch> patches;
        unsigned frameNumber{};
        struct Cvar {Address address{};std::uint32_t original{},written{};std::uint64_t flags{};};
        std::unordered_map<std::string,Cvar> cvars;
        template<class T> T Field(Address object,const char *type,const char *name,T fallback={})
        {
            return combat_access::Field<T>(object,type,name).value_or(fallback);
        }
        std::uint32_t Handle(Address entity)
        {
            const auto identity = memory::read<Address>(entity+0x10);
            return identity ? memory::read<std::uint32_t>(identity+0x10) : 0xffffffffu;
        }
        Vec3 Position(Address entity)
        {
            return Field<Vec3>(Field<Address>(entity,"C_BaseEntity","m_pGameSceneNode"),"CGameSceneNode","m_vecAbsOrigin");
        }
        void Restore(Patch &patch)
        {
            std::array<std::byte,16> current{};
            if (Handle(patch.entity)==patch.handle && memory::copy(current.data(),patch.address,patch.size) &&
                !std::memcmp(current.data(),patch.written.data(),patch.size))
                memory::store(patch.address,patch.original.data(),patch.size);
        }
        template<class T> void SetField(Address entity,Address address,const T &value)
        {
            static_assert(sizeof(T)<=16);
            if (!entity || !address) return;
            const auto handle=Handle(entity);
            if (handle==0xffffffffu) return;
            auto found=std::find_if(patches.begin(),patches.end(),[&](const auto &p){return p.address==address;});
            if (found!=patches.end() && found->handle!=handle) {patches.erase(found);found=patches.end();}
            if (found==patches.end())
            {
                if(patches.size()>=4096) return;
                Patch patch;patch.entity=entity;patch.address=address;patch.handle=handle;patch.size=sizeof(T);
                if(!memory::copy(patch.original.data(),address,sizeof(T)))return;
                patches.push_back(patch);found=std::prev(patches.end());
            }
            std::array<std::byte,16> current{};
            if(memory::copy(current.data(),address,sizeof(T))&&std::memcmp(current.data(),found->written.data(),sizeof(T)))
                std::memcpy(found->original.data(),current.data(),sizeof(T));
            if(memory::safe_write(address,value))
            {
                std::memcpy(found->written.data(),&value,sizeof(T));found->frame=frameNumber;
            }
        }
        template<class T> void SetField(Address entity,const char *type,const char *name,const T &value)
        {
            const auto offset=systems::schemas::lookup(type,fnv1a::runtime_hash(name));
            if(offset)SetField(entity,entity+offset,value);
        }
        Address FindCvar(const char *name)
        {
            if(!cvarSystem)return 0;
            const auto count=memory::read<std::uint16_t>(cvarSystem+0x4a)&0x7fff;
            const auto entries=memory::read<Address>(cvarSystem+0x50);
            auto index=memory::read<std::uint16_t>(cvarSystem+0x58);
            for(unsigned i=0;entries && index!=0xffff && i<static_cast<unsigned>(count);++i)
            {
                if(index>=count)return 0;
                const auto entry=entries+index*0x10;
                const auto variable=memory::read<Address>(entry);
                if(variable && memory::read_string(memory::read<Address>(variable),128)==name)return variable;
                index=memory::read<std::uint16_t>(entry+0xa);
            }
            return 0;
        }
        void RestoreCvar(Cvar &variable)
        {
            if(memory::read<std::uint32_t>(variable.address+0x58)==variable.written)
                memory::safe_write(variable.address+0x58,variable.original);
            const auto flags=memory::safe_read<std::uint64_t>(variable.address+0x30);
            if(flags)memory::safe_write(variable.address+0x30,(*flags&~2ull)|(variable.flags&2ull));
        }
        void Override(const char *name,bool active,float value)
        {
            auto found=cvars.find(name);
            if(!active)
            {
                if(found!=cvars.end()){RestoreCvar(found->second);cvars.erase(found);}return;
            }
            if(found==cvars.end())
            {
                const auto address=FindCvar(name);
                const auto original=memory::safe_read<std::uint32_t>(address+0x58);
                const auto flags=memory::safe_read<std::uint64_t>(address+0x30);
                if(!address || !original || !flags || !std::isfinite(std::bit_cast<float>(*original)))return;
                found=cvars.emplace(name,Cvar{address,*original,*original,*flags}).first;
            }
            auto &variable=found->second;
            const auto current=memory::safe_read<std::uint32_t>(variable.address+0x58);
            if(current&&*current!=variable.written)variable.original=*current;
            if(memory::safe_write(variable.address+0x58,value))
            {
                variable.written=std::bit_cast<std::uint32_t>(value);
                const auto flags=memory::read<std::uint64_t>(variable.address+0x30);
                memory::safe_write(variable.address+0x30,flags&~2ull);
            }
        }
        void Glow(Address entity,color tint)
        {
            const auto offset=systems::schemas::lookup("C_BaseModelEntity","m_Glow"_hash);
            const auto type=systems::schemas::lookup("CGlowProperty","m_iGlowType"_hash);
            const auto enabled=systems::schemas::lookup("CGlowProperty","m_bGlowing"_hash);
            const auto packed=systems::schemas::lookup("CGlowProperty","m_glowColorOverride"_hash);
            if(!offset || !type || !enabled || !packed)return;
            const auto glow=entity+offset;
            SetField(entity,glow+type,3);
            SetField(entity,glow+enabled,true);
            const unsigned rgba=tint.r|(tint.g<<8)|(tint.b<<16)|(unsigned(tint.a)<<24);
            SetField(entity,glow+packed,rgba);
        }
        void Predict(Snapshot &result,Address pawn,Address weapon,int definition)
        {
            if(definition<43 || definition>48 || !PATTERN(patterns::trace_ray))return;
            static ULONGLONG next{};static Address last{};static std::vector<Vec3> cached;
            if(last==weapon&&result.time<next){result.trajectory=cached;return;}
            last=weapon;next=result.time+32;cached.clear();
            Vec3 angles;
            if(!combat_access::ReadViewAngles(angles))return;
            angles.x-= (90.f-std::abs(angles.x))*10.f/90.f;
            const float pitch=angles.x*std::numbers::pi_v<float>/180.f,yaw=angles.y*std::numbers::pi_v<float>/180.f;
            const Vec3 direction{std::cos(pitch)*std::cos(yaw),std::cos(pitch)*std::sin(yaw),-std::sin(pitch)};
            const auto strength=std::clamp(Field<float>(weapon,"C_BaseCSGrenade","m_flThrowStrength",1),0.f,1.f);
            auto start=Position(pawn)+Field<Vec3>(pawn,"C_BaseModelEntity","m_vecViewOffset");
            start.z+=strength*12-12;
            const auto launch=systems::g_tracing.trace(start,start+direction*22,pawn);
            if(launch.all_solid||!combat_access::Finite(launch.end_pos))return;
            auto position=launch.end_pos-direction*6;
            const auto data=memory::read<Address>(weapon+game_access::field::C_BaseEntity::m_nSubclassID+8);
            const auto velocity=std::clamp(Field<float>(data,"CCSWeaponBaseVData","m_flThrowVelocity",750)*.9f,15.f,750.f);
            auto motion=direction*(velocity*(strength*.7f+.3f))+Field<Vec3>(pawn,"C_BaseEntity","m_vecAbsVelocity")*1.25f;
            const float gravity=combat_access::Convar("sv_gravity",800)*.4f,dt=1.f/32.f;
            result.trajectory.push_back(position);
            int bounces=0;
            for(int step=0;step<96 && bounces<12;++step)
            {
                const Vec3 delta{motion.x*dt,motion.y*dt,(motion.z-gravity*dt*.5f)*dt};
                motion.z-=gravity*dt;
                const auto trace=systems::g_tracing.trace(position,position+delta,pawn);
                if(!std::isfinite(trace.fraction) || trace.all_solid)break;
                position=trace.end_pos;
                if(trace.fraction<1)
                {
                    ++bounces;
                    const auto dot=motion.x*trace.normal.x+motion.y*trace.normal.y+motion.z*trace.normal.z;
                    motion=(motion-trace.normal*(dot*2))*.45f;
                    position=position+trace.normal*.1f;
                    if(trace.normal.z>.7f && (definition==46 || definition==48 || motion.length_sqr()<400))
                    {result.trajectory.push_back(position);break;}
                }
                result.trajectory.push_back(position);
                if((definition==43 || definition==44) && (step+1)*dt>=1.5f)break;
            }
            cached=result.trajectory;
        }
        ImU32 U32(color c,float alpha=1)
        {return IM_COL32(c.r,c.g,c.b,static_cast<int>(std::clamp(alpha,0.f,1.f)*c.a));}
        void Circle(ImDrawList *draw,const Vec3 &origin,float radius,ImU32 tint,float thickness=2)
        {
            std::optional<ImVec2> previous;
            for(int i=0;i<=48;++i)
            {
                const auto angle=float(i)*2*std::numbers::pi_v<float>/48;
                const auto p=systems::g_view.project_full(origin+Vec3{std::cos(angle)*radius,std::sin(angle)*radius,1});
                if(p.w<=.01f){previous.reset();continue;}
                const ImVec2 point{p.screen.x,p.screen.y};
                if(previous)draw->AddLine(*previous,point,tint,thickness);
                previous=point;
            }
        }
        bool Color(const char *name,color &c)
        {
            float rgba[]{c.r/255.f,c.g/255.f,c.b/255.f,c.a/255.f};
            if(!ImGui::ColorEdit4(name,rgba,ImGuiColorEditFlags_NoInputs))return false;
            c={std::uint8_t(rgba[0]*255),std::uint8_t(rgba[1]*255),std::uint8_t(rgba[2]*255),std::uint8_t(rgba[3]*255)};
            return true;
        }
    }
    Configuration Get(){std::lock_guard lock(mutex);return config;}
    void Set(Configuration value)
    {
        const auto bound=[](float x,float lo,float hi,float fallback){return std::isfinite(x)?std::clamp(x,lo,hi):fallback;};
        value.gravityScale=bound(value.gravityScale,-2,3,0);value.flashOpacity=bound(value.flashOpacity,0,100,30);
        value.spreadScale=bound(value.spreadScale,.5f,2,1);value.crosshairSize=bound(value.crosshairSize,1,20,4);
        value.scopeLength=bound(value.scopeLength,10,500,120);value.scopeGap=bound(value.scopeGap,0,50,12);
        value.scopeThickness=bound(value.scopeThickness,.5f,5,1.5f);value.crosshairStyle=std::clamp(value.crosshairStyle,0,2);
        value.scopeStyle=std::clamp(value.scopeStyle,0,1);value.hatStyle=std::clamp(value.hatStyle,0,1);
        value.jumpRadius=bound(value.jumpRadius,10,200,60);value.jumpDuration=bound(value.jumpDuration,.2f,5,1.5f);
        value.lightRadius=bound(value.lightRadius,20,1000,250);value.lightHeight=bound(value.lightHeight,-50,200,50);
        value.opacity=bound(value.opacity,5,100,35);value.viewmodelFov=bound(value.viewmodelFov,40,100,68);
        for(auto &offset:value.viewmodelOffset)offset=bound(offset,-10,10,0);
        for(auto &throwable:value.throwables){throwable.width=bound(throwable.width,.5f,5,2);throwable.markerRadius=bound(throwable.markerRadius,0,40,12);}
        std::lock_guard lock(mutex);config=value;
    }
    bool HideLocalLegs(std::uintptr_t owner)
    {
        const auto value=Get();if(!value.removeLegs||stopping)return false;
        const auto local=game_access::ReadLocalPlayer(game_access::verifiedClient.load());
        const auto input=memory::read<Address>(game_access::verifiedClient.load()+view_bindings::kInputPointer);
        return local.state==game_access::PlayerState::Alive&&owner==local.pawn&&input&&!memory::read<bool>(input+view_bindings::kThirdPerson);
    }
    bool Initialize()
    {
        ready=game::module_matches("client.dll",skin_bindings::kClientSha256);
        cvarSystem=lightAlloc=lightTime=0;
        if(ready){
            const auto base=game_access::verifiedClient.load();
            const auto bind=[&](const skin_bindings::Function &fn){std::array<std::uint8_t,24> bytes{};return memory::copy(bytes.data(),base+fn.rva,bytes.size())&&bytes==fn.entry?base+fn.rva:0;};
            lightAlloc=bind(visual_bindings::lightAlloc);lightTime=bind(visual_bindings::lightTime);
        }
        if(ready && game::module_matches("tier0.dll","00ecf3e3d33f9036f3fd6e69f2c89510148e1a6202dd8bdea592fecc7891cfd4"))
        {
            const auto create=reinterpret_cast<void*(*)(const char*,int*)>(GetProcAddress(GetModuleHandleA("tier0.dll"),"CreateInterface"));
            if(create)cvarSystem=reinterpret_cast<Address>(create("VEngineCvar007",nullptr));
        }
        stopping=false;stopped=!ready;return ready;
    }
    void Frame()
    {
        if(stopping){Reset();stopped=true;return;}
        if(!ready)return;
        const auto value=Get();Snapshot result;result.time=GetTickCount64();++frameNumber;
        const auto local=game_access::ReadLocalPlayer(game_access::verifiedClient.load());
        const bool inWorld=local.list && local.pawn;
        if(inWorld&&value.dynamicLight&&local.state==game_access::PlayerState::Alive&&lightAlloc&&lightTime){
            const auto manager=memory::read<Address>(game_access::verifiedClient.load()+visual_bindings::lightManager);
            if(manager!=lightManager||!lightEntry||memory::read<unsigned>(lightEntry+0x24)!=lightKey){RetireLight();if(manager){lightManager=manager;lightEntry=AllocateLight(manager);}}
            float now{};auto position=Position(local.pawn);position.z+=value.lightHeight;
            if(lightEntry&&combat_access::Finite(position)&&LightTime(now)){
                const unsigned rgb=value.lightColor.r|(unsigned(value.lightColor.g)<<8)|(unsigned(value.lightColor.b)<<16)|(5u<<24);
                if(memory::safe_write(lightEntry+4,position)&&memory::safe_write(lightEntry+0x10,value.lightRadius)&&memory::safe_write(lightEntry+0x14,rgb)&&memory::safe_write(lightEntry+0x18,now+5)&&memory::safe_write(lightEntry+0x1c,0.f)){
                    std::lock_guard lock(lightMutex);linearLight=Vec3{value.lightColor.r/255.f,value.lightColor.g/255.f,value.lightColor.b/255.f}*(32*value.lightRadius);
                    lightScene=memory::read<Address>(lightEntry+0x48);
                }else RetireLight();
            }
        }else RetireLight();
        // crosshair is a bool/integer union value; zero has the same representation.
        Override("crosshair",inWorld&&(value.crosshair||value.removeCrosshair),0.f);
        Override("ragdoll_gravity_scale",inWorld&&value.ragdollGravity,value.gravityScale);
        Override("viewmodel_fov",inWorld&&value.viewmodel,value.viewmodelFov);
        Override("viewmodel_offset_x",inWorld&&value.viewmodel,value.viewmodelOffset[0]);
        Override("viewmodel_offset_y",inWorld&&value.viewmodel,value.viewmodelOffset[1]);
        Override("viewmodel_offset_z",inWorld&&value.viewmodel,value.viewmodelOffset[2]);
        if(inWorld)
        {
            result.alive=local.state==game_access::PlayerState::Alive;
            result.scoped=Field<bool>(local.pawn,"C_CSPlayerPawn","m_bIsScoped");
            Address input=memory::read<Address>(game_access::verifiedClient.load()+view_bindings::kInputPointer);
            result.third=input&&memory::read<bool>(input+view_bindings::kThirdPerson);
            result.speed=Field<Vec3>(local.pawn,"C_BaseEntity","m_vecAbsVelocity").length_2d();
            result.head=Position(local.pawn)+Vec3{0,0,72};
            if(value.hat&&result.third){const auto skeleton=systems::g_bones.get_skeleton(local.pawn);const auto head=skeleton[cstypes::bone_ids::head].position;if(combat_access::Finite(head)&&head.length_sqr()>1)result.head=head+Vec3{0,0,3};}
            if(value.flashLimit)SetField(local.pawn,"C_CSPlayerPawnBase","m_flFlashMaxAlpha",value.flashOpacity*2.55f);
            if(value.localOpacity && result.third && (!value.opacityScopedOnly || result.scoped))
            {
                const auto old=Field<unsigned>(local.pawn,"C_BaseModelEntity","m_clrRender",0xffffffffu);
                SetField(local.pawn,"C_BaseModelEntity","m_clrRender",(old&0xffffff)|(unsigned(value.opacity*2.55f)<<24));
            }
            const auto services=Field<Address>(local.pawn,"C_BasePlayerPawn","m_pWeaponServices");
            const auto weapon=game_access::Resolve(local.list,Field<std::uint32_t>(services,"CPlayer_WeaponServices","m_hActiveWeapon",0xffffffffu));
            if(result.alive && (value.spreadCircle||value.scope))result.accuracy=combat_access::ReadAccuracy(weapon,result.spread,result.inaccuracy);
            if(result.alive && value.grenadePrediction && weapon)
            {
                const auto definition=Field<std::uint16_t>(weapon+game_access::field::C_EconEntity::m_AttributeManager+
                    game_access::field::C_AttributeContainer::m_Item,"C_EconItemView","m_iItemDefinitionIndex");
                result.throwable=ThrowableIndex(definition);
                if(result.throwable>=0&&value.throwables[result.throwable].trajectory)
                    Predict(result,local.pawn,weapon,definition);
            }
            const auto team=Field<std::uint8_t>(local.pawn,"C_BaseEntity","m_iTeamNum");
            for(const auto &entry:systems::g_entities.get_by_type(systems::entities::type::player))
            {
                if(systems::g_entities.get_by_index(entry.index)!=entry.ptr)continue;
                const auto pawn=systems::g_entities.lookup(Field<std::uint32_t>(entry.ptr,"CBasePlayerController","m_hPawn",0xffffffffu));
                if(!pawn)continue;
                const auto health=Field<int>(pawn,"C_BaseEntity","m_iHealth");
                const bool enemy=Field<std::uint8_t>(pawn,"C_BaseEntity","m_iTeamNum")!=team;
                if(health>0 && value.radar)
                {
                    const auto spotted=systems::schemas::lookup("C_CSPlayerPawn","m_entitySpottedState"_hash);
                    const auto flag=systems::schemas::lookup("EntitySpottedState_t","m_bSpotted"_hash);
                    if(spotted&&flag)SetField(pawn,pawn+spotted+flag,true);
                }
                if((health<=0&&value.corpseGlow) || (health>0 &&
                    (pawn==local.pawn ? value.localGlow&&result.third : enemy?value.enemyGlow:value.teamGlow)))
                    Glow(pawn,enemy?value.enemyColor:value.teamColor);
                if(value.spectators && health<=0 && pawn!=local.pawn)
                {
                    const auto observer=Field<Address>(pawn,"C_BasePlayerPawn","m_pObserverServices");
                    const auto target=systems::g_entities.lookup(Field<std::uint32_t>(observer,"CPlayer_ObserverServices","m_hObserverTarget",0xffffffffu));
                    if(target==local.pawn)
                        result.spectators.push_back(memory::read_string(Field<Address>(entry.ptr,"CCSPlayerController","m_sSanitizedPlayerName"),128));
                }
                if(value.jumpRings && health>0 && (!value.enemyJumpsOnly||enemy))
                {
                    const auto handle=Handle(pawn);const bool ground=(Field<unsigned>(pawn,"C_BaseEntity","m_fFlags")&1)!=0;
                    const auto previous=grounded.find(handle);
                    if(previous!=grounded.end() && previous->second&&!ground && rings.size()<128)
                        rings.push_back({Position(pawn),result.time});
                    grounded[handle]=ground;
                }
            }
            if(value.corpseGlow)for(const auto &entry:systems::g_entities.get_by_type(systems::entities::type::pawn))
                if(systems::g_entities.get_by_index(entry.index)==entry.ptr&&Field<int>(entry.ptr,"C_BaseEntity","m_iHealth")<=0)
                    Glow(entry.ptr,Field<std::uint8_t>(entry.ptr,"C_BaseEntity","m_iTeamNum")==team?value.teamColor:value.enemyColor);
            if(value.itemGlow)
                for(const auto &item:systems::g_entities.get_by_type(systems::entities::type::item))
                    if(systems::g_entities.get_by_index(item.index)==item.ptr &&
                       !systems::g_entities.lookup(Field<std::uint32_t>(item.ptr,"C_BaseEntity","m_hOwnerEntity",0xffffffffu)))
                        Glow(item.ptr,value.itemColor);
            for(const auto &entry:systems::g_entities.get_by_type(systems::entities::type::projectile))
            {
                if(systems::g_entities.get_by_index(entry.index)!=entry.ptr)continue;
                if(entry.schema_hash=="C_SmokeGrenadeProjectile"_hash && Field<bool>(entry.ptr,"C_SmokeGrenadeProjectile","m_bDidSmokeEffect"))
                {
                    if(value.smokeTint)SetField(entry.ptr,"C_SmokeGrenadeProjectile","m_vSmokeColor",
                        Vec3{value.smokeColor.r/255.f,value.smokeColor.g/255.f,value.smokeColor.b/255.f});
                    if(value.smokeArea)result.smoke.push_back(Position(entry.ptr));
                }
                if(value.fireArea && entry.schema_hash=="C_Inferno"_hash)
                {
                    const auto count=std::clamp(Field<int>(entry.ptr,"C_Inferno","m_fireCount"),0,64);
                    const auto positions=systems::schemas::lookup("C_Inferno","m_firePositions"_hash);
                    const auto burning=systems::schemas::lookup("C_Inferno","m_bFireIsBurning"_hash);
                    if(positions&&burning)for(int i=0;i<count;++i)
                        if(memory::read<bool>(entry.ptr+burning+i))
                        {
                            const auto position=memory::read<Vec3>(entry.ptr+positions+i*sizeof(Vec3));
                            if(combat_access::Finite(position)&&position.length_sqr()>1)result.fire.push_back(position);
                        }
                }
            }
        }
        if(!inWorld || !value.jumpRings){rings.clear();grounded.clear();}
        if(grounded.size()>256)grounded.clear();
        std::erase_if(rings,[&](const auto &r){return result.time-r.born>value.jumpDuration*1000;});
        result.rings=rings;
        for(auto it=patches.begin();it!=patches.end();)
            if(it->frame!=frameNumber){Restore(*it);it=patches.erase(it);}else ++it;
        std::lock_guard lock(mutex);snapshot=std::move(result);
    }
    float SpreadRadius(float spread,float inaccuracy,float horizontalFov,float width)
    {
        if(!std::isfinite(spread)||!std::isfinite(inaccuracy)||!std::isfinite(horizontalFov)||!std::isfinite(width)||
           spread<0||inaccuracy<0||horizontalFov<=1||horizontalFov>=179||width<=0)return 0;
        return std::clamp((spread+inaccuracy)*width/(2*std::tan(horizontalFov*std::numbers::pi_v<float>/360)),0.f,width*.45f);
    }
    void Render(const frame_snapshot &frame)
    {
        Configuration value;Snapshot data;
        {std::lock_guard lock(mutex);value=config;data=snapshot;}
        if(GetTickCount64()-data.time>1000){speeds.clear();scopeAlpha=0;return;}
        auto *draw=ImGui::GetBackgroundDrawList();const auto &io=ImGui::GetIO();
        const ImVec2 center{frame.screen_width*.5f,frame.screen_height*.5f};
        const float radius=data.accuracy?SpreadRadius(data.spread,data.inaccuracy,frame.horizontal_fov,frame.screen_width)*value.spreadScale:0;
        if(data.alive&&value.spreadCircle&&radius>1)
        {
            if(value.spreadFill)draw->AddCircleFilled(center,radius,U32(value.accent,.15f),96);
            draw->AddCircle(center,radius,U32(value.accent),96,1.5f);
        }
        if(data.alive&&value.crosshair&&!data.scoped)
        {
            const float size=value.crosshairSize;
            if(value.crosshairStyle==0)draw->AddCircleFilled(center,size,U32(value.accent),24);
            if(value.crosshairStyle==1)
            {draw->AddLine({center.x-size,center.y},{center.x+size,center.y},U32(value.accent),1.5f);
             draw->AddLine({center.x,center.y-size},{center.x,center.y+size},U32(value.accent),1.5f);}
            if(value.crosshairStyle==2)draw->AddCircle(center,size,U32(value.accent),32,1.5f);
        }
        scopeAlpha=std::lerp(scopeAlpha,data.alive&&value.scope&&data.scoped?1.f:0.f,std::clamp(io.DeltaTime*15,0.f,1.f));
        if(scopeAlpha>.01f)
        {
            const float gap=value.scopeStyle==1?0:std::max(value.scopeGap,radius);
            const float length=value.scopeStyle==1?std::max(frame.screen_width,frame.screen_height):value.scopeLength*scopeAlpha;
            for(int i=0;i<4;++i)
            {
                const float angle=i*std::numbers::pi_v<float>*.5f;
                const ImVec2 a{center.x+std::cos(angle)*gap,center.y+std::sin(angle)*gap};
                const ImVec2 b{center.x+std::cos(angle)*(gap+length),center.y+std::sin(angle)*(gap+length)};
                if(value.scopeGlow)for(int w=3;w>0;--w)draw->AddLine(a,b,U32(value.accent,scopeAlpha*.08f),value.scopeThickness+w*3);
                draw->AddLine(a,b,U32(value.accent,scopeAlpha),value.scopeThickness);
            }
        }
        for(const auto &ring:data.rings)
        {
            const float life=float(GetTickCount64()-ring.born)/(value.jumpDuration*1000);
            Circle(draw,ring.origin,value.jumpRadius*(.5f+life*.5f),U32(value.jumpColor,1-life),2);
        }
        for(const auto &point:data.fire)Circle(draw,point,60,IM_COL32(255,120,70,190),1.5f);
        for(const auto &point:data.smoke)Circle(draw,point,144,U32(value.smokeColor,.7f),2);
        const auto path=data.throwable>=0&&data.throwable<value.throwables.size()?value.throwables[data.throwable]:ThrowableSettings{};
        for(size_t i=1;i<data.trajectory.size();++i)
        {
            const auto a=systems::g_view.project_full(data.trajectory[i-1]),b=systems::g_view.project_full(data.trajectory[i]);
            if(a.w>.01f&&b.w>.01f)draw->AddLine({a.screen.x,a.screen.y},{b.screen.x,b.screen.y},U32(path.pathColor),path.width);
        }
        if(!data.trajectory.empty()&&path.markerRadius>0)Circle(draw,data.trajectory.back(),path.markerRadius,U32(path.pathColor),path.width);
        if(value.hat&&data.third)
        {
            Circle(draw,data.head,18,U32(value.accent),2);
            const auto tip=systems::g_view.project_full(data.head+Vec3{0,0,value.hatStyle==0?16.f:8.f});
            for(int i=0;i<12;++i)
            {
                const float angle=i*std::numbers::pi_v<float>/6;
                const auto edge=systems::g_view.project_full(data.head+Vec3{std::cos(angle)*18,std::sin(angle)*18,0});
                if(tip.w>.01f&&edge.w>.01f)draw->AddLine({tip.screen.x,tip.screen.y},{edge.screen.x,edge.screen.y},U32(value.accent,.6f));
            }
        }
        if(value.velocityChart)
        {
            speeds.push_back(data.speed);if(speeds.size()>180)speeds.pop_front();
            const float left=center.x-160,top=frame.screen_height-145;
            draw->AddRectFilled({left-8,top-8},{left+328,top+80},IM_COL32(18,22,30,170),6);
            for(size_t i=1;i<speeds.size();++i)
                draw->AddLine({left+(i-1)*320.f/179,top+64-std::clamp(speeds[i-1],0.f,400.f)*.16f},
                              {left+i*320.f/179,top+64-std::clamp(speeds[i],0.f,400.f)*.16f},U32(value.accent),1.5f);
        }
        else speeds.clear();
        if(value.watermark)
        {
            char line[80]{};std::snprintf(line,sizeof(line),"%.0f FPS",io.Framerate);
            const auto size=ImGui::CalcTextSize(line);
            draw->AddRectFilled({frame.screen_width-size.x-32,16},{frame.screen_width-16,46},IM_COL32(18,22,30,190),5);
            draw->AddText({frame.screen_width-size.x-24,23},U32(value.accent),line);
        }
        if(value.spectators)
        {
            ImGui::SetNextWindowPos({frame.screen_width-220,70},ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowBgAlpha(.85f);
            const auto flags=ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoSavedSettings|
                (io.MouseDrawCursor?0:ImGuiWindowFlags_NoInputs);
            if(ImGui::Begin("Spectators",nullptr,flags))
            {
                if(data.spectators.empty())ImGui::TextDisabled("None");
                for(const auto &name:data.spectators)ImGui::TextUnformatted(name.c_str());
            }
            ImGui::End();
        }
    }
    std::optional<Vec3> LightColor(std::uintptr_t object){std::lock_guard lock(lightMutex);return object&&object==lightScene.load()?std::optional<Vec3>{linearLight}:std::nullopt;}
    void Reset()
    {
        RetireLight();
        for(auto &patch:patches)Restore(patch);
        patches.clear();for(auto &[name,cvar]:cvars)RestoreCvar(cvar);cvars.clear();
        rings.clear();grounded.clear();std::lock_guard lock(mutex);snapshot={};
    }
    void RequestStop(){stopping=true;}
    bool Stopped(){return stopped.load();}
    void Menu(Page page)
    {
        auto value=Get();bool change=false;
        if(page==Page::Hud)
        {
            if(ImGui::CollapsingHeader("Crosshair",ImGuiTreeNodeFlags_DefaultOpen))
            {
                change|=ImGui::Checkbox("Custom crosshair",&value.crosshair);
                if(value.crosshair){change|=ImGui::Combo("Shape",&value.crosshairStyle,"Dot\0Cross\0Ring\0");change|=ImGui::SliderFloat("Size",&value.crosshairSize,1,20,"%.0f px");}
                change|=ImGui::Checkbox("Weapon spread circle",&value.spreadCircle);
                if(ImGui::IsItemHovered())ImGui::SetTooltip("Uses the equipped weapon's current spread and inaccuracy.");
                if(value.spreadCircle){change|=ImGui::Checkbox("Fill spread circle",&value.spreadFill);change|=ImGui::SliderFloat("Circle scale",&value.spreadScale,.5f,2,"%.2fx");}
                change|=Color("HUD accent",value.accent);
            }
            if(ImGui::CollapsingHeader("Scope",ImGuiTreeNodeFlags_DefaultOpen))
            {
                change|=ImGui::Checkbox("Custom scope lines",&value.scope);
                if(value.scope){change|=ImGui::Combo("Scope style",&value.scopeStyle,"Short lines\0Full screen\0");change|=ImGui::SliderFloat("Line length",&value.scopeLength,10,500,"%.0f px");change|=ImGui::SliderFloat("Center gap",&value.scopeGap,0,50,"%.0f px");change|=ImGui::SliderFloat("Line thickness",&value.scopeThickness,.5f,5,"%.1f px");change|=ImGui::Checkbox("Scope glow",&value.scopeGlow);}
            }

        }
        if(page==Page::Widgets){change|=ImGui::Checkbox("Spectator list",&value.spectators);change|=ImGui::Checkbox("Speed history chart",&value.velocityChart);change|=ImGui::Checkbox("FPS counter",&value.watermark);}
        if(page==Page::Movement)
        {
            change|=ImGui::Checkbox("Jump rings",&value.jumpRings);
            if(value.jumpRings){change|=ImGui::Checkbox("Enemies only",&value.enemyJumpsOnly);change|=ImGui::SliderFloat("Ring radius",&value.jumpRadius,10,200,"%.0f units");change|=ImGui::SliderFloat("Ring lifetime",&value.jumpDuration,.2f,5,"%.1f s");change|=Color("Ring color",value.jumpColor);}
            ImGui::Separator();change|=ImGui::Checkbox("Ragdoll gravity",&value.ragdollGravity);
            if(value.ragdollGravity){change|=ImGui::SliderFloat("Gravity scale",&value.gravityScale,-2,3,"%.2fx");if(ImGui::IsItemHovered())ImGui::SetTooltip("0 floats, negative values rise, 1 uses normal gravity. Applies to new ragdolls.");}
        }
        if(page==Page::Models)
        {
            if(ImGui::CollapsingHeader("Outline glow",ImGuiTreeNodeFlags_DefaultOpen))
            {change|=ImGui::Checkbox("Enemy glow",&value.enemyGlow);change|=Color("Enemy glow color",value.enemyColor);change|=ImGui::Checkbox("Teammate glow",&value.teamGlow);change|=Color("Friendly glow color",value.teamColor);change|=ImGui::Checkbox("Ragdoll glow",&value.corpseGlow);change|=ImGui::Checkbox("Dropped item glow",&value.itemGlow);change|=Color("Item glow color",value.itemColor);}
            change|=ImGui::Checkbox("Reveal players on radar",&value.radar);
        }
        if(page==Page::Removals)
        {
            change|=ImGui::Checkbox("Limit flash brightness",&value.flashLimit);
            if(value.flashLimit)change|=ImGui::SliderFloat("Maximum flash opacity",&value.flashOpacity,0,100,"%.0f%%");
                        change|=ImGui::Checkbox("Hide default crosshair",&value.removeCrosshair);
            change|=ImGui::Checkbox("Hide default scope",&value.removeScope);
        }
        if(page==Page::Local)
        {
            if(ImGui::CollapsingHeader("Appearance",ImGuiTreeNodeFlags_DefaultOpen))
            {change|=ImGui::Checkbox("Third-person fade",&value.localOpacity);if(value.localOpacity){change|=ImGui::SliderFloat("Opacity",&value.opacity,5,100,"%.0f%%");change|=ImGui::Checkbox("Scoped only",&value.opacityScopedOnly);}}
            change|=ImGui::Checkbox("Glow",&value.localGlow);
            change|=ImGui::Checkbox("Hide legs",&value.removeLegs);
            if(ImGui::CollapsingHeader("Viewmodel",ImGuiTreeNodeFlags_DefaultOpen))
            {change|=ImGui::Checkbox("Position",&value.viewmodel);if(value.viewmodel){change|=ImGui::SliderFloat3("Offset",value.viewmodelOffset.data(),-10,10,"%.1f");change|=ImGui::SliderFloat("FOV",&value.viewmodelFov,40,100,"%.0f degrees");}}
            if(ImGui::CollapsingHeader("Effects",ImGuiTreeNodeFlags_DefaultOpen)){
                change|=ImGui::Checkbox("Hat",&value.hat);
                if(value.hat)change|=ImGui::Combo("Shape",&value.hatStyle,"Cone\0Low crown\0");
                change|=ImGui::Checkbox("Light",&value.dynamicLight);
                if(value.dynamicLight){change|=Color("Color",value.lightColor);change|=ImGui::SliderFloat("Radius",&value.lightRadius,20,1000,"%.0f units");change|=ImGui::SliderFloat("Height",&value.lightHeight,-50,200,"%.0f units");}
            }
        }
        if(page==Page::Grenades)
        {
            change|=ImGui::Checkbox("Predict your grenade throw",&value.grenadePrediction);
            const char* names[]{"Flashbang","HE Grenade","Smoke","Molotov","Decoy","Incendiary"};
            for(int i=0;i<6;++i){
                ImGui::PushID(i);
                if(ImGui::TreeNode(names[i])){
                    auto& throwable=value.throwables[i];
                    change|=ImGui::Checkbox("Trajectory",&throwable.trajectory);
                    change|=Color("Path color",throwable.pathColor);
                    change|=ImGui::SliderFloat("Line width",&throwable.width,.5f,5,"%.1f px");
                    change|=ImGui::SliderFloat("Landing marker",&throwable.markerRadius,0,40,"%.0f units");
                    if(i==2){
                        change|=ImGui::Checkbox("Radius rings",&value.smokeArea);
                        change|=ImGui::Checkbox("Recolor smoke",&value.smokeTint);
                        if(value.smokeTint)change|=Color("Smoke color",value.smokeColor);
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            change|=ImGui::Checkbox("Burning ground outlines",&value.fireArea);
            if(ImGui::IsItemHovered())ImGui::SetTooltip("Shared by Molotov and Incendiary ground fire.");
        }
        if(change)Set(value);
    }
}
