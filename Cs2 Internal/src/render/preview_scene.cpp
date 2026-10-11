#include "platform/windows.h"
#include "render/preview_scene.h"
#include "core/runtime_paths.h"
#include "nlohmann/json.hpp"
#include <d3dcompiler.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>

#pragma comment(lib, "d3dcompiler.lib")
namespace preview_scene
{
    using namespace DirectX;
    using Microsoft::WRL::ComPtr;
    using Json = nlohmann::json;
    namespace fs = std::filesystem;
    namespace
    {
        void Require(bool ok, const char *text)
        {
            if (!ok)
                throw std::runtime_error(text);
        }
        void Hr(HRESULT result)
        {
            Require(SUCCEEDED(result), "Could not create the 3D preview graphics resources.");
        }
        struct Glb
        {
            Json json;
            std::vector<unsigned char> bin;
            explicit Glb(const fs::path &file)
            {
                std::ifstream in(runtime_paths::NativePath(file), std::ios::binary);
                std::uint32_t h[5]{};
                in.read(reinterpret_cast<char *>(h), sizeof(h));
                Require(in.good() && h[0] == 0x46546c67 && h[1] == 2 && h[2] <= 256 * 1024 * 1024 &&
                            h[3] <= 32 * 1024 * 1024 && h[4] == 0x4e4f534a,
                        "Invalid preview GLB header.");
                std::string text(h[3], '\0');
                in.read(text.data(), text.size());
                Require(in.good(), "Truncated preview JSON.");
                json = Json::parse(text);
                std::uint32_t chunk[2]{};
                in.read(reinterpret_cast<char *>(chunk), 8);
                Require(in.good() && chunk[1] == 0x004e4942 && chunk[0] <= 256 * 1024 * 1024 &&
                            28ull + h[3] + chunk[0] == h[2],
                        "Invalid preview mesh data.");
                bin.resize(chunk[0]);
                in.read(reinterpret_cast<char *>(bin.data()), bin.size());
                Require(in.good(), "Truncated preview mesh data.");
            }
            std::vector<std::array<float, 16>> Access(int index)
            {
                const auto &a = json.at("accessors").at(index);
                Require(!a.contains("sparse"), "Sparse preview data is unsupported.");
                const auto &v = json.at("bufferViews").at(a.at("bufferView").get<int>());
                Require(v.value("buffer", 0) == 0, "External preview buffers are unsupported.");
                const std::string type = a.at("type");
                const unsigned n = type == "SCALAR" ? 1
                                   : type == "VEC2" ? 2
                                   : type == "VEC3" ? 3
                                   : type == "VEC4" ? 4
                                   : type == "MAT4" ? 16
                                                    : 0;
                const int component = a.at("componentType");
                const unsigned size = component == 5126 || component == 5125 ? 4
                                      : component == 5123                    ? 2
                                      : component == 5121                    ? 1
                                                                             : 0;
                const size_t count = a.at("count"),
                             offset = a.value("byteOffset", size_t{}) + v.value("byteOffset", size_t{});
                const size_t stride = v.value("byteStride", size_t(n * size));
                const size_t viewStart = v.value("byteOffset", size_t{}), viewSize = v.at("byteLength");
                Require(n && size && count <= 1000000 && stride >= n * size && offset >= viewStart &&
                            viewStart <= bin.size() && viewSize <= bin.size() - viewStart &&
                            offset <= viewStart + viewSize &&
                            (!count || ((count - 1) * stride + n * size <= viewStart + viewSize - offset)),
                        "Preview accessor outside buffer.");
                std::vector<std::array<float, 16>> result(count);
                for (size_t i = 0; i < count; ++i)
                    for (unsigned c = 0; c < n; ++c)
                    {
                        const auto *p = bin.data() + offset + i * stride + c * size;
                        float value = 0;
                        if (component == 5126)
                            std::memcpy(&value, p, 4);
                        else
                        {
                            unsigned u = 0;
                            std::memcpy(&u, p, size);
                            value = static_cast<float>(u);
                            if (a.value("normalized", false))
                                value /= size == 1 ? 255.f : 65535.f;
                        }
                        Require(std::isfinite(value), "Non-finite preview vertex.");
                        result[i][c] = value;
                    }
                return result;
            }
        };
        XMMATRIX Local(const Json &node)
        {
            if (node.contains("matrix"))
            {
                XMFLOAT4X4 m{};
                for (unsigned i = 0; i < 16; ++i)
                    (&m._11)[i] = node["matrix"].at(i).get<float>();
                return XMLoadFloat4x4(&m);
            }
            const auto s = node.value("scale", std::vector<float>{1, 1, 1});
            const auto r = node.value("rotation", std::vector<float>{0, 0, 0, 1});
            const auto t = node.value("translation", std::vector<float>{0, 0, 0});
            Require(s.size() == 3 && r.size() == 4 && t.size() == 3, "Invalid node transform.");
            return XMMatrixScaling(s[0], s[1], s[2]) * XMMatrixRotationQuaternion(XMVectorSet(r[0], r[1], r[2], r[3])) *
                   XMMatrixTranslation(t[0], t[1], t[2]);
        }
        std::string BoneName(std::string name)
        {
            for (char &c : name)
                if (c >= 'A' && c <= 'Z')
                    c += char('a' - 'A');
            return name;
        }
        using PoseTransforms = std::unordered_map<std::string, XMFLOAT4X4>;
        std::vector<XMFLOAT4X4> WorldTransforms(const Json &nodes, const PoseTransforms &overrides = {})
        {
            std::vector<int> parents(nodes.size(), -1), state(nodes.size(), 0);
            std::vector<XMFLOAT4X4> world(nodes.size());
            for (size_t i = 0; i < nodes.size(); ++i)
                if (nodes[i].contains("children"))
                    for (int child : nodes[i]["children"])
                    {
                        Require(child >= 0 && size_t(child) < nodes.size() && parents[child] == -1,
                                "Invalid preview hierarchy.");
                        parents[child] = static_cast<int>(i);
                    }
            std::function<XMMATRIX(int)> transform = [&](int n) -> XMMATRIX {
                Require(state.at(n) != 1, "Cyclic preview hierarchy.");
                if (state[n] == 2)
                    return XMLoadFloat4x4(&world[n]);
                state[n] = 1;
                XMMATRIX m = Local(nodes[n]);
                if (parents[n] >= 0)
                    m = m * transform(parents[n]);
                const auto pose = overrides.find(BoneName(nodes[n].value("name", std::string{})));
                if (pose != overrides.end())
                    m = XMLoadFloat4x4(&pose->second);
                XMStoreFloat4x4(&world[n], m);
                state[n] = 2;
                return m;
            };
            for (size_t i = 0; i < nodes.size(); ++i)
                transform(static_cast<int>(i));
            return world;
        }
        ComPtr<ID3D11ShaderResourceView> Texture(ID3D11Device *device, const fs::path &path, bool srgb = true)
        {
            ComPtr<ID3D11Texture2D> texture;
            UINT width = 1, height = 1;
            std::vector<unsigned char> rgba(4, 255);
            if (!path.empty())
            {
                const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                struct ComEnd
                {
                    HRESULT hr;
                    ~ComEnd()
                    {
                        if (SUCCEEDED(hr))
                            CoUninitialize();
                    }
                } end{initialized};
                ComPtr<IWICImagingFactory> factory;
                ComPtr<IWICBitmapDecoder> decoder;
                ComPtr<IWICBitmapFrameDecode> frame;
                ComPtr<IWICBitmapScaler> scaler;
                ComPtr<IWICFormatConverter> converter, opaque;
                Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
                const auto file = runtime_paths::NativePath(path);
                Hr(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &decoder));
                Hr(decoder->GetFrame(0, &frame));
                Hr(frame->GetSize(&width, &height));
                Require(width && height && width <= 16384 && height <= 16384, "Invalid preview texture dimensions.");
                const float ratio = std::min(1.f, 1024.f / static_cast<float>(std::max(width, height)));
                width = std::max(1u, static_cast<UINT>(width * ratio));
                height = std::max(1u, static_cast<UINT>(height * ratio));
                // Source weapon color alpha contains shader data, not transparency.
                // Strip it before filtering so WIC cannot premultiply RGB to black.
                Hr(factory->CreateFormatConverter(&opaque));
                Hr(opaque->Initialize(frame.Get(), GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone, nullptr, 0,
                                      WICBitmapPaletteTypeCustom));
                Hr(factory->CreateBitmapScaler(&scaler));
                Hr(scaler->Initialize(opaque.Get(), width, height, WICBitmapInterpolationModeFant));
                Hr(factory->CreateFormatConverter(&converter));
                Hr(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                         0, WICBitmapPaletteTypeCustom));
                rgba.resize(size_t(width) * height * 4);
                Hr(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(rgba.size()), rgba.data()));
            }
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{rgba.data(), width * 4, 0};
            Hr(device->CreateTexture2D(&desc, &data, &texture));
            ComPtr<ID3D11ShaderResourceView> view;
            Hr(device->CreateShaderResourceView(texture.Get(), nullptr, &view));
            return view;
        }
    } // namespace
    Model Load(const fs::path &file, const std::string &pose, bool agent, const fs::path &poseTemplate, bool legacyWeapon)
    {
        Glb glb(file);
        auto &j = glb.json;
        auto nodes = j.at("nodes");
        Require(nodes.size() <= 8192, "Too many preview nodes.");
        Model model;
        const bool hasHd = std::any_of(nodes.begin(), nodes.end(), [](const auto &node) {
            return node.value("name", std::string{}).find("body_hd") != std::string::npos;
        });
        if (agent && j.contains("animations"))
            for (const auto &animation : j["animations"])
            {
                if (!animation.value("name", std::string{}).ends_with(pose))
                    continue;
                for (const auto &channel : animation.at("channels"))
                {
                    const auto &sampler = animation.at("samplers").at(channel.at("sampler").get<int>());
                    const auto values = glb.Access(sampler.at("output"));
                    if (values.empty())
                        continue;
                    const auto &target = channel.at("target");
                    const std::string path = target.at("path");
                    if (path != "rotation" && path != "translation" && path != "scale")
                        continue;
                    const size_t frame = sampler.value("interpolation", std::string{}) == "CUBICSPLINE" ? 1 : 0;
                    Require(frame < values.size(), "Invalid preview animation.");
                    auto &node = nodes.at(target.at("node").get<int>());
                    node.erase("matrix");
                    node[path] = Json::array();
                    for (unsigned n = 0; n < (path == "rotation" ? 4u : 3u); ++n)
                        node[path].push_back(values[frame][n]);
                }
                model.posed = true;
                break;
            }
        PoseTransforms fallbackPose;
        if (agent && !model.posed && !poseTemplate.empty())
        {
            Glb donor(poseTemplate);
            auto donorNodes = donor.json.at("nodes");
            if (donor.json.contains("animations"))
                for (const auto &animation : donor.json["animations"])
                {
                    if (!animation.value("name", std::string{}).ends_with(pose))
                        continue;
                    for (const auto &channel : animation.at("channels"))
                    {
                        const auto &target = channel.at("target");
                        const auto path = target.at("path").get<std::string>();
                        if (path != "rotation" && path != "translation" && path != "scale")
                            continue;
                        auto &node = donorNodes.at(target.at("node").get<int>());
                        const auto &sampler = animation.at("samplers").at(channel.at("sampler").get<int>());
                        const auto values = donor.Access(sampler.at("output"));
                        const size_t frame = sampler.value("interpolation", std::string{}) == "CUBICSPLINE" ? 1 : 0;
                        Require(frame < values.size(), "Invalid template animation.");
                        node.erase("matrix");
                        node[path] = Json::array();
                        for (unsigned n = 0; n < (path == "rotation" ? 4u : 3u); ++n)
                            node[path].push_back(values[frame][n]);
                    }
                    const auto donorWorld = WorldTransforms(donorNodes);
                    unsigned matched = 0;
                    for (size_t i = 0; i < donorNodes.size(); ++i)
                    {
                        const auto name = BoneName(donorNodes[i].value("name", std::string{}));
                        if (name.empty() || donorNodes[i].contains("mesh"))
                            continue;
                        if (std::any_of(nodes.begin(), nodes.end(), [&](const auto &n) {
                                return BoneName(n.value("name", std::string{})) == name;
                            }))
                        {
                            fallbackPose.emplace(name, donorWorld[i]);
                            ++matched;
                        }
                    }
                    // Transfer model-space transforms: some custom rigs put pelvis
                    // beside root_motion instead of below it.
                    model.posed = matched >= 10;
                    if (!model.posed)
                        fallbackPose.clear();
                    break;
                }
        }
        const auto world = WorldTransforms(nodes, fallbackPose);
        for (size_t nodeId = 0; nodeId < nodes.size(); ++nodeId)
        {
            const auto &node = nodes[nodeId];
            const std::string name = node.value("name", std::string{});
            if (name == "hand_R" || name == "hand_r")
            {
                model.hand = world[nodeId];
                model.hasHand = true;
            }
            if (name == "hand_L" || name == "hand_l")
            {
                model.supportHand = {world[nodeId]._41, world[nodeId]._42, world[nodeId]._43};
                model.hasSupportHand = true;
            }
            if (!node.contains("mesh") || (agent && name.find("firstperson") != std::string::npos) ||
                (hasHd && name.find(!agent && legacyWeapon ? "body_hd" : "body_legacy") != std::string::npos))
                continue;
            std::vector<XMFLOAT4X4> joints;
            if (node.contains("skin"))
            {
                const auto &skin = j.at("skins").at(node["skin"].get<int>());
                const auto inverse = glb.Access(skin.at("inverseBindMatrices"));
                const auto &ids = skin.at("joints");
                Require(ids.size() == inverse.size(), "Invalid joint palette.");
                for (size_t i = 0; i < ids.size(); ++i)
                {
                    XMFLOAT4X4 matrix{};
                    std::memcpy(&matrix, inverse[i].data(), 64);
                    XMFLOAT4X4 result{};
                    XMStoreFloat4x4(&result, XMLoadFloat4x4(&matrix) * XMLoadFloat4x4(&world.at(ids[i].get<int>())));
                    joints.push_back(result);
                }
            }
            for (const auto &primitive : j.at("meshes").at(node["mesh"].get<int>()).at("primitives"))
            {
                if (primitive.value("mode", 4) != 4)
                    continue;
                const auto &a = primitive.at("attributes");
                const auto positions = glb.Access(a.at("POSITION"));
                const auto normals = a.contains("NORMAL") ? glb.Access(a["NORMAL"])
                                                          : std::vector<std::array<float, 16>>(positions.size());
                const auto uv = a.contains("TEXCOORD_0") ? glb.Access(a["TEXCOORD_0"])
                                                         : std::vector<std::array<float, 16>>(positions.size());
                const auto weights =
                    a.contains("WEIGHTS_0") ? glb.Access(a["WEIGHTS_0"]) : std::vector<std::array<float, 16>>{};
                const auto boneIds =
                    a.contains("JOINTS_0") ? glb.Access(a["JOINTS_0"]) : std::vector<std::array<float, 16>>{};
                Require(
                    normals.size() == positions.size() && uv.size() == positions.size() &&
                        (weights.empty() || (weights.size() == positions.size() && boneIds.size() == positions.size())),
                    "Mismatched preview attributes.");
                Part part;
                if (primitive.contains("material"))
                {
                    const auto &material = j.at("materials").at(primitive["material"].get<int>());
                    part.doubleSided = material.value("doubleSided", false);
                    const auto pbr = material.value("pbrMetallicRoughness", Json::object());
                    if (pbr.contains("baseColorFactor"))
                        for (unsigned i = 0; i < 4; ++i)
                            (&part.color.x)[i] = pbr["baseColorFactor"].at(i).get<float>();
                    // Source 2 tints can be masked; a global GLTF color multiplier
                    // alone makes these custom characters completely black.
                    const auto vmat = material.value("extras", Json::object()).value("vmat", Json::object());
                    part.material = vmat.value("Name", std::string{});
                    if (vmat.value("IntParams", Json::object()).value("F_TINT_MASK", 0) != 0)
                    {
                        const auto mask =
                            vmat.value("TextureParams", Json::object()).value("g_tTintMask", std::string{});
                        auto filename = fs::path(std::u8string(mask.begin(), mask.end())).filename();
                        filename.replace_extension(L".png");
                        if (!mask.empty() && j.contains("images"))
                            for (const auto &image : j["images"])
                            {
                                const auto uri = image.value("uri", std::string{});
                                const auto relative = fs::path(std::u8string(uri.begin(), uri.end()));
                                if (relative.filename() != filename)
                                    continue;
                                Require(!relative.has_root_path(), "Unsafe tint mask path.");
                                for (const auto &component : relative)
                                    Require(component != L".." && component.native().find(L':') == std::wstring::npos,
                                            "Unsafe tint mask path.");
                                part.tintMask = file.parent_path() / relative;
                                break;
                            }
                    }
                    if (pbr.contains("baseColorTexture"))
                    {
                        const auto &texture = j.at("textures").at(pbr["baseColorTexture"]["index"].get<int>());
                        const auto &image = j.at("images").at(texture["source"].get<int>());
                        if (image.contains("uri"))
                        {
                            const auto uri = image["uri"].get<std::string>();
                            const fs::path relative = fs::path(std::u8string(uri.begin(), uri.end()));
                            Require(!relative.empty() && !relative.has_root_path(), "Unsafe preview texture path.");
                            for (const auto &component : relative)
                                Require(component != L".." && component.native().find(L':') == std::wstring::npos,
                                        "Unsafe preview texture path.");
                            part.texture = file.parent_path() / relative;
                        }
                    }
                }
                for (size_t i = 0; i < positions.size(); ++i)
                {
                    const auto &p = positions[i];
                    const auto &n = normals[i];
                    XMMATRIX m = XMLoadFloat4x4(&world[nodeId]);
                    if (!joints.empty() && !weights.empty())
                    {
                        m = XMMatrixScaling(0, 0, 0);
                        m.r[3] = XMVectorZero();
                        for (unsigned b = 0; b < 4; ++b)
                            if (weights[i][b] > 0)
                            {
                                const auto matrix = XMLoadFloat4x4(&joints.at(static_cast<size_t>(boneIds[i][b])));
                                for (unsigned row = 0; row < 4; ++row)
                                    m.r[row] = XMVectorAdd(m.r[row], XMVectorScale(matrix.r[row], weights[i][b]));
                            }
                    }
                    Vertex v{};
                    XMStoreFloat3(&v.position, XMVector3TransformCoord(XMVectorSet(p[0], p[1], p[2], 1), m));
                    XMStoreFloat3(&v.normal,
                                  XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(n[0], n[1], n[2], 0), m)));
                    v.uv = {uv[i][0], uv[i][1]};
                    part.vertices.push_back(v);
                }
                const auto indices = glb.Access(primitive.at("indices"));
                Require(indices.size() % 3 == 0, "Invalid triangle indices.");
                for (const auto &i : indices)
                {
                    Require(i[0] >= 0 && i[0] < positions.size(), "Preview vertex index outside mesh.");
                    part.indices.push_back(static_cast<unsigned>(i[0]));
                }
                model.parts.push_back(std::move(part));
            }
        }
        Require(!model.parts.empty(), "Model contains no preview meshes.");
        return model;
    }
    unsigned ApplyFinish(Model &weapon, const std::vector<MaterialTexture> &textures)
    {
        const auto normalize = [](std::string name) {
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                return c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? char(c + 'a' - 'A') : char(c);
            });
            if (name.ends_with(".vmat_c")) name.resize(name.size() - 2);
            return name;
        };
        unsigned matched = 0;
        for (auto &part : weapon.parts)
        {
            part.liveTexture.Reset();
            if (part.material.empty()) continue;
            const auto name = normalize(part.material);
            for (const auto &texture : textures)
                if (texture.texture && normalize(texture.material) == name)
                {
                    part.liveTexture = texture.texture;
                    ++matched;
                    break;
                }
        }
        return matched;
    }
    void ApplyImage(Model &weapon,const ImageFinish &image){for(auto &part:weapon.parts)part.image=image;}
    void Attach(Model &agent, Model weapon)
    {
        Require(agent.hasHand && agent.posed, "Agent has no compatible weapon holding pose.");
        const auto grip = XMVectorSet(agent.hand._41, agent.hand._42, agent.hand._43, 1);
        auto forward =
            agent.hasSupportHand ? XMVectorSubtract(XMLoadFloat3(&agent.supportHand), grip) : XMVectorSet(0, 0, 1, 0);
        if (XMVectorGetX(XMVector3Length(forward)) < .08f)
            forward = XMVectorSet(0, 0, 1, 0);
        forward = XMVectorSetW(XMVector3Normalize(forward), 0);
        auto right = XMVector3Cross(XMVectorSet(0, 1, 0, 0), forward);
        if (XMVectorGetX(XMVector3Length(right)) < .01f)
            right = XMVectorSet(1, 0, 0, 0);
        right = XMVector3Normalize(right);
        const auto up = XMVector3Normalize(XMVector3Cross(forward, right));
        const XMMATRIX m(right, up, forward, grip);
        for (auto &part : weapon.parts)
        {
            for (auto &vertex : part.vertices)
            {
                XMStoreFloat3(&vertex.position, XMVector3TransformCoord(XMLoadFloat3(&vertex.position), m));
                XMStoreFloat3(&vertex.normal,
                              XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&vertex.normal), m)));
            }
            agent.parts.push_back(std::move(part));
        }
    }
    void Renderer::Reset()
    {
        *this = Renderer{};
    }
    void Renderer::Upload(ID3D11Device *device, const Model &model)
    {
        Reset();
        device_ = device;
        Hr(device->CreateDeferredContext(0, &deferred_));
        constexpr char shader[] = R"(
cbuffer Scene : register(b0) { row_major float4x4 vp; float4 color; float4 imageParams; float4 eye; };
struct Input { float3 p:POSITION; float3 n:NORMAL; float2 uv:TEXCOORD0; };
struct Output { float4 p:SV_POSITION; float3 n:NORMAL; float2 uv:TEXCOORD0; float3 world:TEXCOORD1; };
Output VS(Input i) { Output o; o.p=mul(float4(i.p,1),vp); o.n=i.n; o.uv=i.uv; o.world=i.p; return o; }
TextureCube environment:register(t2); Texture2D image:register(t0); Texture2D tintMask:register(t1); SamplerState sampleState:register(s0);
float4 PS(Output i):SV_TARGET { float3 tint=lerp(float3(1,1,1),color.rgb,tintMask.Sample(sampleState,i.uv).r); float3 t=image.Sample(sampleState,i.uv).rgb*tint;
if(imageParams.x>1.5) t=environment.Sample(sampleState,reflect(normalize(i.world-eye.xyz),normalize(i.n))).rgb*imageParams.yzw;
else if(imageParams.x>.5) t=image.Sample(sampleState,i.uv).rgb*imageParams.yzw;
float3 n=normalize(i.n); float3 v=normalize(eye.xyz-i.world);
float light=.52+.48*saturate(dot(n,normalize(float3(-.5,.8,.6))))+.14*saturate(dot(n,normalize(float3(.7,.25,-.6))));
float rim=.035*pow(1-saturate(dot(n,v)),3); if(imageParams.x>.5){light=1;rim=0;} return float4(pow(max(t*light+rim,0),1/2.2),1); }
)";
        ComPtr<ID3DBlob> vs, ps, errors;
        Hr(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "VS", "vs_4_0", 0, 0, &vs, &errors));
        Hr(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "PS", "ps_4_0", 0, 0, &ps, &errors));
        Hr(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_));
        Hr(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_));
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        Hr(device->CreateInputLayout(elements, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &layout_));
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = 112;
        cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Hr(device->CreateBuffer(&cb, nullptr, &constants_));
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        Hr(device->CreateSamplerState(&sampler, &sampler_));
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_BACK;
        raster.FrontCounterClockwise = TRUE;
        raster.DepthClipEnable = TRUE;
        Hr(device->CreateRasterizerState(&raster, &raster_));
        raster.CullMode = D3D11_CULL_NONE;
        Hr(device->CreateRasterizerState(&raster, &doubleSidedRaster_));
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = TRUE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc = D3D11_COMPARISON_LESS;
        Hr(device->CreateDepthStencilState(&ds, &depthState_));
        XMFLOAT3 low{FLT_MAX, FLT_MAX, FLT_MAX}, high{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (const auto &part : model.parts)
        {
            Require(!part.vertices.empty() && !part.indices.empty(), "Empty preview geometry.");
            GpuPart gpu;
            D3D11_BUFFER_DESC desc{};
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.ByteWidth = static_cast<UINT>(part.vertices.size() * sizeof(Vertex));
            desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA data{part.vertices.data(), 0, 0};
            Hr(device->CreateBuffer(&desc, &data, &gpu.vertices));
            desc.ByteWidth = static_cast<UINT>(part.indices.size() * 4);
            desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            data.pSysMem = part.indices.data();
            Hr(device->CreateBuffer(&desc, &data, &gpu.indices));
            if(part.image.texture){
                ComPtr<ID3D11Device> owner;part.image.texture->GetDevice(&owner);
                Require(owner.Get()==device,"Image finish belongs to a different graphics device.");
                gpu.imageParams={part.image.cube?2.f:1.f,part.image.tint[0],part.image.tint[1],part.image.tint[2]};
                if(part.image.cube)gpu.environment=part.image.texture;
                else gpu.texture=part.image.texture;
            }
            if (gpu.texture){}
            else if (part.liveTexture)
            {
                ComPtr<ID3D11Device> owner;
                part.liveTexture->GetDevice(&owner);
                Require(owner.Get() == device, "Weapon skin belongs to a different graphics device.");
                gpu.texture = part.liveTexture;
            }
            else gpu.texture = Texture(device, part.texture);
            gpu.tintMask = Texture(device, part.tintMask, false);
            gpu.count = static_cast<UINT>(part.indices.size());
            gpu.color = part.color;
            gpu.doubleSided = part.doubleSided;
            parts_.push_back(std::move(gpu));
            for (const auto &vertex : part.vertices)
            {
                framingPoints_.push_back(vertex.position);
                low.x = std::min(low.x, vertex.position.x);
                low.y = std::min(low.y, vertex.position.y);
                low.z = std::min(low.z, vertex.position.z);
                high.x = std::max(high.x, vertex.position.x);
                high.y = std::max(high.y, vertex.position.y);
                high.z = std::max(high.z, vertex.position.z);
            }
        }
        center_ = {(low.x + high.x) * .5f, (low.y + high.y) * .5f, (low.z + high.z) * .5f};
        radius_ = std::max({.01f, (high.x - low.x) * .55f, (high.y - low.y) * .55f, (high.z - low.z) * .55f});
    }
    ID3D11ShaderResourceView *Renderer::Render(ID3D11DeviceContext *context, unsigned width, unsigned height, float yaw,
                                               float pitch, float zoom)
    {
        if (!Ready())
            return nullptr;
        width = std::clamp(width, 64u, 1536u);
        height = std::clamp(height, 64u, 1536u);
        if (width != width_ || height != height_)
        {
            target_.Reset();
            view_.Reset();
            depth_.Reset();
            width_ = height_ = 0;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> texture;
            Hr(device_->CreateTexture2D(&desc, nullptr, &texture));
            Hr(device_->CreateRenderTargetView(texture.Get(), nullptr, &target_));
            Hr(device_->CreateShaderResourceView(texture.Get(), nullptr, &view_));
            desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            texture.Reset();
            Hr(device_->CreateTexture2D(&desc, nullptr, &texture));
            Hr(device_->CreateDepthStencilView(texture.Get(), nullptr, &depth_));
            width_ = width;
            height_ = height;
        }
        // Let the dock's translucent background show around the character.
        const float background[] = {0.f, 0.f, 0.f, 0.f};
        deferred_->ClearState();
        deferred_->ClearRenderTargetView(target_.Get(), background);
        deferred_->ClearDepthStencilView(depth_.Get(), D3D11_CLEAR_DEPTH, 1, 0);
        auto *target = target_.Get();
        deferred_->OMSetRenderTargets(1, &target, depth_.Get());
        deferred_->OMSetDepthStencilState(depthState_.Get(), 0);
        D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        deferred_->RSSetViewports(1, &viewport);
        deferred_->RSSetState(raster_.Get());
        deferred_->IASetInputLayout(layout_.Get());
        deferred_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        deferred_->VSSetShader(vs_.Get(), nullptr, 0);
        deferred_->PSSetShader(ps_.Get(), nullptr, 0);
        auto *constants = constants_.Get();
        auto *sampler = sampler_.Get();
        deferred_->VSSetConstantBuffers(0, 1, &constants);
        deferred_->PSSetConstantBuffers(0, 1, &constants);
        deferred_->PSSetSamplers(0, 1, &sampler);
        Require(std::isfinite(yaw)&&std::isfinite(pitch)&&std::isfinite(zoom),"Invalid preview camera.");
        zoom=std::clamp(zoom,.65f,1.65f);
        const float aspect = float(width) / height, tangent=std::tan(.72f*.5f);
        const XMVECTOR direction=XMVectorSet(std::sin(yaw)*std::cos(pitch),std::sin(pitch),std::cos(yaw)*std::cos(pitch),0);
        const XMVECTOR right=XMVectorSet(std::cos(yaw),0,-std::sin(yaw),0);
        const XMVECTOR up=XMVector3Cross(direction,right);
        float fit=radius_;
        // Fit the actual silhouette; box corners add empty space around posed arms.
        for(const auto& point:framingPoints_){
            const auto offset=XMVectorSet(point.x-center_.x,point.y-center_.y,point.z-center_.z,0);
            const float horizontal=std::abs(XMVectorGetX(XMVector3Dot(offset,right)))/(tangent*aspect);
            const float vertical=std::abs(XMVectorGetX(XMVector3Dot(offset,up)))/tangent;
            fit=std::max(fit,XMVectorGetX(XMVector3Dot(offset,direction))+std::max(horizontal,vertical)*1.1f);
        }
        const float distance=fit/zoom;
        const XMVECTOR targetPoint = XMLoadFloat3(&center_);
        const XMVECTOR eye =
            XMVectorAdd(targetPoint,XMVectorScale(direction,distance));
        struct Constants
        {
            XMFLOAT4X4 vp;
            XMFLOAT4 color,imageParams,eye;
        } data{};
        XMStoreFloat4x4(&data.vp, XMMatrixLookAtRH(eye, targetPoint, XMVectorSet(0, 1, 0, 0)) *
                                      XMMatrixPerspectiveFovRH(.72f, aspect, std::max(.001f, radius_ * .005f),
                                                               std::max(100.f, distance + radius_ * 4)));
        for (const auto &part : parts_)
        {
            deferred_->RSSetState(part.doubleSided ? doubleSidedRaster_.Get() : raster_.Get());
            data.color = part.color;
            data.imageParams=part.imageParams;XMStoreFloat4(&data.eye,eye);
            deferred_->UpdateSubresource(constants_.Get(), 0, nullptr, &data, 0, 0);
            UINT stride = sizeof(Vertex), offset = 0;
            auto *vertices = part.vertices.Get();
            ID3D11ShaderResourceView *textures[] = {part.texture.Get(), part.tintMask.Get(),part.environment.Get()};
            deferred_->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
            deferred_->IASetIndexBuffer(part.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
            deferred_->PSSetShaderResources(0, 3, textures);
            deferred_->DrawIndexed(part.count, 0, 0);
        }
        ComPtr<ID3D11CommandList> commands;
        Hr(deferred_->FinishCommandList(FALSE, &commands));
        context->ExecuteCommandList(commands.Get(), TRUE);
        return view_.Get();
    }
} // namespace preview_scene
