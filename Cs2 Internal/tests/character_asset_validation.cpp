#include "platform/windows.h"
#include "render/preview_scene.h"
#include "core/runtime_paths.h"
#include "game/character_catalog.h"
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
void Require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void Hr(HRESULT value) { Require(SUCCEEDED(value), "Native graphics / WIC operation failed"); }
void WritePng(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11ShaderResourceView *view,
              const fs::path &file) {
    Require(view != nullptr, "Renderer returned no texture");
    ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture; Hr(resource.As(&texture));
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    Require(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "Unexpected render target format");
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; Hr(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{}; Hr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    std::vector<unsigned char> pixels(size_t(desc.Width) * desc.Height * 4);
    for (UINT row = 0; row < desc.Height; ++row)
        std::memcpy(pixels.data() + size_t(row) * desc.Width * 4,
                    static_cast<const unsigned char *>(mapped.pData) + size_t(row) * mapped.RowPitch,
                    size_t(desc.Width) * 4);
    context->Unmap(staging.Get(), 0);
    unsigned visible = 0;
    for (size_t i = 0; i < pixels.size(); i += 4)
        if (pixels[i] != pixels[0] || pixels[i + 1] != pixels[1] || pixels[i + 2] != pixels[2]) ++visible;
    Require(visible > size_t(desc.Width) * desc.Height / 100, "Rendered pose is empty");
    fs::create_directories(file.parent_path());
    ComPtr<IWICImagingFactory> factory;
    Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream; Hr(factory->CreateStream(&stream));
    Hr(stream->InitializeFromFilename(runtime_paths::NativePath(file).c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder; Hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    Hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame; Hr(encoder->CreateNewFrame(&frame, nullptr));
    Hr(frame->Initialize(nullptr)); Hr(frame->SetSize(desc.Width, desc.Height));
    auto format = GUID_WICPixelFormat32bppRGBA; Hr(frame->SetPixelFormat(&format));
    ComPtr<IWICBitmap> bitmap;
    Hr(factory->CreateBitmapFromMemory(desc.Width, desc.Height, GUID_WICPixelFormat32bppRGBA,
                                      desc.Width * 4, static_cast<UINT>(pixels.size()), pixels.data(), &bitmap));
    ComPtr<IWICFormatConverter> encodePixels; Hr(factory->CreateFormatConverter(&encodePixels));
    Hr(encodePixels->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    Hr(frame->WriteSource(encodePixels.Get(), nullptr));
    Hr(frame->Commit()); Hr(encoder->Commit()); stream.Reset(); encoder.Reset(); frame.Reset();
    ComPtr<IWICBitmapDecoder> decoder;
    Hr(factory->CreateDecoderFromFilename(runtime_paths::NativePath(file).c_str(), nullptr, GENERIC_READ,
                                          WICDecodeMetadataCacheOnLoad, &decoder));
    ComPtr<IWICBitmapFrameDecode> decoded; Hr(decoder->GetFrame(0, &decoded));
    UINT width = 0, height = 0; Hr(decoded->GetSize(&width, &height));
    Require(width == desc.Width && height == desc.Height, "Generated thumbnail failed PNG readback");
    ComPtr<IWICFormatConverter> decodePixels; Hr(factory->CreateFormatConverter(&decodePixels));
    Hr(decodePixels->Initialize(decoded.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                               WICBitmapPaletteTypeCustom));
    std::vector<unsigned char> readback(pixels.size());
    Hr(decodePixels->CopyPixels(nullptr, width * 4, static_cast<UINT>(readback.size()), readback.data()));
    Require(readback == pixels, "WIC thumbnail pixels differ from native render");
}
}

// Use the production GLB loader, donor-pose transfer, GPU upload, renderer and native WIC.
// No model/texture/index input is changed. An optional fourth path publishes the rifle PNG.
int wmain(int argc, wchar_t **argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--discover") {
        constexpr char resource[] = "models/custom/agents/silver_casual/silver_casual.vmdl";
        try {
            Require(SetEnvironmentVariableW(L"CS2_GAME_DIR", argv[2]) != FALSE, "Could not select game directory");
            Require(character_catalog::IsAgentPath(resource) && character_catalog::Installed(resource),
                    "Custom character is not installed at its canonical resource path");
            const auto agents = character_catalog::Discover();
            const auto matches = std::count_if(agents.begin(), agents.end(), [&](const auto &agent) {
                return agent.model == resource && agent.name == "silver casual";
            });
            Require(matches == 1, "Local/enemy character catalog did not discover one Silver Casual entry");
            std::cout << "PASS: installed Silver Casual appears in the shared local/enemy character catalog\n";
            return 0;
        } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
    }
    if (argc != 4 && argc != 5) {
        std::cerr << "Usage: character_asset_validation.exe model.glb donor.glb output-folder [thumbnail.png]\n";
        std::cerr << "       character_asset_validation.exe --discover game/csgo\n";
        return 2;
    }
    try {
        Hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        const fs::path modelPath = argv[1], donorPath = argv[2], output = argv[3];
        Require(fs::is_regular_file(modelPath) && fs::is_regular_file(donorPath), "Missing model or donor GLB");
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                            &device, nullptr, &context));
        constexpr std::array<const char *, 3> poses{"idle_rifle", "idle_pistol", "idle_knife"};
        constexpr std::array<const char *, 3> weapons{"weapons_models_ak47_weapon_rif_ak47_vmdl.glb",
            "weapons_models_glock18_weapon_pist_glock18_vmdl.glb",
            "weapons_models_knife_knife_karambit_weapon_knife_karambit_vmdl.glb"};
        constexpr std::array<float, 3> yaw{0.f, 1.57079633f, 3.14159265f};
        constexpr std::array<const char *, 3> angles{"front", "side", "back"};
        std::array<DirectX::XMFLOAT3, 3> hands{};
        for (size_t p = 0; p < poses.size(); ++p) {
            auto model = preview_scene::Load(modelPath, poses[p], true, donorPath);
            Require(model.posed && model.hasHand && model.hasSupportHand, "Missing transferred holding pose / hands");
            hands[p] = {model.hand._41, model.hand._42, model.hand._43};
            std::cout << poses[p] << " grip=(" << hands[p].x << ',' << hands[p].y << ',' << hands[p].z
                      << ") support=(" << model.supportHand.x << ',' << model.supportHand.y << ',' << model.supportHand.z << ")\n";
            size_t vertices = 0, triangles = 0, textured = 0;
            DirectX::XMFLOAT3 low{FLT_MAX, FLT_MAX, FLT_MAX}, high{-FLT_MAX, -FLT_MAX, -FLT_MAX};
            for (const auto &part : model.parts) {
                Require(!part.vertices.empty() && !part.indices.empty() && part.indices.size() % 3 == 0,
                        "Empty or malformed triangle mesh");
                vertices += part.vertices.size(); triangles += part.indices.size() / 3;
                if (!part.texture.empty()) {
                    Require(fs::is_regular_file(part.texture), "Missing external preview texture"); ++textured;
                }
                for (const auto &v : part.vertices) {
                    const std::array<float, 8> values{v.position.x,v.position.y,v.position.z,
                        v.normal.x,v.normal.y,v.normal.z,v.uv.x,v.uv.y};
                    for (float value : values) Require(std::isfinite(value), "Non-finite skinned vertex");
                    low.x=std::min(low.x,v.position.x); low.y=std::min(low.y,v.position.y); low.z=std::min(low.z,v.position.z);
                    high.x=std::max(high.x,v.position.x); high.y=std::max(high.y,v.position.y); high.z=std::max(high.z,v.position.z);
                }
                for (unsigned index : part.indices) Require(index < part.vertices.size(), "Index outside skinned mesh");
            }
            Require(vertices > 100 && high.y - low.y > .1f, "Degenerate skinned character");
            if (argc == 5) Require(textured == model.parts.size(), "Published thumbnail requires all material textures");
            preview_scene::Renderer renderer; renderer.Upload(device.Get(), model);
            Require(renderer.Ready(), "GPU upload failed");
            for (size_t a = 0; a < yaw.size(); ++a) {
                const auto file = output / (std::string(poses[p]) + "_" + angles[a] + ".png");
                WritePng(device.Get(), context.Get(), renderer.Render(context.Get(), 512, 640, yaw[a], .02f, 1.1f), file);
                if (argc == 5 && p == 0 && a == 0) {
                    const fs::path thumbnail = argv[4];
                    fs::create_directories(thumbnail.parent_path());
                    fs::copy_file(file, thumbnail, fs::copy_options::overwrite_existing);
                }
            }
            const auto weaponPath = donorPath.parent_path() / weapons[p];
            Require(fs::is_regular_file(weaponPath), "Missing weapon model for actual preview attachment check");
            const auto partCount = model.parts.size();
            auto attached = model;
            preview_scene::Attach(attached, preview_scene::Load(weaponPath, "", false));
            Require(attached.parts.size() > partCount, "Weapon attachment added no mesh");
            preview_scene::Renderer weaponRenderer; weaponRenderer.Upload(device.Get(), attached);
            WritePng(device.Get(), context.Get(), weaponRenderer.Render(context.Get(), 512, 640, 0, .02f, 1.1f),
                     output / (std::string(poses[p]) + "_weapon.png"));
            std::cout << poses[p] << ": parts=" << model.parts.size() << " vertices=" << vertices
                      << " triangles=" << triangles << " textured=" << textured
                      << " bounds=(" << low.x << ',' << low.y << ',' << low.z << ")..("
                      << high.x << ',' << high.y << ',' << high.z << ")\n";
        }
        const auto distance=[](auto a, auto b) { return std::abs(a.x-b.x)+std::abs(a.y-b.y)+std::abs(a.z-b.z); };
        Require(distance(hands[0], hands[1]) > .0001f || distance(hands[0], hands[2]) > .0001f,
                "Donor poses did not change the character hands");
        std::cout << "PASS: three donor poses, twelve nonempty WARP renders / WIC PNG readbacks, rifle/pistol/knife attachment\n";
        CoUninitialize(); return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
