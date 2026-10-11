// Decode a compiled, uncompressed BC6H cubemap through the native D3D11 sampler.
// This test never loads the game or its DLL.
#include "support/render_capture.h"
#include <d3dcompiler.h>
#include <array>
#include <cwchar>
#include <cstring>
#include <fstream>
#include <iterator>

namespace {
template<class T> T Read(const std::vector<unsigned char>& file, size_t offset) {
    Check(offset <= file.size() && sizeof(T) <= file.size()-offset, "Texture header outside file");
    T value{}; std::memcpy(&value,file.data()+offset,sizeof(value)); return value;
}
ComPtr<ID3DBlob> Compile(const char* source,const char* entry,const char* target) {
    ComPtr<ID3DBlob> shader,error;
    auto result=D3DCompile(source,std::strlen(source),"skybox_capture",nullptr,nullptr,entry,target,
                          D3DCOMPILE_ENABLE_STRICTNESS,0,&shader,&error);
    if(FAILED(result) && error) std::cerr << static_cast<const char*>(error->GetBufferPointer());
    Check(SUCCEEDED(result),"Shader compilation"); return shader;
}
constexpr const char* shader=R"(
TextureCube<float4> sky : register(t0);
SamplerState linearSampler : register(s0);
cbuffer Capture : register(b0) {
    float perspective, yaw, pitch, tanHalfFov;
    float aspect; float3 padding;
};
struct Vertex {float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
Vertex VS(uint id:SV_VertexID) {
    Vertex v; v.uv=float2((id<<1)&2,id&2);
    v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1); return v;
}
float4 PS(Vertex v):SV_Target {
    float longitude=(v.uv.x-.5)*6.28318530718+yaw;
    float latitude=(.5-v.uv.y)*3.14159265359;
    // Source 2 cubemaps use Z up, with panorama center facing -X.
    float3 direction=float3(-cos(longitude)*cos(latitude),sin(longitude)*cos(latitude),sin(latitude));
    if(perspective>0) {
        float3 forward=float3(-cos(yaw)*cos(pitch),sin(yaw)*cos(pitch),sin(pitch));
        float3 right=float3(sin(yaw),cos(yaw),0);
        float3 up=float3(cos(yaw)*sin(pitch),-sin(yaw)*sin(pitch),cos(pitch));
        float2 plane=float2(v.uv.x*2-1,1-v.uv.y*2)*tanHalfFov;
        plane.y/=aspect;
        direction=normalize(forward+plane.x*right+plane.y*up);
    }
    float3 rgb=max(sky.SampleLevel(linearSampler,direction,0).rgb,0);
    rgb=lerp(1.055*pow(rgb,1.0/2.4)-.055,12.92*rgb,rgb<=.0031308);
    return float4(saturate(rgb),1);
}
)";
}

int wmain(int argc,wchar_t** argv) try {
    const bool perspective=argc>=4 && std::wcscmp(argv[3],L"--perspective")==0;
    const bool panorama=argc>=4 && std::wcscmp(argv[3],L"--panorama")==0;
    Check(argc==3 || (perspective && argc<=7) || (panorama && argc<=5),
          "Usage: skybox_capture asset.vtex_c preview.png [--perspective [yaw [pitch [horizontal-fov]]]] [--panorama [yaw]]");
    auto argument=[&](int index,float fallback) {
        if(argc<=index) return fallback;
        wchar_t* end{};float value=std::wcstof(argv[index],&end);
        Check(end!=argv[index] && *end==L'\0' && std::isfinite(value),"Invalid camera argument"); return value;
    };
    constexpr float degrees=.017453292519943295f;
    const float yaw=argument(4,0),pitch=argument(5,0),fov=argument(6,90);
    Check(std::abs(pitch)<=89 && fov>=10 && fov<=160,"Camera pitch/FOV out of range");
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    std::ifstream input(std::filesystem::path(argv[1]),std::ios::binary);
    Check(input.good(),"Texture input");
    std::vector<unsigned char> file{std::istreambuf_iterator<char>(input),{}};
    const size_t payload=Read<uint32_t>(file,0),directory=8+Read<uint32_t>(file,8);
    const uint32_t count=Read<uint32_t>(file,12);
    Check(count>0 && count<100,"Texture block count");
    size_t data=0;
    for(uint32_t i=0;i<count;++i) {
        const size_t block=directory+12*i;
        if(Read<uint32_t>(file,block)==0x41544144) data=block+4+Read<uint32_t>(file,block+4);
    }
    Check(data && Read<uint16_t>(file,data)==1,"Texture DATA version");
    Check((Read<uint16_t>(file,data+2)&0x10)!=0,"Texture must be a cubemap");
    const UINT width=Read<uint16_t>(file,data+20),height=Read<uint16_t>(file,data+22);
    Check(width==height && width>=4 && width<=8192,"Cube face dimensions");
    Check(Read<uint16_t>(file,data+24)==1,"Texture depth");
    Check(Read<uint8_t>(file,data+26)==19,"Expected BC6H texture");
    Check(Read<uint8_t>(file,data+27)==1,"This capture expects one uncompressed mip");
    const UINT rowPitch=((width+3)/4)*16,faceBytes=rowPitch*((height+3)/4);
    Check(payload+size_t(faceBytes)*6==file.size(),"Expected six contiguous BC6H faces");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
          D3D11_SDK_VERSION,&device,nullptr,&context)),"WARP device");
    D3D11_TEXTURE2D_DESC cubeDesc{};
    cubeDesc.Width=width;cubeDesc.Height=height;cubeDesc.MipLevels=1;cubeDesc.ArraySize=6;
    cubeDesc.Format=DXGI_FORMAT_BC6H_UF16;cubeDesc.SampleDesc.Count=1;
    cubeDesc.Usage=D3D11_USAGE_IMMUTABLE;cubeDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    cubeDesc.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;
    std::array<D3D11_SUBRESOURCE_DATA,6> faces{};
    for(size_t i=0;i<faces.size();++i) faces[i]={file.data()+payload+i*faceBytes,rowPitch,faceBytes};
    ComPtr<ID3D11Texture2D> cube; ComPtr<ID3D11ShaderResourceView> sky;
    Check(SUCCEEDED(device->CreateTexture2D(&cubeDesc,faces.data(),&cube)),"BC6H cube upload");
    Check(SUCCEEDED(device->CreateShaderResourceView(cube.Get(),nullptr,&sky)),"Cube view");
    auto vertexCode=Compile(shader,"VS","vs_5_0"),pixelCode=Compile(shader,"PS","ps_5_0");
    ComPtr<ID3D11VertexShader> vertexShader;ComPtr<ID3D11PixelShader> pixelShader;
    Check(SUCCEEDED(device->CreateVertexShader(vertexCode->GetBufferPointer(),vertexCode->GetBufferSize(),nullptr,&vertexShader)),"Vertex shader");
    Check(SUCCEEDED(device->CreatePixelShader(pixelCode->GetBufferPointer(),pixelCode->GetBufferSize(),nullptr,&pixelShader)),"Pixel shader");
    const UINT outWidth=perspective?1920:2048,outHeight=perspective?1080:1024;
    D3D11_TEXTURE2D_DESC targetDesc{};targetDesc.Width=outWidth;targetDesc.Height=outHeight;
    targetDesc.MipLevels=targetDesc.ArraySize=1;targetDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    targetDesc.SampleDesc.Count=1;targetDesc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11ShaderResourceView> view;
    Check(SUCCEEDED(device->CreateTexture2D(&targetDesc,nullptr,&texture)),"Capture texture");
    Check(SUCCEEDED(device->CreateRenderTargetView(texture.Get(),nullptr,&target)),"Capture target");
    Check(SUCCEEDED(device->CreateShaderResourceView(texture.Get(),nullptr,&view)),"Capture view");
    D3D11_SAMPLER_DESC samplerDesc{};samplerDesc.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU=samplerDesc.AddressV=samplerDesc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    Check(SUCCEEDED(device->CreateSamplerState(&samplerDesc,&sampler)),"Cube sampler");
    struct Capture {float perspective,yaw,pitch,tanHalfFov,aspect,padding[3];};
    Capture capture{perspective?1.f:0.f,yaw*degrees,pitch*degrees,std::tan(fov*.5f*degrees),float(outWidth)/outHeight,{}};
    D3D11_BUFFER_DESC bufferDesc{};bufferDesc.ByteWidth=sizeof(Capture);
    bufferDesc.Usage=D3D11_USAGE_IMMUTABLE;bufferDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA bufferData{&capture,0,0};ComPtr<ID3D11Buffer> constants;
    Check(SUCCEEDED(device->CreateBuffer(&bufferDesc,&bufferData,&constants)),"Capture camera");
    D3D11_VIEWPORT viewport{0,0,float(outWidth),float(outHeight),0,1};
    context->RSSetViewports(1,&viewport);context->OMSetRenderTargets(1,target.GetAddressOf(),nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertexShader.Get(),nullptr,0);context->PSSetShader(pixelShader.Get(),nullptr,0);
    context->PSSetShaderResources(0,1,sky.GetAddressOf());context->PSSetSamplers(0,1,sampler.GetAddressOf());
    context->PSSetConstantBuffers(0,1,constants.GetAddressOf());
    context->Draw(3,0);
    SavePng(device.Get(),context.Get(),view.Get(),argv[2]);
    std::cout << "PASS: native BC6H cube sampling; " << width << "x" << height << ", 6 faces, 1 mip\n";
    if(perspective) std::cout << "Perspective: " << outWidth << "x" << outHeight << ", yaw " << yaw
                             << ", pitch " << pitch << ", horizontal FOV " << fov << " degrees\n";
    else std::cout << "Panorama: " << outWidth << "x" << outHeight << ", yaw " << yaw << " degrees\n";
    return 0;
} catch(const std::exception& error) { std::cerr << error.what() << '\n';return 1; }
