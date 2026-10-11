#pragma once
#include <functional>
#include <string>
#include "nlohmann/json.hpp"
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace gui_bridge {
using DrawPage = std::function<void(int)>;
bool Start(bool initializing=false);
void Loading(unsigned progress,const char* message);
void FinishLoading(); // Releases controls once native features and the frontend are ready.
bool GuiReady(); // Frontend has mounted and displayed the loading screen.
void Stop();
// Requests wait for the render thread; callers must not hold its render lock.
std::string Request(const std::string& request);
void DestroyCapture();
void Process(const DrawPage& draw, ID3D11Device* device=nullptr, ID3D11DeviceContext* context=nullptr);
bool SessionActive();
bool MenuVisible();
// Also used by the standalone production-menu integration fixture.
nlohmann::json Execute(const nlohmann::json& request, const DrawPage& draw,
                       ID3D11Device* device=nullptr, ID3D11DeviceContext* context=nullptr);
}
