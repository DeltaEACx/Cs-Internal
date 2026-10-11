// Native scopes run against local scene buffers; no game or hooks are loaded.
#include "features/visuals/effects/world.cpp"
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
bool Near(float a,float b) {return std::abs(a-b)<0.00001f;}
std::unordered_map<std::uint32_t,std::array<float,4>> fog_vectors;
std::uintptr_t __fastcall CaptureFog(void*,std::uint32_t hash,const void* value) {
    fog_vectors[hash]=static_cast<const custom::world::shader_vector*>(value)->values;
    return 0;
}
custom::configuration Manual() {
    custom::configuration value{};
    value.enabled=true;value.skybox=true;value.skybox_index=17;
    value.lighting=false;value.lighting_color={.78f,.19f,.75f};value.lighting_intensity=3.88f;
    value.rainbow_lighting=true;value.rainbow_speed=1.13f;
    value.fog=true;value.fog_color={.1f,.065f,.18f};
    value.fog_density=.045f;value.fog_distance=6800;value.fog_anisotropy=.03f;
    value.bloom=true;value.bloom_strength=.2f;
    return value;
}
void EffectiveConfiguration() {
    using namespace custom::world;
    auto manual=Manual();
    Check(sky_lighting_active(manual),"Japanese sky matching is not active by default");
    const auto matched=effective_configuration(manual);
    Check(matched.lighting&&!matched.rainbow_lighting&&matched.lighting_color==crimson_dawn_lighting.light_color
        &&Near(matched.lighting_intensity,crimson_dawn_lighting.light_intensity),"Matched sky light has the wrong color/intensity");
    Check(matched.fog&&matched.fog_color==crimson_dawn_lighting.fog_color
        &&matched.fog_density==manual.fog_density&&matched.fog_distance==manual.fog_distance
        &&matched.fog_anisotropy==manual.fog_anisotropy,"Matching overwrote manual fog density or distance");
    Check(matched.bloom==manual.bloom&&matched.bloom_strength==manual.bloom_strength,"Matching changed manual bloom");
    Check(!manual.lighting&&manual.rainbow_lighting&&Near(manual.lighting_intensity,3.88f),"Matching mutated the source config");
    manual.fog=false;
    auto resolved=effective_configuration(manual);
    Check(!resolved.fog&&resolved.fog_color==manual.fog_color,"Matching forced disabled fog on");
    for(int scenario=0;scenario<7;++scenario) {
        auto disabled=Manual();
        switch(scenario) {
        case 0:disabled.match_sky_lighting=false;break;
        case 1:disabled.enabled=false;break;
        case 2:disabled.skybox=false;break;
        case 3:disabled.sky_texture[0]='m';break;
        case 4:disabled.skybox_index=16;break;
        case 5:disabled.skybox_index=-1;break;
        case 6:disabled.skybox_index=999;break;
        }
        resolved=effective_configuration(disabled);
        Check(!sky_lighting_active(disabled)&&resolved.lighting==disabled.lighting
            &&resolved.rainbow_lighting==disabled.rainbow_lighting&&resolved.lighting_color==disabled.lighting_color
            &&resolved.lighting_intensity==disabled.lighting_intensity&&resolved.fog_color==disabled.fog_color,
            "Unavailable/disabled matching failed to restore manual settings");
    }
}
void NativeLightScope() {
    using namespace custom::world;
    scene_supported=true;
    std::array<std::byte,0x100> object{};
    const auto address=reinterpret_cast<std::uintptr_t>(object.data());
    constexpr std::array original{.3f,.4f,.5f};
    Check(memory::safe_write(address+0xe4,original),"Light fixture write failed");
    const auto manual=Manual();
    {
        light_draw_scope matched(address,manual);
        const auto value=memory::read<std::array<float,3>>(address+0xe4);
        Check(Near(value[0],1.15f)&&Near(value[1],.9085f)&&Near(value[2],.713f),
            "Native light scope did not apply the linked warm light");
        auto nested=manual;nested.match_sky_lighting=false;nested.lighting=true;nested.rainbow_lighting=false;
        {
            light_draw_scope custom(address,nested);
            const auto inner=memory::read<std::array<float,3>>(address+0xe4);
            Check(Near(inner[0],.78f*3.88f),"Manual nested lighting did not apply");
        }
        Check(memory::read<std::array<float,3>>(address+0xe4)==value,"Nested light scope did not restore linked lighting");
    }
    Check(memory::read<std::array<float,3>>(address+0xe4)==original,"Light scope did not restore native map light");
    auto disabled=manual;disabled.match_sky_lighting=false;
    {light_draw_scope scope(address,disabled);Check(memory::read<std::array<float,3>>(address+0xe4)==original,
        "Disabled matching enabled saved-disabled manual lighting");}
    scene_supported=false;
    {light_draw_scope scope(address,manual);Check(memory::read<std::array<float,3>>(address+0xe4)==original,
        "Unsupported scene build still modified native lighting");}
}
void NativeFogSetter() {
    using namespace custom::world;
    shader_supported=true;
    std::array<std::byte,0x400> object{};
    const auto address=reinterpret_cast<std::uintptr_t>(object.data());
    memory::safe_write(address+17*16,enable_gradient_fog);
    memory::safe_write(address+17*16+48,std::uint32_t{1});
    memory::safe_write(address+0x388,std::uint64_t{1234});
    int mode{};auto manual=Manual();
    Check(apply_fog(object.data(),&mode,CaptureFog,manual),"Native fog setter rejected fixture");
    const auto color=fog_vectors.at(gradient_fog_3);
    Check(Near(color[0],.63f)&&Near(color[1],.60f)&&Near(color[2],.57f),"Native fog setter kept purple fog");
    Check(Near(fog_vectors.at(gradient_fog)[1],manual.fog_distance)
        &&Near(fog_vectors.at(gradient_fog_2)[0],manual.fog_density),"Native fog setter lost user density/distance");
    manual.match_sky_lighting=false;
    Check(apply_fog(object.data(),&mode,CaptureFog,manual),"Native manual fog setter failed");
    Check(Near(fog_vectors.at(gradient_fog_3)[0],manual.fog_color[0])
        &&Near(fog_vectors.at(gradient_fog_3)[2],manual.fog_color[2]),"Native fog setter did not restore manual color");
    manual.match_sky_lighting=true;manual.fog=false;fog_vectors.clear();
    Check(!apply_fog(object.data(),&mode,CaptureFog,manual)&&fog_vectors.empty(),"Matching enabled disabled native fog");
    shader_supported=false;
}
}
int main() {
    try {
        EffectiveConfiguration();NativeLightScope();NativeFogSetter();
        std::cout<<"PASS: matched sky light/fog, manual restoration, custom-path/disabled gating, native light apply/restore and fog shader parameters\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
