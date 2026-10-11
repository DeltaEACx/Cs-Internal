#pragma once
#include <d3d11.h>
#include "render/preview_material.h"
namespace agent_preview
{
    enum class WebAction { Frame, Rotate, Reset, Reload, Equipped, Drag, Zoom };
    struct WebFrame {
        ID3D11ShaderResourceView* texture{}; // Borrowed until the next render call.
        std::string name, weapon, status{"empty"}, message;
        bool rotate{};
    };
    WebFrame RenderWeb(ID3D11Device*, ID3D11DeviceContext*, WebAction action=WebAction::Frame,
                       float x=0, float y=0, bool enabled=false, unsigned width=480, unsigned height=960);
    void SetImageFinish(const preview_scene::ImageFinish &image);
    void Capture(); // Game-thread snapshot after post-data update; no model writes.
    void Draw(ID3D11Device *device, ID3D11DeviceContext *context, bool *open);
    void Shutdown();
} // namespace agent_preview
