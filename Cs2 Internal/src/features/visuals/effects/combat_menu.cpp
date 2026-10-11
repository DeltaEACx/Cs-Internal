#include "features/visuals/engine.hpp"
#include "config.hpp"
#include "particles.hpp"
#include "ui/option_row.h"

namespace custom {
namespace {
void all_effects(configuration& value,bool enabled) {
    value.kill_particles=value.impact_particles=value.hit_particles=value.headshot_accent=enabled;
    value.bullet_tracers=enabled;
    value.spawn_particles=value.jump_particles=value.landing_particles=value.speed_trail=enabled;
    value.reload_particles=value.victory_particles=value.multikill_crescendo=enabled;
}
}
void draw_effects_menu() {
    auto value=get_configuration();bool changed=false;
    if(ImGui::Button("Enable all")) {all_effects(value,true);changed=true;}
    ImGui::SameLine();
    if(ImGui::Button("Disable all")) {all_effects(value,false);changed=true;}
    ImGui::Separator();
    if(ImGui::CollapsingHeader("Eliminations",ImGuiTreeNodeFlags_DefaultOpen)) {
        changed|=ImGui::Checkbox("Kill burst",&value.kill_particles);
        if(value.kill_particles) {
            changed|=ImGui::Checkbox("Hide body after burst",&value.vanish_victim);
            const auto recipes=particles::kill_recipes();
            if(!recipes.empty()) {
                value.kill_recipe=std::clamp(value.kill_recipe,0,static_cast<int>(recipes.size())-1);
                if(ImGui::BeginCombo("Kill particle style",recipes[value.kill_recipe].label)) {
                    for(std::size_t i=0;i<recipes.size();++i)
                        if(ImGui::Selectable(recipes[i].label,value.kill_recipe==static_cast<int>(i))) {
                            value.kill_recipe=static_cast<int>(i);changed=true;
                        }
                    ImGui::EndCombo();
                }
            }
            changed|=ImGui::SliderFloat("Duration",&value.kill_lifetime,.4f,8,"%.1f s");
        }
        changed|=ImGui::Checkbox("Multikill bursts",&value.multikill_crescendo);
        if(value.multikill_crescendo)changed|=ImGui::SliderFloat("Multikill window",&value.multikill_window,1,10,"%.1f s");
    }
    if(ImGui::CollapsingHeader("Hits and impacts",ImGuiTreeNodeFlags_DefaultOpen)) {
        const bool tracers=value.bullet_tracers;
        option_row::Draw("Bullet Tracers",value.bullet_tracers,[&]{
            changed|=ImGui::ColorEdit4("Color",value.tracer_color.data(),ImGuiColorEditFlags_AlphaBar);
            const char* styles[]{"Line","Glow","Dashed"};
            changed|=ImGui::Combo("Style",&value.tracer_style,styles,3);
            changed|=ImGui::SliderFloat("Width",&value.tracer_width,.5f,6,"%.1f px");
            changed|=ImGui::SliderFloat("Duration",&value.tracer_lifetime,.1f,5,"%.1f s");
            changed|=ImGui::Checkbox("Fade",&value.tracer_fade);
        });
        changed|=value.bullet_tracers!=tracers;
        changed|=ImGui::Checkbox("Impact particles",&value.impact_particles);
        const char* impacts[]={"Sparks","Shockwave rings","Smoke"};
        if(value.impact_particles)changed|=ImGui::Combo("Bullet impact style",&value.impact_style,impacts,3);
        changed|=ImGui::Checkbox("Hit burst",&value.hit_particles);
        changed|=ImGui::Checkbox("Headshot flare",&value.headshot_accent);
        changed|=ImGui::Checkbox("Reload pulse",&value.reload_particles);
        if(ImGui::IsItemHovered())ImGui::SetTooltip("An effect at your player. The weapon reload effect is on the Weapon particles page.");
    }
    if(ImGui::CollapsingHeader("Movement and rounds")) {
        changed|=ImGui::Checkbox("Spawn burst",&value.spawn_particles);
        changed|=ImGui::Checkbox("Jump dust",&value.jump_particles);
        changed|=ImGui::Checkbox("Landing wave",&value.landing_particles);
        changed|=ImGui::Checkbox("Movement trail",&value.speed_trail);
        if(value.speed_trail)changed|=ImGui::SliderFloat("Minimum trail speed",&value.trail_min_speed,50,350,"%.0f units/s");
        changed|=ImGui::Checkbox("Victory fireworks",&value.victory_particles);
    }
    if(changed) {value.enabled=true;set_configuration(value);}
}
}
