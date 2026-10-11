#pragma once
#include "ui/menu_style.h"
#include <iterator>

namespace menu_shell {
struct Page {int group;const char *label;const char *description;};
inline ImVec2 previewPosition{},previewSize{};
inline constexpr const char *groups[]{"Combat","Skins","Players","Effects","World","View","Local","Movement"};
inline constexpr int groupOrder[]{0,7,2,6,1,4,3,5};
inline constexpr Page pages[]{
    {1,"Weapons","Weapon finishes and custom models."},{1,"Knives","Knife models and finishes."},
    {1,"Gloves","Glove models and finishes."},{1,"Agents","Agents from the game catalog."},{1,"Custom Agents","Installed character models."},
    {5,"Camera","Third person and aspect ratio."},{2,"ESP","Player markers and information."},
    {4,"Dropped items","Dropped items, bomb and grenades."},{5,"Widgets","Speed, bomb status and hit feedback."},
    {2,"Chams","Visible and through-wall materials."},{3,"Particles","Gun and hand particle effects."},
    {3,"Impact Effects","Combat and movement effects."},{4,"Sky","Sky textures and color."},
    {4,"Lighting","Map lighting, bloom and brightness."},{4,"Atmosphere","Fog, wind and wetness."},
    {4,"Particles","World particle effects."},{5,"Crosshair","Surface crosshair and scope settings."},
    {4,"Physics","Jump effects and ragdoll gravity."},{2,"Glow & radar","Glow, opacity and radar."},
    {5,"Visibility","Flash, crosshair and scope removals."},{4,"Throwables","Trajectory, fire and smoke."},
    {3,"Images","Image finishes for your held weapon."},{2,"Models","Custom enemy character models."},
    {0,"Rage","Targeting and firing controls."},{0,"Anti Aim","Body orientation and spin."},
    {7,"Assists","Jumping, strafing and ground assists."},{0,"Surfaces","Highlight penetrable map surfaces."},
    {0,"History","Recent enemy poses and historical chams."},{6,"Player","Local player glow, model and effects."},
    {0,"Legit","Unavailable in this build."},{3,"Weapon Camo","Weapon camo presets."}
};
inline constexpr int navigationPages[]{23,24,25,6,9,22,28,0,1,2,3,4,7,20,12,13,14,15,17,30,10,11,21,5,16,8};
inline int firstPage(int group){for(int i:navigationPages)if(pages[i].group==group)return i;return 23;}

template<class Content> void Draw(int &page,bool &open,bool &preview,Content content){
    using namespace menu_style;if(!open)return;
    page=std::clamp(page,0,static_cast<int>(std::size(pages))-1);
    const auto display=ImGui::GetIO().DisplaySize;
    const float maxW=std::max(360.f,display.x-24),maxH=std::max(360.f,display.y-24);
    ImGui::SetNextWindowSize({std::min(1060.f,maxW),std::min(748.f,maxH)},ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos({std::max(12.f,(display.x-1060)*.5f),std::max(12.f,(display.y-748)*.5f)},ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({std::min(880.f,maxW),std::min(600.f,maxH)},{maxW,maxH});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
    const bool visible=ImGui::Begin("DeltaEAC###VeloraMain",&open,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if(visible){
        auto *draw=ImGui::GetWindowDrawList();const auto pos=ImGui::GetWindowPos(),size=ImGui::GetWindowSize();
        const bool compact=false;
        const float rail=132.f,footer=16.f;
        const float navTop=size.y<460?74.f:90.f;
        const float rowHeight=std::min(44.f,(size.y-footer-navTop-24.f)/std::size(groups)-2.f);
        const float previewW=std::clamp(size.x*.27f,164.f,292.f);
        const float contentX=rail+18,contentW=size.x-contentX-previewW-18;
        previewPosition={pos.x+size.x-previewW,pos.y};previewSize={previewW,size.y};
        draw->AddRectFilled(pos,{pos.x+rail,pos.y+size.y-footer},ink,8,ImDrawCornerFlags_TopLeft);
        draw->AddLine({pos.x+rail,pos.y+20},{pos.x+rail,pos.y+size.y-footer-20},line);

        Text(draw,nav,{pos.x+25,pos.y+29},paper,"DeltaEAC");

        auto *state=ImGui::GetStateStorage();
        const auto memoryId=[](int group){ImGui::PushID(group);const auto id=ImGui::GetID("last_page");ImGui::PopID();return id;};
        state->SetInt(memoryId(pages[page].group),page);
        draw->AddRectFilled({pos.x+10,pos.y+navTop-6},{pos.x+rail-10,pos.y+navTop+std::size(groups)*(rowHeight+2)+4},IM_COL32(24,25,29,230),7);
        for(int row=0;row<std::size(groupOrder);++row){
            const int g=groupOrder[row];
            const auto rememberedId=memoryId(g);
            ImGui::PushID(g);ImGui::SetCursorPos({14.f,navTop+row*(rowHeight+2)});
            const auto p=ImGui::GetCursorScreenPos();const float width=rail-(compact?20.f:24.f);
            if(ImGui::InvisibleButton("group",{width,rowHeight})){
                const int remembered=state->GetInt(rememberedId,firstPage(g));
                page=remembered>=0&&remembered<std::size(pages)&&pages[remembered].group==g?remembered:firstPage(g);
            }
            const bool selected=pages[page].group==g,hovered=ImGui::IsItemHovered();
            const float hover=Animate(state,ImGui::GetID("group_hover"),hovered&&!selected?1.f:0.f);
            const float active=Animate(state,ImGui::GetID("group_active"),selected?1.f:0.f);
            const auto railText=muted,railHover=paper;
            draw->AddRectFilled(p,{pos.x+rail-14,p.y+rowHeight},Blend(IM_COL32(30,31,36,static_cast<int>(hover*255)),IM_COL32(105,45,52,235),active),5);
            ImGui::PushFont(nav);const auto labelSize=ImGui::CalcTextSize(groups[g]);ImGui::PopFont();
            Text(draw,nav,{p.x+(rail-28-labelSize.x)*.5f,p.y+(rowHeight-labelSize.y)*.5f-1},Blend(Blend(railText,railHover,hover),paper,active),groups[g]);
            ImGui::PopID();
        }

        float tabX=contentX,tabY=18;
        const float tabRight=size.x-previewW-18;
        const int activeGroup=pages[page].group;
        ImGui::PushFont(nav);
        for(int i:navigationPages){
            if(pages[i].group!=activeGroup)continue;
            const auto labelSize=ImGui::CalcTextSize(pages[i].label);
            const float width=labelSize.x+28;
            if(tabX>contentX&&tabX+width>tabRight){tabX=contentX;tabY+=44;}
            ImGui::SetCursorPos({tabX,tabY});ImGui::PushID(i);
            const auto p=ImGui::GetCursorScreenPos();
            if(ImGui::InvisibleButton("page",{width,38}))page=i;
            const bool selected=page==i,hovered=ImGui::IsItemHovered();
            const float hover=Animate(state,ImGui::GetID("tab_hover"),hovered?1.f:0.f);
            const float active=Animate(state,ImGui::GetID("tab_active"),selected?1.f:0.f,26.f);
            draw->AddRectFilled(p,{p.x+width,p.y+38},Blend(IM_COL32(20,21,24,255),IM_COL32(37,31,37,255),std::max(hover*.7f,active*.8f)),4);
            draw->AddText({p.x+14,p.y+(38-labelSize.y)*.5f-1},Blend(muted,paper,std::max(active,hover)),pages[i].label);
            if(active>.001f){const float half=(width-28)*.5f*active;
                draw->AddRectFilled({p.x+width*.5f-half,p.y+35},{p.x+width*.5f+half,p.y+37},Blend(IM_COL32(189,74,81,0),crimson,active),1);}
            ImGui::PopID();tabX+=width+6;
        }
        ImGui::PopFont();
        state->SetInt(memoryId(pages[page].group),page);

        ImGui::SetCursorPos({size.x-previewW-40,25});const auto close=ImGui::GetCursorScreenPos();
        if(ImGui::InvisibleButton("Close menu",{24,24}))open=false;
        const bool closeHovered=ImGui::IsItemHovered();
        const float closeHover=Animate(state,ImGui::GetID("close_hover"),closeHovered?1.f:0.f);
        if(closeHover>.001f)draw->AddRectFilled(close,{close.x+24,close.y+24},ImGui::GetColorU32(ImVec4(.73f,.2f,.17f,closeHover*.1f)),4);
        const auto closeColor=Blend(muted,crimson,closeHover);
        draw->AddLine({close.x+8,close.y+8},{close.x+16,close.y+16},closeColor,1.5f);
        draw->AddLine({close.x+16,close.y+8},{close.x+8,close.y+16},closeColor,1.5f);
        if(closeHovered)ImGui::SetTooltip("Close  /  Insert");

        const float controlsY=tabY+54;
        draw->AddLine({pos.x+contentX+2,pos.y+controlsY-8},{pos.x+size.x-previewW-18,pos.y+controlsY-8},line);
        ImGui::SetCursorPos({contentX,controlsY});
        const auto fadeId=ImGui::GetID("page_fade"),fadePageId=ImGui::GetID("page_fade_page");
        if(state->GetInt(fadePageId,-1)!=page){
            state->SetInt(fadePageId,page);state->SetFloat(fadeId,.25f);
            state->SetInt(fadeId^0xa71348d1u,ImGui::GetFrameCount());
        }
        const float pageAlpha=Animate(state,fadeId,1.f,24.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{22,18});ImGui::PushID(page);
        if(ImGui::BeginChild("Controls",{contentW,std::max(80.f,size.y-controlsY-footer-16)},false,ImGuiWindowFlags_AlwaysUseWindowPadding)){
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,ImGui::GetStyle().Alpha*pageAlpha);
            ImGui::PushItemWidth(std::clamp(ImGui::GetContentRegionAvail().x*.64f,175.f,440.f));
            content(page);ImGui::PopItemWidth();
            ImGui::PopStyleVar();
        }ImGui::EndChild();ImGui::PopID();ImGui::PopStyleVar();

        preview=true;
    }
    ImGui::End();
}
}
