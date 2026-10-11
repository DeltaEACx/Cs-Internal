#pragma once
#include "platform/windows.h"
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#ifdef small
#undef small
#endif

namespace menu_logo {
inline ID3D11ShaderResourceView* Load(ID3D11Device* device){
    using Microsoft::WRL::ComPtr;
    if(!device)return nullptr;
    HMODULE module{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&Load),&module))return nullptr;
    const auto resource=FindResourceW(module,MAKEINTRESOURCEW(101),RT_RCDATA);
    if(!resource)return nullptr;
    const auto size=SizeofResource(module,resource);
    auto* bytes=static_cast<BYTE*>(LockResource(LoadResource(module,resource)));
    if(!bytes||!size)return nullptr;
    const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    struct ComScope{bool owns;~ComScope(){if(owns)CoUninitialize();}} scope{SUCCEEDED(initialized)};
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converted;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))||
        FAILED(factory->CreateStream(&stream))||FAILED(stream->InitializeFromMemory(bytes,size))||
        FAILED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder))||
        FAILED(decoder->GetFrame(0,&frame))||FAILED(factory->CreateFormatConverter(&converted))||
        FAILED(converted->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return nullptr;
    UINT width{},height{};
    if(FAILED(converted->GetSize(&width,&height))||!width||!height||width>4096||height>4096)return nullptr;
    std::vector<BYTE> rgba(width*height*4);
    if(FAILED(converted->CopyPixels(nullptr,width*4,static_cast<UINT>(rgba.size()),rgba.data())))return nullptr;
    D3D11_TEXTURE2D_DESC description{};
    description.Width=width;description.Height=height;description.MipLevels=description.ArraySize=1;
    description.Format=DXGI_FORMAT_R8G8B8A8_UNORM;description.SampleDesc.Count=1;
    description.Usage=D3D11_USAGE_IMMUTABLE;description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{rgba.data(),width*4,0};ComPtr<ID3D11Texture2D> texture;
    if(FAILED(device->CreateTexture2D(&description,&data,&texture)))return nullptr;
    ID3D11ShaderResourceView* view{};
    return SUCCEEDED(device->CreateShaderResourceView(texture.Get(),nullptr,&view))?view:nullptr;
}
}
