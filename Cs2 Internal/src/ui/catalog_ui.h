#pragma once
#include <cstddef>
#include <string>

struct ID3D11Device;

namespace catalog_ui
{
    std::string AgentName(const std::string &model);
    std::string SelectedAgentPreview();
    void PreviewAgent(const std::string &model);
    bool ConsumeAgentPreviewRequest();
    void PreviewEquippedAgent();
    std::string WeaponName(int definition);
    void DrawEquippedFinish(ID3D11Device *device, int definition, int paintkit, const std::string &model = {});
    // Call within an active ImGui frame/tab on the render thread.
    enum class Page {Weapons, Knives, Gloves, Agents, CustomAgents};
    void Draw(ID3D11Device *device, Page page=Page::Weapons);
    // Call before destroying the renderer or changing its D3D11 device.
    void Shutdown();
    // Optional read-only live schema IDs. nullptr marks the snapshot unavailable.
    // All gallery entry points must be serialized on the render thread.
    void SetLiveDefinitionIndices(const int *indices, std::size_t count);
} // namespace catalog_ui
