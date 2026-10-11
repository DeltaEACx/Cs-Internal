#include "core/user_defaults.hpp"
#include "gun_images.h"
#include "chams_materials.h"
#include "effects/world.hpp"
#include "ui/agent_preview.h"
#include "game/sdk/PreviewBindings.h"
namespace visuals::gun_images {
namespace {
std::mutex mutex;Configuration config=user_defaults::GunImages(Configuration{});std::string status;
std::atomic<std::uintptr_t> active{};
preview_scene::ImageFinish activePreview;
ULONGLONG nextUpdate{};
__declspec(noinline) ID3D11ShaderResourceView* TextureView(std::uintptr_t binding,bool cube){
    ID3D11ShaderResourceView *result{};
    __try{
        const auto payload=memory::read<std::uintptr_t>(binding);
        if(!payload||memory::read<int>(binding+0x20)<=0)return nullptr;
        auto *view=reinterpret_cast<IUnknown*>(memory::read<std::uintptr_t>(payload+preview_bindings::kTextureSrgbView));
        if(!view)view=reinterpret_cast<IUnknown*>(memory::read<std::uintptr_t>(payload+preview_bindings::kTextureLinearView));
        if(!view||FAILED(view->QueryInterface(__uuidof(ID3D11ShaderResourceView),reinterpret_cast<void**>(&result))))return nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};result->GetDesc(&desc);
        if(desc.ViewDimension!=(cube?D3D11_SRV_DIMENSION_TEXTURECUBE:D3D11_SRV_DIMENSION_TEXTURE2D)){result->Release();result=nullptr;}
    }__except(EXCEPTION_EXECUTE_HANDLER){if(result)result->Release();return nullptr;}
    return result;
}
constexpr std::string_view portrait="materials/custom/skybox/portrait_gun.vtex";
bool Valid(std::string_view p){return p.starts_with("materials/")&&p.ends_with(".vtex")&&p.size()<200&&p.find("..") == std::string_view::npos&&std::ranges::none_of(p,[](unsigned char c){return c<32||c=='"'||c=='\\'||c==':'||c=='\'';});}
}
Configuration Get(){std::lock_guard lock(mutex);return config;}
void Set(Configuration value){value.texture.back()=0;std::lock_guard lock(mutex);config=value;}
bool SelectPreset(Configuration &value, std::size_t index){
 if(index>=image_finishes::kPresets.size())return false;
 value.enabled=true;value.followSkybox=false;
 strcpy_s(value.texture.data(),value.texture.size(),image_finishes::kPresets[index].texture);
 return true;
}
bool CanUpdate(const Configuration &value,bool skyboxEnabled){return value.enabled&&(!value.followSkybox||skyboxEnabled);}
std::string Source(std::string_view path,bool cube){
 if(!Valid(path))return {};
 std::string s="<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->\n{\n";
 if(cube){
  // This shader exposes a material cubemap. csgo_complex's EnvironmentMap is a global cube array.
  s+="shader = \"csgo_simple_3layer_parallax.vfx\"\nF_SECOND_LAYER_CUBEMAP = 1\n";
  s+="g_tLayer0Mask = resource:\"materials/effects/black_color_tga_cd7d39e9.vtex\"\n";
  s+="g_tLayer1Color = resource:\"materials/custom/skybox/clear_layer.vtex\"\n";
  s+="TextureCubeMap = resource:\""+std::string(path)+"\"\ng_tLayer2Cubemap = resource:\""+std::string(path)+"\"\n";
  s+="g_flLayer1EmissiveLevel = 0.0\ng_flLayer2EmissiveLevel = 1.0\ng_flLayer2RefractionAmount = 1.0\ng_flLayer2Offset = 0.0\ng_flLayer2RoughnessBlurMin = 0.0\ng_flLayer2RoughnessBlurMax = 0.0\ng_flMetalness = 0.9\ng_flReflectance = 0.5\n";
 }else{
  s+="shader = \"csgo_unlitgeneric.vfx\"\nF_PAINT_VERTEX_COLORS = 1\ng_tColor = resource:\""+std::string(path)+"\"\n";
 }
 s+="F_RENDER_BACKFACES = 0\nF_DISABLE_Z_BUFFERING = 0\ng_vColorTint = [1,1,1,1]\n}\n";return s;
}
void Update(bool inWorld){
 const auto value=Get();
 const auto world=custom::get_configuration();
 if(!CanUpdate(value,world.enabled&&world.skybox)){Reset();return;}
 const auto now=GetTickCount64();if(now<nextUpdate)return;nextUpdate=now+100;
 std::string skyPath=custom::world::active_sky_path();
 if(value.followSkybox&&!inWorld){
  const auto end=std::find(world.sky_texture.begin(),world.sky_texture.end(),'\0');
  if(end!=world.sky_texture.end()&&end!=world.sky_texture.begin())skyPath.assign(world.sky_texture.begin(),end);
  else if(end==world.sky_texture.begin()){
   const auto skies=custom::world::skyboxes();
   if(!skies.empty())skyPath=skies[std::clamp(world.skybox_index,0,static_cast<int>(skies.size())-1)].resource_path;
  }
 }
 const std::string path=value.followSkybox?skyPath:(value.texture[0]?std::string(value.texture.data()):std::string(portrait));
 std::string state;std::uintptr_t material{};preview_scene::ImageFinish preview;
 if(!Valid(path))state=value.followSkybox?"Choose a custom skybox first.":"Use a materials/*.vtex resource path.";
 else if(!custom::world::texture_binding(path)||(value.followSkybox&&!custom::world::texture_binding("materials/custom/skybox/clear_layer.vtex")))state="Image resource unavailable.";
 else {
  material=chams::materials::CreateCached((value.followSkybox?"cube:":"image:")+path,Source(path,value.followSkybox));
  if(!material)state="Image material unavailable for this game build.";
 }
 if(material&&game::module_matches("rendersystemdx11.dll",preview_bindings::kRenderSha256)){
    preview.texture.Attach(TextureView(custom::world::texture_binding(path),value.followSkybox));
    preview.cube=value.followSkybox;preview.tint={value.tint.r/255.f,value.tint.g/255.f,value.tint.b/255.f};
 }
 active=material;agent_preview::SetImageFinish(preview);
 std::lock_guard lock(mutex);status=state;activePreview=preview;
}
std::uintptr_t Material(){
 const auto value=Get();if(!value.enabled)return 0;
 if(value.followSkybox){const auto world=custom::get_configuration();if(!world.enabled||!world.skybox)return 0;}
 return active.load();
}
color Tint(){return Get().tint;}
void Reset(){active=0;nextUpdate=0;agent_preview::SetImageFinish({});std::lock_guard lock(mutex);activePreview={};status.clear();}
void Menu(MenuMode mode){
 auto value=Get();bool change=ImGui::Checkbox("Enable image finish",&value.enabled);
 if(ImGui::IsItemHovered())ImGui::SetTooltip("Covers the selected skin, including lobby previews. Disable to restore the skin.");
 if(value.enabled){
  if(mode==MenuMode::Images)change|=ImGui::Checkbox("Follow skybox",&value.followSkybox);
  if(ImGui::IsItemHovered())ImGui::SetTooltip("Follows the selected skybox. Disabling the skybox restores your usual gun finish.");
  if(!value.followSkybox||mode==MenuMode::Camo){
   const char *selected=value.texture[0]?"Custom resource":"Portrait";
   if(mode==MenuMode::Camo&&value.followSkybox)selected="Choose camo";
   for(const auto &preset:image_finishes::kPresets)if(std::string_view(value.texture.data())==preset.texture)selected=preset.name;
   if(ImGui::BeginCombo(mode==MenuMode::Camo?"Camo":"Image / camo",selected)){
    if(mode==MenuMode::Images&&ImGui::Selectable("Portrait",!value.texture[0])){value.texture.fill(0);change=true;}
    for(std::size_t i=0;i<image_finishes::kPresets.size();++i)
     if(ImGui::Selectable(image_finishes::kPresets[i].name,std::string_view(value.texture.data())==image_finishes::kPresets[i].texture))change|=SelectPreset(value,i);
    ImGui::EndCombo();
   }
   if(mode==MenuMode::Images)change|=ImGui::InputText("Custom image texture",value.texture.data(),value.texture.size());
   if(ImGui::IsItemHovered())ImGui::SetTooltip("Choose a camo above, or enter your compiled materials/*.vtex image.");
  }
  float rgba[]{value.tint.r/255.f,value.tint.g/255.f,value.tint.b/255.f,value.tint.a/255.f};
  if(ImGui::ColorEdit4("Finish tint",rgba,ImGuiColorEditFlags_NoInputs)){value.tint={std::uint8_t(rgba[0]*255),std::uint8_t(rgba[1]*255),std::uint8_t(rgba[2]*255),255};change=true;}
 }
 if(change)Set(value);
 std::lock_guard lock(mutex);
 if(value.enabled&&activePreview.texture&&!activePreview.cube)ImGui::Image(activePreview.texture.Get(),ImVec2(144,144));
}
}
