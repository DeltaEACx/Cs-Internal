#pragma once
#include "imgui/imgui.h"
#include "ui/gui_capture.h"
#include <algorithm>

namespace option_row {
// Each row owns a separate popup ID. Options stay available while its feature
// is disabled, so appearance can be prepared without enabling the overlay.
template<class Options> void Draw(const char *label,bool &enabled,Options options){
    ImGui::PushID(label);
    if(gui_capture::active){
        ImGui::Checkbox(label,&enabled);
        gui_capture::active->BeginGroup(label,"popup",ImGui::GetID("##options"));
        options();
        gui_capture::active->EndGroup();
        ImGui::PopID();
        return;
    }
    const float start=ImGui::GetCursorPosX(),width=ImGui::GetContentRegionAvail().x;
    const float height=ImGui::GetFrameHeight();
    ImGui::Checkbox(label,&enabled);
    ImGui::SameLine(start+std::max(0.f,width-32.f));
    const auto pos=ImGui::GetCursorScreenPos();
    const bool open=ImGui::IsPopupOpen("Options");
    const auto accent=ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(accent.x,accent.y,accent.z,open?.12f:0.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(accent.x,accent.y,accent.z,.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(accent.x,accent.y,accent.z,.23f));
    const bool clicked=ImGui::Button("##options",{32,height});
    ImGui::PopStyleColor(3);
    if(clicked)ImGui::OpenPopup("Options");
    const bool hovered=ImGui::IsItemHovered();
    auto *draw=ImGui::GetWindowDrawList();
    const auto color=ImGui::GetColorU32((open||clicked)?ImGuiCol_CheckMark: (hovered?ImGuiCol_Text:ImGuiCol_TextDisabled));
    for(int dot=0;dot<3;++dot)draw->AddCircleFilled({pos.x+10+dot*6,pos.y+height*.5f},1.5f,color,12);
    if(hovered&&!open&&!clicked)ImGui::SetTooltip("%s options",label);
    ImGui::SetNextWindowSizeConstraints({320,0},{420,std::max(180.f,ImGui::GetIO().DisplaySize.y-24)});
    if(ImGui::BeginPopup("Options")){
        ImGui::TextUnformatted(label);ImGui::Separator();
        ImGui::PushItemWidth(175);options();ImGui::PopItemWidth();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}
template<class Enum,int Count> bool Style(Enum &value,const char *const (&names)[Count],const char *label="Style"){
    int selected=std::clamp(static_cast<int>(value),0,Count-1);
    if(!ImGui::Combo(label,&selected,names,Count))return false;
    value=static_cast<Enum>(selected);return true;
}
}
