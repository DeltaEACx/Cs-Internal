#include "features/visuals/engine.hpp"
#include "config.hpp"
#include "world.hpp"
#include "particles.hpp"
#include "imgui/imgui_internal.h"

namespace custom {
namespace {
void resource_help(const char* message) {
    if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",message);
}
void begin_disabled(bool disabled) {
    ImGui::PushItemFlag(ImGuiItemFlags_Disabled,disabled);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,ImGui::GetStyle().Alpha*(disabled?0.45f:1.0f));
}
void end_disabled() {ImGui::PopStyleVar();ImGui::PopItemFlag();}
bool sky_lighting_toggle(configuration& value) {
    begin_disabled(!world::sky_lighting_available(value));
    const bool changed=ImGui::Checkbox("Match sky lighting",&value.match_sky_lighting);
    resource_help("Match map light and fog colors to this sky.");
    end_disabled();
    return changed;
}
}
void draw_menu(world_page page) {
    auto value=get_configuration();bool changed=false;
    if(page==world_page::skybox) {
        changed|=ImGui::Checkbox("Custom sky",&value.skybox);
        const auto entries=world::skyboxes();
        value.skybox_index=std::clamp(value.skybox_index,0,static_cast<int>(entries.size())-1);
        if(ImGui::BeginCombo("Sky image",entries[value.skybox_index].label.data())) {
            for(std::size_t i=0;i<entries.size();++i)
                if(ImGui::Selectable(entries[i].label.data(),value.skybox_index==static_cast<int>(i))) {
                    value.skybox_index=static_cast<int>(i);value.sky_texture.fill(0);changed=true;
                    if(entries[i].reset_tint_on_select)value.tint_sky=false;
                }
            ImGui::EndCombo();
        }
        changed|=sky_lighting_toggle(value);
        ImGui::Separator();
        changed|=ImGui::Checkbox("Sky tint",&value.tint_sky);
        if(value.tint_sky) {
            changed|=ImGui::ColorEdit3("Sky color",value.sky_color.data());
            changed|=ImGui::SliderFloat("Sky intensity",&value.sky_intensity,0,5,"%.2f");
        }
        if(ImGui::TreeNode("Custom texture")) {
            changed|=ImGui::InputText("Texture resource",value.sky_texture.data(),value.sky_texture.size());
            resource_help("Game resource path: materials/.../*.vtex. Leave empty to use the chosen sky image.");
            ImGui::TreePop();
        }
    }
    else if(page==world_page::lighting) {
        changed|=sky_lighting_toggle(value);
        auto lighting=world::effective_configuration(value);
        bool lighting_changed=false;
        begin_disabled(world::sky_lighting_active(value));
        lighting_changed|=ImGui::Checkbox("Map lighting",&lighting.lighting);
        if(lighting.lighting) {
            lighting_changed|=ImGui::Checkbox("Color cycle",&lighting.rainbow_lighting);
            if(lighting.rainbow_lighting)lighting_changed|=ImGui::SliderFloat("Color cycle speed",&lighting.rainbow_speed,.01f,3,"%.2f");
            else lighting_changed|=ImGui::ColorEdit3("Map light color",lighting.lighting_color.data());
            lighting_changed|=ImGui::SliderFloat("Light intensity",&lighting.lighting_intensity,0,8,"%.2f");
        }
        end_disabled();
        if(lighting_changed) {
            value.lighting=lighting.lighting;value.rainbow_lighting=lighting.rainbow_lighting;
            value.rainbow_speed=lighting.rainbow_speed;value.lighting_color=lighting.lighting_color;
            value.lighting_intensity=lighting.lighting_intensity;changed=true;
        }
        ImGui::Separator();
        changed|=ImGui::Checkbox("Bloom",&value.bloom);
        resource_help("Controls the glow around bright areas of the image.");
        if(value.bloom)changed|=ImGui::SliderFloat("Bloom amount",&value.bloom_strength,0,3,"%.2f");
        changed|=ImGui::Checkbox("Depth of field",&value.depthOfField);
        if(value.depthOfField) {
            changed|=ImGui::SliderFloat("Near blur distance",&value.focusDistances[0],0,value.focusDistances[1],"%.0f units");
            changed|=ImGui::SliderFloat("Near focus distance",&value.focusDistances[1],value.focusDistances[0],value.focusDistances[2],"%.0f units");
            changed|=ImGui::SliderFloat("Far focus distance",&value.focusDistances[2],value.focusDistances[1],value.focusDistances[3],"%.0f units");
            changed|=ImGui::SliderFloat("Far blur distance",&value.focusDistances[3],value.focusDistances[2],10000,"%.0f units");
        }
        changed|=ImGui::Checkbox("Custom brightness",&value.gamma);
        if(value.gamma)changed|=ImGui::SliderFloat("Brightness (gamma)",&value.gamma_value,.2f,3,"%.2f");
    }
    else if(page==world_page::weather) {
        if(ImGui::CollapsingHeader("Fog",ImGuiTreeNodeFlags_DefaultOpen)) {
            changed|=ImGui::Checkbox("Enable fog",&value.fog);
            if(value.fog) {
                auto fog=world::effective_configuration(value);
                begin_disabled(world::sky_lighting_active(value));
                if(ImGui::ColorEdit3("Fog color",fog.fog_color.data())) {value.fog_color=fog.fog_color;changed=true;}
                end_disabled();
                changed|=ImGui::SliderFloat("Fog density",&value.fog_density,0,1,"%.2f");
                changed|=ImGui::SliderFloat("Fog distance",&value.fog_distance,100,10000,"%.0f units");
                changed|=ImGui::SliderFloat("Light scattering",&value.fog_anisotropy,0,1,"%.2f");
            }
        }
        if(ImGui::CollapsingHeader("Wind",ImGuiTreeNodeFlags_DefaultOpen)) {
            changed|=ImGui::Checkbox("Enable wind",&value.wind);
            if(value.wind) {
                changed|=ImGui::SliderFloat("Wind direction",&value.wind_direction,0,360,"%.0f degrees");
                changed|=ImGui::SliderFloat("Wind strength",&value.wind_strength,0,5,"%.2f");
                changed|=ImGui::SliderFloat("Gust frequency",&value.wind_frequency,.1f,5,"%.2f");
            }
        }
        if(ImGui::CollapsingHeader("Wet surfaces",ImGuiTreeNodeFlags_DefaultOpen)) {
            changed|=ImGui::Checkbox("Enable wetness",&value.wetness);
            if(value.wetness) {
                changed|=ImGui::SliderFloat("Wetness amount",&value.wetness_amount,0,2,"%.2f");
                changed|=ImGui::SliderFloat("Wetness speed",&value.wetness_speed,0,5,"%.2f");
            }
        }
    }
    else {
        changed|=ImGui::Checkbox("World particles",&value.particles);
        const auto entries=particles::presets();
        value.particle_preset=std::clamp(value.particle_preset,0,static_cast<int>(entries.size())-1);
        if(ImGui::BeginCombo("World particle style",entries[value.particle_preset].label)) {
            for(std::size_t i=0;i<entries.size();++i)
                if(ImGui::Selectable(entries[i].label,value.particle_preset==static_cast<int>(i))) {
                    value.particle_preset=static_cast<int>(i);changed=true;
                }
            ImGui::EndCombo();
        }
        const auto& selected=entries[value.particle_preset];
        if(!selected.resource) {
            changed|=ImGui::InputText("Custom particle resource",value.particle_resource.data(),value.particle_resource.size());
            resource_help("Game resource path: particles/.../*.vpcf.");
        }
        changed|=ImGui::SliderInt("Emitter count",&value.particle_count,1,8);
        changed|=ImGui::SliderFloat("Radius",&value.particle_radius,50,2000,"%.0f units");
        changed|=ImGui::SliderFloat("Height",&value.particle_height,0,1200,"%.0f units");
        changed|=ImGui::SliderFloat("Emitter spread",&value.particle_scale,.1f,5,"%.2f");
        if(selected.bursts)changed|=ImGui::SliderFloat("Burst rate",&value.particle_intensity,.1f,5,"%.2f");
        if(selected.color_control)changed|=ImGui::ColorEdit3("World particle color",value.particle_color.data());
    }
    if(changed) {value.enabled=true;set_configuration(value);}
}
}
