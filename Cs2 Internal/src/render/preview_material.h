#pragma once
#include <array>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>

namespace preview_scene
{
    struct MaterialTexture
    {
        std::string material;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture;
    };
}

namespace preview_scene {
struct ImageFinish {Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture;bool cube{};std::array<float,3> tint{1,1,1};};
}
