#include "support/render_capture.h"
namespace
{
    ComPtr<ID3D11ShaderResourceView> Solid(ID3D11Device *device, unsigned rgba)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{&rgba, 4, 0};
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        Check(SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)) &&
              SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &view)), "Skin texture");
        return view;
    }
    std::vector<unsigned> Pixels(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11ShaderResourceView *view)
    {
        ComPtr<ID3D11Resource> source;
        view->GetResource(&source);
        ComPtr<ID3D11Texture2D> texture;
        Check(SUCCEEDED(source.As(&texture)), "Readback source");
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        Check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "Readback staging");
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE data{};
        Check(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &data)), "Readback map");
        std::vector<unsigned> pixels(size_t(desc.Width) * desc.Height);
        for (unsigned y = 0; y < desc.Height; ++y)
            std::memcpy(pixels.data() + y * desc.Width, static_cast<const char *>(data.pData) + y * data.RowPitch,
                        desc.Width * sizeof(unsigned));
        context->Unmap(staging.Get(), 0);
        return pixels;
    }
}
int wmain(int argc, wchar_t **argv)
{
    try
    {
        Check(argc >= 4, "Usage: preview_regression agent.glb weapon.glb output.png [yaw] [--skin]");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto agent = preview_scene::Load(argv[1], "idle_rifle", true);
        Check(agent.hasHand && agent.posed, "Agent hand / idle pose missing");
        auto weapon = preview_scene::Load(argv[2], "", false);
        const auto count = agent.parts.size();
        preview_scene::Attach(agent, std::move(weapon));
        Check(agent.parts.size() > count, "No attached weapon");
        for (const auto &part : agent.parts)
            for (const auto &v : part.vertices)
                Check(std::isfinite(v.position.x) && std::isfinite(v.position.y) && std::isfinite(v.position.z),
                      "Non-finite mesh");
        std::cout << "Parts " << agent.parts.size() << ", hand " << agent.hand._41 << ' ' << agent.hand._42 << ' '
                  << agent.hand._43 << '\n';
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                          &device, nullptr, &context)),
              "WARP device");
        preview_scene::Renderer renderer;
        renderer.Upload(device.Get(), agent);
        const D3D11_VIEWPORT before{3, 7, 345, 210, .2f, .7f};
        context->RSSetViewports(1, &before);
        auto *view = renderer.Render(context.Get(), 600, 800, argc > 4 ? float(_wtof(argv[4])) : .35f, .05f, 1.f);
        D3D11_VIEWPORT after{};
        UINT n = 1;
        context->RSGetViewports(&n, &after);
        Check(n == 1 && !memcmp(&before, &after, sizeof(before)), "Preview did not restore game viewport state");
        SavePng(device.Get(), context.Get(), view, argv[3]);
        Check(renderer.Render(context.Get(), 320, 480, 0, 0, 1) != nullptr, "Preview resize failed");
        renderer.Reset();
        Check(!renderer.Ready(), "Preview reset retained resources");
        if (argc > 5 && std::wstring_view(argv[5]) == L"--skin")
        {
            auto skinWeapon = preview_scene::Load(argv[2], "", false);
            const auto material = skinWeapon.parts.front().material;
            Check(!material.empty(), "Weapon material name missing");
            const auto red = Solid(device.Get(), 0xff2020ff), blue = Solid(device.Get(), 0xffff2020);
            Check(!preview_scene::ApplyFinish(skinWeapon, {{"unrelated.vmat", red}}), "Unrelated material accepted");
            auto compiledName = material + "_c";
            for (char &ch : compiledName) if (ch == '/') ch = '\\';
            Check(preview_scene::ApplyFinish(skinWeapon, {{compiledName, red}}) > 0, "Compiled material path mismatch");
            auto pose = preview_scene::Load(argv[1], "idle_rifle", true);
            const auto bodyParts = pose.parts.size();
            preview_scene::Attach(pose, skinWeapon);
            for (size_t i = 0; i < bodyParts; ++i) Check(!pose.parts[i].liveTexture, "Skin recolored the agent");
            renderer.Upload(device.Get(), pose);
            const auto redPixels = Pixels(device.Get(), context.Get(), renderer.Render(context.Get(), 600, 800, .35f, .05f, 1));
            Check(preview_scene::ApplyFinish(skinWeapon, {{material, blue}}) > 0, "Second finish was not applied");
            pose = preview_scene::Load(argv[1], "idle_rifle", true);
            preview_scene::Attach(pose, skinWeapon);
            renderer.Upload(device.Get(), pose);
            const auto bluePixels = Pixels(device.Get(), context.Get(), renderer.Render(context.Get(), 600, 800, .35f, .05f, 1));
            unsigned differences = 0;
            for (size_t i = 0; i < redPixels.size(); ++i) differences += redPixels[i] != bluePixels[i];
            Check(differences > 100, "Changing skin did not change the rendered weapon");
            auto imageWeapon=preview_scene::Load(argv[2],"",false);
            preview_scene::ImageFinish finish;finish.texture=red;
            preview_scene::ApplyImage(imageWeapon,finish);
            pose=preview_scene::Load(argv[1],"idle_rifle",true);preview_scene::Attach(pose,imageWeapon);
            for(size_t i=0;i<bodyParts;++i)Check(!pose.parts[i].image.texture,"Image finish affected agent geometry");
            renderer.Upload(device.Get(),pose);
            const auto wrapped=Pixels(device.Get(),context.Get(),renderer.Render(context.Get(),600,800,.35f,.05f,1));
            D3D11_TEXTURE2D_DESC cubeDesc{};cubeDesc.Width=cubeDesc.Height=cubeDesc.MipLevels=1;cubeDesc.ArraySize=6;
            cubeDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;cubeDesc.SampleDesc.Count=1;cubeDesc.Usage=D3D11_USAGE_IMMUTABLE;
            cubeDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;cubeDesc.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;
            unsigned faceColor=0xffffff20;D3D11_SUBRESOURCE_DATA faces[6]{};for(auto &face:faces)face={&faceColor,4,0};
            ComPtr<ID3D11Texture2D> cube;Check(SUCCEEDED(device->CreateTexture2D(&cubeDesc,faces,&cube)),"Cube texture");
            finish.cube=true;Check(SUCCEEDED(device->CreateShaderResourceView(cube.Get(),nullptr,&finish.texture)),"Cube view");
            preview_scene::ApplyImage(imageWeapon,finish);pose=preview_scene::Load(argv[1],"idle_rifle",true);preview_scene::Attach(pose,imageWeapon);
            renderer.Upload(device.Get(),pose);
            const auto reflected=Pixels(device.Get(),context.Get(),renderer.Render(context.Get(),600,800,.35f,.05f,1));
            unsigned imageChanges=0;for(size_t i=0;i<wrapped.size();++i)imageChanges+=wrapped[i]!=reflected[i];
            Check(imageChanges>100,"Cube finish did not change the rendered gun");
            preview_scene::ApplyImage(imageWeapon,{});for(const auto &part:imageWeapon.parts)Check(!part.image.texture,"Image toggle retained cube finish");
            std::cout<<"PASS: wrapped and cube gun images ("<<imageChanges<<" changed pixels), agent isolation and reset\n";
            Check(!preview_scene::ApplyFinish(skinWeapon, {}), "Stock reset matched a finish");
            for (const auto &part : skinWeapon.parts) Check(!part.liveTexture, "Stock reset retained the old skin");
            auto legacy = preview_scene::Load(argv[2], "", false, {}, true);
            Check(legacy.parts.front().material != material, "Legacy skin selected the HD weapon mesh");
            std::cout << "PASS: finish swaps (" << differences << " changed pixels), material matching, stock reset and legacy mesh\n";
        }
        std::cout << "PASS: real agent + weapon GLB, textures, pose, WARP rendering, state preservation and resize\n";
        CoUninitialize();
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
