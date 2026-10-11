#pragma once
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <filesystem>
#include <string>
#include <vector>
#include "render/preview_material.h"

namespace preview_scene
{
    struct Vertex
    {
        DirectX::XMFLOAT3 position, normal;
        DirectX::XMFLOAT2 uv;
    };
    struct Part
    {
        std::vector<Vertex> vertices;
        std::vector<unsigned> indices;
        std::filesystem::path texture;
        std::filesystem::path tintMask;
        std::string material;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> liveTexture;
        DirectX::XMFLOAT4 color{1, 1, 1, 1};
        ImageFinish image;
        bool doubleSided = false;
    };
    struct Model
    {
        std::vector<Part> parts;
        DirectX::XMFLOAT4X4 hand{};
        DirectX::XMFLOAT3 supportHand{};
        bool hasSupportHand = false;
        bool hasHand = false, posed = false;
    };
    Model Load(const std::filesystem::path &file, const std::string &pose, bool agent,
               const std::filesystem::path &poseTemplate = {}, bool legacyWeapon = false);
    // Apply only exact material matches, before attaching the weapon to an agent.
    unsigned ApplyFinish(Model &weapon, const std::vector<MaterialTexture> &textures);
    void ApplyImage(Model &weapon,const ImageFinish &image);
    void Attach(Model &agent, Model weapon);
    class Renderer
    {
        struct GpuPart
        {
            Microsoft::WRL::ComPtr<ID3D11Buffer> vertices, indices;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> tintMask;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> environment;
            DirectX::XMFLOAT4 imageParams{};
            DirectX::XMFLOAT4 color;
            UINT count = 0;
            bool doubleSided = false;
        };
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> deferred_;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_;
        Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> doubleSidedRaster_;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState_;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view_;
        std::vector<GpuPart> parts_;
        unsigned width_ = 0, height_ = 0;
        DirectX::XMFLOAT3 center_{0, .85f, 0};
        std::vector<DirectX::XMFLOAT3> framingPoints_;
        float radius_ = 1.f;

      public:
        void Reset();
        void Upload(ID3D11Device *device, const Model &model);
        ID3D11ShaderResourceView *Render(ID3D11DeviceContext *context, unsigned width, unsigned height, float yaw,
                                         float pitch, float zoom);
        bool Ready() const
        {
            return !parts_.empty();
        }
    };
} // namespace preview_scene
