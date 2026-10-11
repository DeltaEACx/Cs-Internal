#include "../../tests/support/render_capture.h"
#include "nlohmann/json.hpp"
#include <fstream>
int wmain(int argc, wchar_t **argv)
{
    try
    {
        Check(argc == 3, "Usage: agent_thumbnails preview_models custom_agent_exports.json");
        const std::filesystem::path root = argv[1];
        std::ifstream stream(argv[2]);
        const auto models = nlohmann::json::parse(stream);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                          &device, nullptr, &context)),
              "Thumbnail renderer");
        std::filesystem::create_directories(root / L"thumbnails");
        for (const auto &entry : models)
        {
            const auto name = entry.at("preview").get<std::string>();
            auto file = std::filesystem::path(std::u8string(name.begin(), name.end()));
            std::cout << name << std::endl;
            const auto model =
                preview_scene::Load(root / file, "idle_rifle", true, root / L"agents_models_ctm_sas_ctm_sas_vmdl.glb");
            preview_scene::Renderer renderer;
            renderer.Upload(device.Get(), model);
            file.replace_extension(L".png");
            SavePng(device.Get(), context.Get(), renderer.Render(context.Get(), 384, 384, -.8f, .03f, 1.05f),
                    (root / L"thumbnails" / file).c_str());
        }
        CoUninitialize();
        std::cout << "PASS: " << models.size() << " custom agent thumbnails\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
