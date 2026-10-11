#include "features/visuals/engine.hpp"

#include "renderer.hpp"
#include "features/cosmetics/skin_changer.h"
#include "game/character_catalog.h"
#include "ui/catalog_ui.h"
#include "ui/option_row.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cctype>

namespace
{
	void color_picker(const char* label, visuals::color& value)
	{
		float rgba[4]{
			static_cast<float>(value.r) / 255.0f,
			static_cast<float>(value.g) / 255.0f,
			static_cast<float>(value.b) / 255.0f,
			static_cast<float>(value.a) / 255.0f
		};
		if (!ImGui::ColorEdit4(label, rgba))
			return;

		const auto to_byte = [](float channel) {
			return static_cast<std::uint8_t>(std::lround(std::clamp(channel, 0.0f, 1.0f) * 255.0f));
		};
		value = { to_byte(rgba[0]), to_byte(rgba[1]), to_byte(rgba[2]), to_byte(rgba[3]) };
	}
    class DisabledControls
    {
    public:
        explicit DisabledControls(bool disabled)
        {
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, disabled);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (disabled ? 0.5f : 1.0f));
        }
        ~DisabledControls()
        {
            ImGui::PopStyleVar();
            ImGui::PopItemFlag();
        }
    };
}

namespace visuals
{
    void draw_enemy_models_menu()
    {
        static std::vector<character_catalog::Agent> models;
        static bool scanned = false;
        static char search[128]{};
        const bool refresh = ImGui::Button("Refresh models");
        if (!scanned || refresh)
        {
            models = character_catalog::Discover();
            scanned = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu installed", models.size());

        auto choice = skin_changer::GetEnemyModel();
        {
            DisabledControls disabled(choice.model.empty());
            if (ImGui::Checkbox("Enable enemy models", &choice.enabled))
                skin_changer::SaveEnemyModel(choice);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Applies the selected model to living enemies. Turning this off restores their original models.");

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputTextWithHint("##enemy_model_search", "Search models", search, sizeof(search));
        std::string filter(search);
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool found = false;
        const float height = std::clamp(ImGui::GetContentRegionAvail().y - 130.0f, 80.0f, 260.0f);
        if (ImGui::ListBoxHeader("##enemy_models", {ImGui::GetContentRegionAvail().x, height}))
        {
            for (const auto &model : models)
            {
                std::string name = model.name;
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!filter.empty() && name.find(filter) == std::string::npos)
                    continue;
                found = true;
                ImGui::PushID(model.model.c_str());
                const bool selected = game_access::SameModelPath(choice.model, model.model);
                if (ImGui::Selectable(model.name.c_str(), selected))
                {
                    choice.model = model.model;
                    choice.name = model.name;
                    skin_changer::SaveEnemyModel(choice);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", model.model.c_str());
                ImGui::PopID();
            }
            if (!found)
                ImGui::TextDisabled(models.empty() ? "No installed custom models found." : "No matching models.");
            ImGui::ListBoxFooter();
        }
        if (models.empty())
            ImGui::TextWrapped("Run Setup.cmd to install the bundled models.");
        {
            DisabledControls disabled(choice.model.empty());
            if (ImGui::Button("Preview model"))
                catalog_ui::PreviewAgent(choice.model);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Shows this model in the Agent preview window.");
        ImGui::SameLine();
        if (ImGui::Button("Restore defaults"))
        {
            skin_changer::SaveEnemyModel({});
            catalog_ui::PreviewEquippedAgent();
        }
    }

    void draw_menu(menu_section section)
    {
        using option_row::Draw;
        constexpr const char* boxes[]{"Full","Cornered","Rounded"};
        constexpr const char* bars[]{"Solid","Gradient","Segmented"};
        constexpr const char* textStyles[]{"Plain","Shadow","Outline"};
        constexpr const char* lines[]{"Solid","Dashed"};
        constexpr const char* markers[]{"Circle","Dot","Square","Diamond","Cross"};
        constexpr const char* arrows[]{"Triangle","Chevron","Dot"};
        constexpr const char* skeletons[]{"Lines","Joints"};
        constexpr const char* flags[]{"Plain","Badges"};
        constexpr const char* hits[]{"Cross","Plus","Dot"};
        const auto textOptions=[&](text_style& style,color& tint){
            option_row::Style(style,textStyles);color_picker("Color",tint);
        };
        const auto barOptions=[&](bar_style& style,float& width,bool& number){
            option_row::Style(style,bars);ImGui::SliderFloat("Width",&width,2,8,"%.0f px");
            ImGui::Checkbox("Show value",&number);
        };
        // Overlays are always on; the master toggle is intentionally not shown.
        g_settings.enabled=true;
        if(ImGui::CollapsingHeader("Overlay",ImGuiTreeNodeFlags_DefaultOpen)){
            ImGui::SliderFloat("Range",&g_settings.max_distance_meters,25,500,"%.0f m");
            ImGui::SliderFloat("Text size",&g_settings.text_size,10,20,"%.0f px");
            if(section==menu_section::players){
                ImGui::Checkbox("Teammates",&g_settings.show_teammates);
                ImGui::Checkbox("Dormant players",&g_settings.show_dormant);
                ImGui::Separator();
                color_picker("Visible",g_settings.enemy_visible);color_picker("Behind cover",g_settings.enemy_hidden);
                color_picker("Team color",g_settings.teammate);color_picker("Dormant",g_settings.dormant);
            }
        }
        ImGui::Separator();
        if(section==menu_section::players)
        {
            if(ImGui::CollapsingHeader("Shapes",ImGuiTreeNodeFlags_DefaultOpen)){
                Draw("Box",g_settings.player_box,[&]{
                    option_row::Style(g_settings.boxes,boxes);
                    ImGui::SliderFloat("Line width",&g_settings.box_thickness,.75f,3,"%.2f px");
                    if(g_settings.boxes==box_style::corners)ImGui::SliderFloat("Corner length",&g_settings.corner_fraction,.08f,.5f,"%.2f");
                    if(g_settings.boxes==box_style::rounded)ImGui::SliderFloat("Rounding",&g_settings.box_rounding,1,18,"%.0f px");
                    ImGui::Checkbox("Outline",&g_settings.box_outline);
                    ImGui::Checkbox("Fill",&g_settings.box_fill);
                    if(g_settings.box_fill)color_picker("Fill color",g_settings.box_fill_color);
                    color_picker("Visible",g_settings.enemy_visible);color_picker("Behind cover",g_settings.enemy_hidden);
                });
                Draw("Skeleton",g_settings.skeleton,[&]{
                    option_row::Style(g_settings.skeleton_render,skeletons);
                    ImGui::SliderFloat("Line width",&g_settings.skeleton_thickness,.5f,4,"%.1f px");
                    color_picker("Color",g_settings.skeleton_color);
                });
                Draw("Head marker",g_settings.head_marker,[&]{
                    option_row::Style(g_settings.head_style,markers);
                    ImGui::SliderFloat("Size",&g_settings.head_size,2,16,"%.0f px");
                    ImGui::SliderFloat("Line width",&g_settings.head_thickness,.5f,4,"%.1f px");
                });
                Draw("Snaplines",g_settings.snapline,[&]{
                    option_row::Style(g_settings.snapline_style,lines);
                    ImGui::SliderFloat("Line width",&g_settings.snapline_thickness,.5f,4,"%.1f px");
                    constexpr const char* origins[]{"Bottom","Center","Top"};
                    int origin=std::clamp(static_cast<int>(std::lround(g_settings.snapline_origin*2)),0,2);
                    if(ImGui::Combo("Origin",&origin,origins,3))g_settings.snapline_origin=origin*.5f;
                });
                Draw("Offscreen markers",g_settings.show_offscreen,[&]{
                    option_row::Style(g_settings.offscreen_style,arrows);
                    ImGui::SliderFloat("Size",&g_settings.offscreen_size,6,28,"%.0f px");
                    ImGui::SliderFloat("Radius",&g_settings.offscreen_radius,.15f,.47f,"%.2f");
                });
            }
            if(ImGui::CollapsingHeader("Information",ImGuiTreeNodeFlags_DefaultOpen)){
                Draw("Name",g_settings.player_name,[&]{textOptions(g_settings.name_style,g_settings.name_color);});
                Draw("Health",g_settings.health_bar,[&]{barOptions(g_settings.health_style,g_settings.health_width,g_settings.health_value);});
                Draw("Armor",g_settings.armor_bar,[&]{
                    barOptions(g_settings.armor_style,g_settings.armor_width,g_settings.armor_value);color_picker("Color",g_settings.armor_color);
                });
                Draw("Ammo",g_settings.ammo_bar,[&]{
                    barOptions(g_settings.ammo_style,g_settings.ammo_width,g_settings.ammo_value);color_picker("Color",g_settings.ammo_color);
                });
                Draw("Weapon",g_settings.weapon_name,[&]{textOptions(g_settings.weapon_style,g_settings.weapon_color);});
                Draw("Distance",g_settings.distance,[&]{textOptions(g_settings.distance_style,g_settings.distance_color);});
                Draw("Status labels",g_settings.player_flags,[&]{
                    option_row::Style(g_settings.flags_style,flags);color_picker("Color",g_settings.flag_color);
                });
            }
        }
        else if(section==menu_section::items)
        {
            if(ImGui::CollapsingHeader("Dropped items",ImGuiTreeNodeFlags_DefaultOpen)){
                const auto itemOptions=[&]{color_picker("Color",g_settings.item_color);
                    ImGui::SliderFloat("Range",&g_settings.max_distance_meters,25,500,"%.0f m");};
                Draw("Weapons",g_settings.dropped_weapons,itemOptions);
                Draw("Grenades",g_settings.dropped_grenades,itemOptions);
                Draw("Objectives",g_settings.dropped_objectives,itemOptions);
                Draw("Other objects",g_settings.other_entities,itemOptions);
                Draw("Names",g_settings.item_names,[&]{textOptions(g_settings.item_style,g_settings.item_color);});
                Draw("Distance",g_settings.item_distance,[&]{option_row::Style(g_settings.item_style,textStyles);});
                Draw("Bomb countdown",g_settings.objective_timer,[&]{option_row::Style(g_settings.item_style,textStyles);});
            }
            if(ImGui::CollapsingHeader("Thrown grenades",ImGuiTreeNodeFlags_DefaultOpen)){
                Draw("Grenade names",g_settings.projectile_names,[&]{
                    textOptions(g_settings.projectile_text_style,g_settings.projectile_color);
                    option_row::Style(g_settings.projectile_marker,markers,"Marker");
                    ImGui::SliderFloat("Marker size",&g_settings.projectile_size,2,12,"%.0f px");
                });
                Draw("Grenade countdowns",g_settings.projectile_timer,[&]{option_row::Style(g_settings.projectile_text_style,textStyles);});
                Draw("Flight paths",g_settings.projectile_path,[&]{
                    option_row::Style(g_settings.trajectory_style,lines);
                    ImGui::SliderFloat("Line width",&g_settings.trajectory_thickness,.5f,4,"%.1f px");
                    color_picker("Color",g_settings.projectile_color);
                });
            }
        }
        else
        {
            Draw("FOV guide",g_settings.fov_guide,[&]{
                option_row::Style(g_settings.fov_style,lines);
                ImGui::SliderFloat("Angle",&g_settings.guide_degrees,1,30,"%.1f deg");
                ImGui::SliderFloat("Line width",&g_settings.fov_thickness,.5f,4,"%.1f px");color_picker("Color",g_settings.guide_color);
            });
            Draw("Movement speed",g_settings.speed_widget,[&]{textOptions(g_settings.speed_text_style,g_settings.speed_color);});
            Draw("Bomb panel",g_settings.bomb_widget,[&]{ImGui::SliderFloat("Text size",&g_settings.text_size,10,20,"%.0f px");});
            Draw("Hit marker",g_settings.hit_marker,[&]{
                option_row::Style(g_settings.hit_marker_style,hits);
                ImGui::SliderFloat("Size",&g_settings.hit_marker_size,4,18,"%.0f px");
                ImGui::SliderFloat("Line width",&g_settings.hit_marker_thickness,.5f,4,"%.1f px");color_picker("Color",g_settings.hit_color);
            });
        }
    }
}
