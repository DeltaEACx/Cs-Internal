// Exercise production ESP drawing without a game, hooks, or native entities.
#include "support/render_capture.h"
#ifdef small
#undef small
#endif
#include "features/visuals/detail/draw.hpp"
#include "imgui/imgui_impl_dx11.h"
#include <array>
#include <bit>
#include <filesystem>
#include <functional>
#include <limits>
#include <set>

namespace {
using namespace visuals;
using Draw = void (*)(ImDrawList*, const frame_snapshot&);
constexpr float width = 1280, height = 800;

settings EmptySettings() {
    settings value{};
    value.player_box=value.box_fill=value.player_name=value.health_bar=value.armor_bar=value.ammo_bar=false;
    value.weapon_name=value.distance=value.skeleton=value.head_marker=value.player_flags=value.snapline=false;
    value.item_names=value.item_distance=value.objective_timer=false;
    value.projectile_names=value.projectile_timer=value.projectile_path=false;
    value.fov_guide=value.speed_widget=value.bomb_widget=value.hit_marker=false;
    value.show_offscreen=false;
    return value;
}

screen_point Point(float x, float y) { return {x,y,true,true}; }
frame_snapshot PlayerFrame(float center=640, float top=180) {
    frame_snapshot frame;
    frame.screen_width=width; frame.screen_height=height; frame.local_team=2;
    player_snapshot player;
    player.name="NINJA"; player.weapon="AK-47"; player.flags={"HELMET","SCOPED"};
    player.bounds={center-52,top,center+52,top+240,true};
    player.head=Point(center,top+22); player.feet=Point(center,top+240);
    player.offscreen_direction=Point(width+80,height*.5f);
    player.skeleton={{Point(center,top+25),Point(center,top+115)},
        {Point(center,top+62),Point(center-30,top+105)},
        {Point(center,top+62),Point(center+30,top+105)},
        {Point(center,top+115),Point(center-25,top+228)},
        {Point(center,top+115),Point(center+25,top+228)}};
    player.health=67; player.armor=82; player.ammo=18; player.max_ammo=30;
    player.team=3; player.alive=true; player.enemy=true; player.visible=true; player.distance_meters=27;
    frame.players.push_back(player);
    return frame;
}
frame_snapshot WorldFrame() {
    frame_snapshot frame;
    frame.screen_width=width;frame.screen_height=height;frame.local_team=2;
    world_entity_snapshot item;
    item.label="AK-47";item.position=Point(640,300);item.kind=world_entity_kind::weapon;item.distance_meters=18;
    frame.entities.push_back(item);
    return frame;
}
frame_snapshot ProjectileFrame() {
    auto frame=WorldFrame();frame.entities.clear();
    projectile_snapshot projectile;
    projectile.label="SMOKE";projectile.position=Point(760,330);projectile.has_timer=true;projectile.time_remaining=3.2f;
    projectile.trajectory={Point(420,500),Point(510,420),Point(630,350),Point(760,330)};
    frame.projectiles.push_back(projectile);return frame;
}

struct Geometry {
    std::uint64_t fingerprint{1469598103934665603ull};
    int vertices{}, indices{};
    float minX=FLT_MAX,minY=FLT_MAX,maxX=-FLT_MAX,maxY=-FLT_MAX;
    float Width() const { return maxX-minX; }
    float CenterX() const { return (minX+maxX)*.5f; }
};
void Hash(std::uint64_t& hash,std::uint32_t value) { hash^=value;hash*=1099511628211ull; }
Geometry Inspect(const ImDrawList* draw) {
    Geometry result;result.vertices=draw->VtxBuffer.Size;result.indices=draw->IdxBuffer.Size;
    for(const auto& vertex:draw->VtxBuffer) {
        Check(std::isfinite(vertex.pos.x)&&std::isfinite(vertex.pos.y)&&std::isfinite(vertex.uv.x)&&std::isfinite(vertex.uv.y),
              "ESP produced non-finite vertices");
        Check(std::abs(vertex.pos.x)<1e6f&&std::abs(vertex.pos.y)<1e6f,"ESP geometry escaped reasonable drawing bounds");
        result.minX=std::min(result.minX,vertex.pos.x);result.minY=std::min(result.minY,vertex.pos.y);
        result.maxX=std::max(result.maxX,vertex.pos.x);result.maxY=std::max(result.maxY,vertex.pos.y);
        Hash(result.fingerprint,std::bit_cast<std::uint32_t>(vertex.pos.x));
        Hash(result.fingerprint,std::bit_cast<std::uint32_t>(vertex.pos.y));Hash(result.fingerprint,vertex.col);
    }
    for(auto index:draw->IdxBuffer){Check(index<draw->VtxBuffer.Size,"ESP index outside vertex buffer");Hash(result.fingerprint,index);}
    return result;
}

struct Renderer {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11ShaderResourceView> view;
    Renderer() {
        Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
                                        &device,nullptr,&context)),"WARP device");
        ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
        io.DisplaySize={width,height};io.DeltaTime=1.f/60.f;io.MousePos={-1000,-1000};
        Check(ImGui_ImplDX11_Init(device.Get(),context.Get()),"ImGui DX11 backend");
        D3D11_TEXTURE2D_DESC desc{};desc.Width=static_cast<UINT>(width);desc.Height=static_cast<UINT>(height);
        desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&texture)),"ESP capture texture");
        Check(SUCCEEDED(device->CreateRenderTargetView(texture.Get(),nullptr,&target)),"ESP capture target");
        Check(SUCCEEDED(device->CreateShaderResourceView(texture.Get(),nullptr,&view)),"ESP capture view");
    }
    ~Renderer(){ImGui_ImplDX11_Shutdown();ImGui::DestroyContext();}
    ImDrawList* Begin(){ImGui_ImplDX11_NewFrame();ImGui::NewFrame();return ImGui::GetBackgroundDrawList();}
    Geometry Render(Draw function,const frame_snapshot& frame,const settings& value) {
        g_settings=value;auto* draw=Begin();function(draw,frame);const auto result=Inspect(draw);ImGui::Render();return result;
    }
    void Save(const std::filesystem::path& output) {
        ImGui::Render();const float clear[]{.027f,.026f,.034f,1.f};context->ClearRenderTargetView(target.Get(),clear);
        auto* rtv=target.Get();context->OMSetRenderTargets(1,&rtv,nullptr);ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        SavePng(device.Get(),context.Get(),view.Get(),output.c_str());
    }
};

template<class Enum> void Styles(Renderer& renderer,const char* name,Draw draw,const frame_snapshot& frame,
                                 settings value,Enum settings::*member,int count) {
    std::set<std::uint64_t> distinct;
    for(int index=0;index<count;++index) {
        value.*member=static_cast<Enum>(index);const auto geometry=renderer.Render(draw,frame,value);
        if(!geometry.vertices||!geometry.indices||!distinct.insert(geometry.fingerprint).second)
            throw std::runtime_error(std::string(name)+": selection did not produce a distinct visible style");
    }
    std::cout<<"PASS: "<<name<<" ("<<count<<" styles)\n";
}
void StyleSelections(Renderer& renderer) {
    const auto player=PlayerFrame();auto value=EmptySettings();value.player_box=true;
    Styles(renderer,"Player boxes",detail::draw_players,player,value,&settings::boxes,3);
    value=EmptySettings();value.health_bar=true;
    Styles(renderer,"Health bars",detail::draw_players,player,value,&settings::health_style,3);
    value=EmptySettings();value.armor_bar=true;
    Styles(renderer,"Armor bars",detail::draw_players,player,value,&settings::armor_style,3);
    value=EmptySettings();value.ammo_bar=true;
    Styles(renderer,"Ammo bars",detail::draw_players,player,value,&settings::ammo_style,3);
    value=EmptySettings();value.player_name=true;
    Styles(renderer,"Player names",detail::draw_players,player,value,&settings::name_style,3);
    value=EmptySettings();value.weapon_name=true;
    Styles(renderer,"Player weapons",detail::draw_players,player,value,&settings::weapon_style,3);
    value=EmptySettings();value.distance=true;
    Styles(renderer,"Player distances",detail::draw_players,player,value,&settings::distance_style,3);
    value=EmptySettings();value.skeleton=true;
    Styles(renderer,"Skeleton",detail::draw_players,player,value,&settings::skeleton_render,2);
    value=EmptySettings();value.head_marker=true;
    Styles(renderer,"Head markers",detail::draw_players,player,value,&settings::head_style,5);
    value=EmptySettings();value.snapline=true;
    Styles(renderer,"Snaplines",detail::draw_players,player,value,&settings::snapline_style,2);
    value=EmptySettings();value.show_offscreen=true;auto offscreen=player;offscreen.players[0].bounds.valid=false;
    Styles(renderer,"Offscreen markers",detail::draw_players,offscreen,value,&settings::offscreen_style,3);
    value=EmptySettings();value.player_flags=true;
    Styles(renderer,"Player flags",detail::draw_players,player,value,&settings::flags_style,2);
    value=EmptySettings();value.item_names=true;
    Styles(renderer,"Dropped item labels",detail::draw_world,WorldFrame(),value,&settings::item_style,3);
    value=EmptySettings();value.projectile_names=true;
    Styles(renderer,"Projectile labels",detail::draw_world,ProjectileFrame(),value,&settings::projectile_text_style,3);
    value=EmptySettings();value.projectile_path=true;
    Styles(renderer,"Trajectories",detail::draw_world,ProjectileFrame(),value,&settings::trajectory_style,2);
    value=EmptySettings();
    Styles(renderer,"Projectile markers",detail::draw_world,ProjectileFrame(),value,&settings::projectile_marker,5);
    auto world=WorldFrame();world.entities.clear();world.show_fov_guide=true;world.fov_radius=110;
    value=EmptySettings();value.fov_guide=true;
    Styles(renderer,"FOV guides",detail::draw_world,world,value,&settings::fov_style,2);
    value=EmptySettings();value.speed_widget=true;world.local_speed=247;
    Styles(renderer,"Speed labels",detail::draw_world,world,value,&settings::speed_text_style,3);
    value=EmptySettings();value.hit_marker=true;world.show_hit_marker=true;world.hit_marker_time=.2f;
    Styles(renderer,"Hit markers",detail::draw_world,world,value,&settings::hit_marker_style,3);
}
void Filters(Renderer& renderer) {
    auto value=EmptySettings();value.player_box=true;auto frame=PlayerFrame();
    Check(renderer.Render(detail::draw_players,frame,value).vertices>0,"Fixture enemy did not draw");
    auto hidden=frame;hidden.players[0].alive=false;
    Check(!renderer.Render(detail::draw_players,hidden,value).vertices,"Dead enemy bypassed filter");
    hidden=frame;hidden.players[0].team=frame.local_team;hidden.players[0].enemy=false;
    Check(!renderer.Render(detail::draw_players,hidden,value).vertices,"Teammate bypassed filter");
    value.show_teammates=true;Check(renderer.Render(detail::draw_players,hidden,value).vertices>0,"Enabled teammate filter suppressed player");
    value.show_teammates=false;hidden=frame;hidden.players[0].distance_meters=value.max_distance_meters+1;
    Check(!renderer.Render(detail::draw_players,hidden,value).vertices,"Distant enemy bypassed filter");
    hidden=frame;hidden.players[0].dormant=true;value.show_dormant=false;
    Check(!renderer.Render(detail::draw_players,hidden,value).vertices,"Dormant enemy bypassed filter");
    value.show_dormant=true;Check(renderer.Render(detail::draw_players,hidden,value).vertices>0,"Enabled dormant filter suppressed player");
    value=EmptySettings();value.item_names=true;auto world=WorldFrame();
    for(auto kind:{world_entity_kind::weapon,world_entity_kind::grenade,world_entity_kind::objective,world_entity_kind::other}) {
        world.entities[0].kind=kind;value.dropped_weapons=value.dropped_grenades=value.dropped_objectives=value.other_entities=false;
        Check(!renderer.Render(detail::draw_world,world,value).vertices,"Disabled world item category bypassed filter");
        value.dropped_weapons=value.dropped_grenades=value.dropped_objectives=value.other_entities=true;
        Check(renderer.Render(detail::draw_world,world,value).vertices>0,"Enabled world item category did not draw");
    }
    world.entities[0].distance_meters=value.max_distance_meters+1;
    Check(!renderer.Render(detail::draw_world,world,value).vertices,"Distant world item bypassed filter");
    world=WorldFrame();world.entities[0].position.on_screen=false;
    Check(!renderer.Render(detail::draw_world,world,value).vertices,"Offscreen world item bypassed filter");
    std::cout<<"PASS: player and world visibility filters\n";
}
void TextSizing(Renderer& renderer) {
    auto value=EmptySettings();value.player_name=true;value.text_size=12;const auto frame=PlayerFrame();
    const auto first=renderer.Render(detail::draw_players,frame,value);value.text_size=24;
    const auto second=renderer.Render(detail::draw_players,frame,value);
    Check(second.Width()>first.Width()*1.8f,"Player label font size did not scale rendered glyphs");
    Check(std::abs(first.CenterX()-640)<3&&std::abs(second.CenterX()-640)<3,"Player label lost centering when font size changed");
    value=EmptySettings();value.item_names=true;value.text_size=12;
    const auto worldFirst=renderer.Render(detail::draw_world,WorldFrame(),value);value.text_size=24;
    const auto worldSecond=renderer.Render(detail::draw_world,WorldFrame(),value);
    Check(worldSecond.Width()>worldFirst.Width()*1.8f,"World label font size did not scale rendered glyphs");
    Check(std::abs(worldFirst.CenterX()-640)<3&&std::abs(worldSecond.CenterX()-640)<3,"World label lost centering when font size changed");
    std::cout<<"PASS: rendered text scales and remains centered\n";
}
void FiniteGeometry(Renderer& renderer) {
    auto value=settings{};value.ammo_bar=value.skeleton=value.snapline=value.box_fill=true;
    value.projectile_path=value.fov_guide=value.speed_widget=true;
    value.boxes=box_style::rounded;value.show_offscreen=true;
    auto player=PlayerFrame();auto world=ProjectileFrame();world.entities=WorldFrame().entities;
    world.show_fov_guide=world.show_hit_marker=world.bomb_planted=true;
    world.fov_radius=120;world.hit_marker_time=.2f;world.local_speed=250;world.bomb_time_remaining=30;
    constexpr std::array<float settings::*,21> dimensions{{&settings::box_thickness,&settings::box_rounding,
        &settings::health_width,&settings::armor_width,&settings::ammo_width,
        &settings::skeleton_thickness,&settings::head_size,&settings::head_thickness,&settings::snapline_thickness,
        &settings::offscreen_size,&settings::offscreen_radius,&settings::trajectory_thickness,&settings::projectile_size,
        &settings::fov_thickness,&settings::hit_marker_size,&settings::hit_marker_thickness,&settings::text_size,
        &settings::corner_fraction,&settings::max_distance_meters,&settings::snapline_origin,&settings::guide_degrees}};
    for(float extreme:{-1000.f,1e20f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        for(auto member:dimensions) {
            auto changed=value;changed.*member=extreme;
            renderer.Render(detail::draw_players,player,changed);renderer.Render(detail::draw_world,world,changed);
            if(member==&settings::offscreen_size||member==&settings::offscreen_radius) {
                auto offscreen=player;offscreen.players[0].bounds.valid=false;
                renderer.Render(detail::draw_players,offscreen,changed);
            }
        }
    }
    auto malformed=player;malformed.players[0].bounds.left=std::numeric_limits<float>::quiet_NaN();
    renderer.Render(detail::draw_players,malformed,value);
    malformed=player;malformed.players[0].head.x=std::numeric_limits<float>::infinity();
    malformed.players[0].skeleton[0].from.y=std::numeric_limits<float>::quiet_NaN();
    malformed.players[0].feet.x=std::numeric_limits<float>::quiet_NaN();renderer.Render(detail::draw_players,malformed,value);
    world.projectiles[0].trajectory[0].x=std::numeric_limits<float>::quiet_NaN();
    world.projectiles[0].position.y=std::numeric_limits<float>::infinity();
    world.entities[0].position.x=std::numeric_limits<float>::quiet_NaN();
    world.local_speed=world.bomb_time_remaining=world.hit_marker_time=world.fov_radius=std::numeric_limits<float>::quiet_NaN();
    renderer.Render(detail::draw_world,world,value);
    for(int extreme:{std::numeric_limits<int>::min(),std::numeric_limits<int>::max()}) {
        malformed=player;auto& target=malformed.players[0];target.health=target.armor=target.ammo=target.max_ammo=extreme;
        renderer.Render(detail::draw_players,malformed,value);
    }
    std::cout<<"PASS: malformed snapshots and extreme style settings stay finite\n";
}
void Capture(Renderer& renderer,const std::filesystem::path& path) {
    auto* draw=renderer.Begin();
    draw->AddText(ImGui::GetFont(),24,{42,26},IM_COL32(235,225,230,255),"DeltaEAC  /  ESP STYLES");
    const char* labels[]{"FULL + SOLID","CORNERED + GRADIENT","ROUNDED + SEGMENTED"};
    for(int index=0;index<3;++index) {
        const float center=220+index*420.f;
        draw->AddRectFilled({center-190,78},{center+190,520},IM_COL32(23,22,29,255),12);
        draw->AddText({center-164,101},IM_COL32(225,100,115,255),labels[index]);
        auto frame=PlayerFrame(center,168);g_settings=settings{};
        g_settings.boxes=static_cast<box_style>(index);g_settings.health_style=g_settings.armor_style=g_settings.ammo_style=static_cast<bar_style>(index);
        g_settings.ammo_bar=true;g_settings.skeleton=true;g_settings.skeleton_render=skeleton_style::joints;
        g_settings.name_style=g_settings.weapon_style=text_style::shadow;g_settings.text_size=16;
        g_settings.box_fill=true;g_settings.box_fill_color={66,26,36,65};
        g_settings.enemy_visible={224,72,95,255};g_settings.health_value=g_settings.armor_value=true;
        detail::draw_players(draw,frame);
    }
    draw->AddText({42,553},IM_COL32(230,225,232,255),"HEAD MARKERS");
    const char* markerNames[]{"Circle","Dot","Square","Diamond","Cross"};
    for(int index=0;index<5;++index) {
        const float center=105+index*120.f;auto frame=PlayerFrame(center,606);
        g_settings=EmptySettings();g_settings.head_marker=true;g_settings.head_size=9;g_settings.head_style=static_cast<marker_style>(index);
        detail::draw_players(draw,frame);draw->AddText({center-20,657},IM_COL32(170,168,181,255),markerNames[index]);
    }
    draw->AddText({734,553},IM_COL32(230,225,232,255),"PROJECTILE TRAIL");
    auto world=ProjectileFrame();world.projectiles[0].position=Point(1160,644);
    world.projectiles[0].trajectory={Point(766,727),Point(885,658),Point(1015,624),Point(1160,644)};
    g_settings=EmptySettings();g_settings.projectile_path=g_settings.projectile_names=g_settings.projectile_timer=true;
    g_settings.trajectory_style=line_style::dashed;g_settings.projectile_marker=marker_style::diamond;
    g_settings.projectile_size=7;g_settings.projectile_text_style=text_style::outline;g_settings.text_size=16;
    detail::draw_world(draw,world);Inspect(draw);renderer.Save(path);
}
}
int main(int argc,char** argv) {
    try {
        Check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)),"COM initialization");
        {Renderer renderer;StyleSelections(renderer);Filters(renderer);TextSizing(renderer);FiniteGeometry(renderer);
            const auto path=argc>1?std::filesystem::path(argv[1]):std::filesystem::path("tests/artifacts/esp_styles.png");
            std::filesystem::create_directories(path.parent_path());Capture(renderer,path);}
        CoUninitialize();std::cout<<"PASS: production ESP style regression\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
