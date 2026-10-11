#pragma once
#include "platform/windows.h"
#include "imgui/imgui.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace menu_style {
inline ImFont *body{}, *smallFont{}, *nav{};
inline constexpr ImU32 ink=IM_COL32(16,17,20,255), paper=IM_COL32(224,225,230,255),
    muted=IM_COL32(133,135,145,255), crimson=IM_COL32(189,74,81,255), line=IM_COL32(43,44,51,230);
inline float Animate(ImGuiStorage *state,ImGuiID id,float target,float rate=22.f){
    // An exponential response has the same duration at every frame rate.
    const int frame=ImGui::GetFrameCount();
    const int last=state->GetInt(id^0xa71348d1u,frame);
    const float previous=last<frame-1?target:state->GetFloat(id,target);
    float value=target+(previous-target)*std::exp(-rate*std::max(0.f,ImGui::GetIO().DeltaTime));
    if(std::abs(value-target)<.0005f)value=target;
    state->SetFloat(id,value);state->SetInt(id^0xa71348d1u,frame);return value;
}
inline ImU32 Blend(ImU32 from,ImU32 to,float amount){
    const auto a=ImGui::ColorConvertU32ToFloat4(from),b=ImGui::ColorConvertU32ToFloat4(to);
    return ImGui::GetColorU32(ImVec4(a.x+(b.x-a.x)*amount,a.y+(b.y-a.y)*amount,
        a.z+(b.z-a.z)*amount,a.w+(b.w-a.w)*amount));
}
inline void Initialize(){
    ImGui::StyleColorsDark();
    auto &s=ImGui::GetStyle();
    s.CustomControls=true;
    s.WindowPadding={20,20};s.FramePadding={10,5};s.ItemSpacing={10,9};s.ItemInnerSpacing={10,6};
    s.WindowRounding=8;s.ChildRounding=5;s.FrameRounding=4;s.PopupRounding=5;s.ScrollbarRounding=4;s.GrabRounding=4;
    s.WindowBorderSize=1;s.ChildBorderSize=1;s.PopupBorderSize=1;s.FrameBorderSize=1;
    s.ScrollbarSize=5;s.GrabMinSize=12;s.IndentSpacing=18;s.WindowTitleAlign={0,0.5f};
    auto *c=s.Colors;
    const auto rgba=[](int r,int g,int b,int a=255){return ImVec4(r/255.f,g/255.f,b/255.f,a/255.f);};
    const auto accent=rgba(189,74,81);
    c[ImGuiCol_Text]=rgba(224,225,230);c[ImGuiCol_TextDisabled]=rgba(133,135,145);
    c[ImGuiCol_WindowBg]=rgba(20,21,24,235);c[ImGuiCol_ChildBg]=rgba(25,26,30,210);
    c[ImGuiCol_PopupBg]=rgba(28,29,34);c[ImGuiCol_Border]=rgba(43,44,51);c[ImGuiCol_BorderShadow]={0,0,0,0};
    c[ImGuiCol_FrameBg]=rgba(34,35,41);c[ImGuiCol_FrameBgHovered]=rgba(44,45,53);c[ImGuiCol_FrameBgActive]=rgba(50,36,43);
    c[ImGuiCol_TitleBg]=rgba(20,21,24);c[ImGuiCol_TitleBgActive]=rgba(20,21,24);c[ImGuiCol_TitleBgCollapsed]=c[ImGuiCol_TitleBg];
    c[ImGuiCol_MenuBarBg]=c[ImGuiCol_WindowBg];c[ImGuiCol_ScrollbarBg]={0,0,0,0};
    c[ImGuiCol_ScrollbarGrab]=rgba(55,57,66);c[ImGuiCol_ScrollbarGrabHovered]=rgba(77,79,91);c[ImGuiCol_ScrollbarGrabActive]=accent;
    c[ImGuiCol_CheckMark]=accent;c[ImGuiCol_SliderGrab]=accent;c[ImGuiCol_SliderGrabActive]=rgba(206,89,96);
    c[ImGuiCol_Button]=rgba(36,37,44);c[ImGuiCol_ButtonHovered]=rgba(45,40,47);c[ImGuiCol_ButtonActive]=rgba(59,37,46);
    c[ImGuiCol_Header]=rgba(31,32,38);c[ImGuiCol_HeaderHovered]=rgba(40,41,48);c[ImGuiCol_HeaderActive]=rgba(50,34,43);
    c[ImGuiCol_Separator]=rgba(43,44,51);c[ImGuiCol_SeparatorHovered]=accent;c[ImGuiCol_SeparatorActive]=accent;
    c[ImGuiCol_ResizeGrip]=rgba(133,135,145,40);c[ImGuiCol_ResizeGripHovered]=rgba(189,74,81,165);c[ImGuiCol_ResizeGripActive]=accent;
    c[ImGuiCol_Tab]=rgba(31,32,38);c[ImGuiCol_TabHovered]=c[ImGuiCol_ButtonHovered];c[ImGuiCol_TabActive]=rgba(50,34,43);
    c[ImGuiCol_TabUnfocused]=c[ImGuiCol_Tab];c[ImGuiCol_TabUnfocusedActive]=c[ImGuiCol_TabActive];
    c[ImGuiCol_PlotLines]=c[ImGuiCol_CheckMark];c[ImGuiCol_PlotHistogram]=c[ImGuiCol_CheckMark];
    c[ImGuiCol_TextSelectedBg]=rgba(189,74,81,64);c[ImGuiCol_NavHighlight]=rgba(189,74,81,166);
    c[ImGuiCol_ModalWindowDimBg]={.015f,.018f,.025f,.72f};
    ImGui::SetColorEditOptions(ImGuiColorEditFlags_DisplayHex|ImGuiColorEditFlags_Uint8|
        ImGuiColorEditFlags_PickerHueBar|ImGuiColorEditFlags_AlphaBar|ImGuiColorEditFlags_AlphaPreviewHalf);
    char folder[MAX_PATH]{};GetWindowsDirectoryA(folder,MAX_PATH);
    const std::string fonts=std::string(folder)+"/Fonts/";
    auto &io=ImGui::GetIO();
    const auto load=[&](const char *file,float size,const ImWchar *ranges=nullptr){
        const auto path=fonts+file;ImFontConfig config;config.OversampleH=3;config.OversampleV=2;config.PixelSnapH=false;
        if(GetFileAttributesA(path.c_str())!=INVALID_FILE_ATTRIBUTES)
            if(auto *font=io.Fonts->AddFontFromFileTTF(path.c_str(),size,&config,ranges))return font;
        config.SizePixels=size;return io.Fonts->AddFontDefault(&config);
    };
    body=load("segoeui.ttf",16);smallFont=load("seguisb.ttf",12);nav=load("seguisb.ttf",16);io.FontDefault=body;
}
inline void Text(ImDrawList *draw,ImFont *font,ImVec2 pos,ImU32 color,const char *text){
    if(font)draw->AddText(font,font->FontSize,pos,color,text);else draw->AddText(pos,color,text);
}
inline void Moon(ImDrawList *draw,ImVec2 origin,float scale){
    const auto p=[&](float x,float y){return ImVec2(origin.x+x*scale,origin.y+y*scale);};
    const auto stroke=[&](float x,float y,float xx,float yy,ImU32 color,float width){draw->AddLine(p(x,y),p(xx,yy),color,width*scale);};
    const auto poly=[&](std::initializer_list<ImVec2> points,ImU32 color){std::array<ImVec2,16> transformed{};int n=0;for(auto v:points)transformed[n++]=p(v.x,v.y);draw->AddConvexPolyFilled(transformed.data(),n,color);};
    const auto black=IM_COL32(13,14,18,255);
    draw->AddCircle(p(100,100),83*scale,IM_COL32(207,80,68,255),96,scale);
    draw->AddCircleFilled(p(100,100),76*scale,crimson,96);
    draw->AddCircleFilled(p(102,95),69*scale,IM_COL32(195,58,48,255),96);
    // Sparse ink strokes keep the moon organic without a texture or animation.
    for(int i=0;i<7;++i)stroke(32.f+i*3,65.f+i*14,61.f+i*4,59.f+i*14,IM_COL32(142,26,41,105),1);
    draw->AddCircleFilled(p(106,75),14*scale,black,32);
    poly({{93,79},{115,82},{130,120},{97,135},{76,113}},black);
    poly({{88,101},{108,111},{70,166},{48,157}},black);
    poly({{108,115},{130,116},{150,147},{141,160},{114,139}},black);
    poly({{70,151},{86,160},{76,175},{40,175},{43,168}},black);
    poly({{131,146},{146,145},{161,168},{155,175},{126,167}},black);
    stroke(92,91,73,71,black,16);stroke(73,71,67,48,black,12);
    draw->AddCircleFilled(p(66,46),7*scale,black,16);
    stroke(67,48,155,19,black,5);stroke(83,41,157,17,IM_COL32(224,123,128,255),1);
    stroke(73,37,78,53,black,4);stroke(55,52,67,48,black,7);
    stroke(119,98,136,112,black,15);stroke(136,112,159,97,black,11);
    draw->AddCircleFilled(p(159,96),6*scale,black,16);
    stroke(163,119,155,47,black,5);stroke(155,47,157,43,IM_COL32(224,123,128,255),1);
    stroke(151,105,170,102,black,4);
    poly({{94,75},{96,83},{68,94},{41,88}},black);
    poly({{82,92},{97,92},{67,108},{39,108}},black);
    stroke(98,73,110,70,IM_COL32(218,174,172,255),2);
    // Bamboo silhouettes frame the lower edge of the emblem.
    for(int side=0;side<2;++side){const float x=side?178.f:22.f,sign=side?-1.f:1.f;
        stroke(x,178,x+sign*9,113,black,2);
        for(int i=0;i<4;++i){const float y=132.f+i*11;poly({{x+sign*6,y},{x+sign*31,y-18},{x+sign*15,y-2}},black);
            poly({{x+sign*4,y+4},{x-sign*14,y-15},{x-sign*3,y}},black);}}
    stroke(27,178,174,178,IM_COL32(45,27,34,255),1);
}
}
