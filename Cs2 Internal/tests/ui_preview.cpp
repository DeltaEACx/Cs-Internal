// Exercise actual UI + native model loader on WARP, without loading or writing CS2.
#include "support/render_capture.h"
#include "ui/catalog_ui.cpp"
#include "ui/agent_preview.cpp"
#include "features/view/view_settings.cpp"
#include "imgui/imgui_impl_dx11.h"
#include <chrono>

int wmain(int argc, wchar_t **argv)
{
    try
    {
        Check(argc >= 2, "Usage: ui_preview output.png [agent path] [view|custom|gloves]");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                          &device, nullptr, &context)),
              "WARP device");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 1500;
        desc.Height = 900;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> target;
        ComPtr<ID3D11ShaderResourceView> view;
        Check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)), "UI texture");
        device->CreateRenderTargetView(texture.Get(), nullptr, &target);
        device->CreateShaderResourceView(texture.Get(), nullptr, &view);
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1500, 900);
        io.DeltaTime = 1.f / 60;
        ImGui::StyleColorsDark();
        Check(ImGui_ImplDX11_Init(device.Get(), context.Get()), "UI backend");
        g_data = LoadDataset((runtime_paths::DataRoot() / L"game_assets"));
        g_started = true;
        bool open = true;
        if (argc > 2)
        {
            const auto utf8 = std::filesystem::path(argv[2]).generic_u8string();
            g_previewAgent.assign(utf8.begin(), utf8.end());
            g_customAgents = character_catalog::Discover();
            g_customAgentsScanned = true;
        }
        const bool viewTab = argc > 3 && std::wstring_view(argv[3]) == L"view";
        const bool gloveTab = argc > 3 && std::wstring_view(argv[3]) == L"gloves";
        if (viewTab)
        {
            view_settings::cameraReady = true;
            view_settings::aspectReady = true;
            view_settings::aspectRequested = true;
        }
        for (int frame = 0; frame < 900; ++frame)
        {
            {
                std::lock_guard lock(agent_preview::mutex);
                agent_preview::current = {true,
                                          7,
                                          0,
                                          GetTickCount64(),
                                          "agents/models/ctm_sas/ctm_sas.vmdl",
                                          "weapons/models/ak47/weapon_rif_ak47.vmdl",
                                          "idle_rifle"};
            }
            if (argc > 3)
            {
                io.MouseDown[0] = false;
            }
            ImGui_ImplDX11_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(25, 35));
            ImGui::SetNextWindowSize(ImVec2(980, 800));
            ImGui::Begin("\\DeltaEAC Skin Changer");
            ImGui::Checkbox("Agent preview", &open);
            ImGui::Separator();
            if(viewTab)view_settings::Draw();
            else catalog_ui::Draw(device.Get(),argc>3?(gloveTab?catalog_ui::Page::Gloves:catalog_ui::Page::CustomAgents):catalog_ui::Page::Weapons);
            ImGui::End();
            ImGui::SetNextWindowPos(ImVec2(1030, 35));
            ImGui::SetNextWindowSize(ImVec2(440, 800));
            agent_preview::Draw(device.Get(), context.Get(), &open);
            ImGui::Render();
            const float background[] = {.018f, .022f, .032f, 1};
            context->ClearRenderTargetView(target.Get(), background);
            auto *rtv = target.Get();
            context->OMSetRenderTargets(1, &rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if (frame > 30 && agent_preview::renderer.Ready())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(agent_preview::renderer.Ready(), "Native equipped preview did not load");
        if (argc > 2)
            Check(agent_preview::displayedKey.starts_with(g_previewAgent + "|"),
                  "Selected custom agent was not rendered");
        if(argc>3&&!viewTab)
            Check(g_tabCategory==(gloveTab?Category::Gloves:Category::CustomAgents),"Requested catalog page did not activate");
        SavePng(device.Get(), context.Get(), view.Get(), argv[1]);
        agent_preview::Shutdown();
        catalog_ui::Shutdown();
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
        CoUninitialize();
        std::cout << "PASS: catalog pages and native second-window preview\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        agent_preview::Shutdown();
        return 1;
    }
}
