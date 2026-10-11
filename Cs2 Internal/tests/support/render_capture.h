#pragma once
#include "render/preview_scene.h"
#include <wincodec.h>
#include <iostream>
#include <stdexcept>
#include <cmath>

using Microsoft::WRL::ComPtr;
inline void Check(bool ok, const char *text)
{
    if (!ok)
        throw std::runtime_error(text);
}
inline void SavePng(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11ShaderResourceView *view,
                    const wchar_t *file)
{
    ComPtr<ID3D11Resource> source;
    view->GetResource(&source);
    ComPtr<ID3D11Texture2D> texture;
    source.As(&texture);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "Staging texture");
    context->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Readback");
    unsigned changed = 0;
    const auto *data = static_cast<const unsigned char *>(mapped.pData);
    for (unsigned y = 0; y < desc.Height; ++y)
        for (unsigned x = 0; x < desc.Width; ++x)
            if (data[y * mapped.RowPitch + x * 4] > 35)
                ++changed;
    Check(changed > 1000, "Preview is blank");
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))),
          "WIC factory");
    Check(SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(file, GENERIC_WRITE)),
          "PNG output");
    Check(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
              SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)),
          "PNG encoder");
    Check(SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)), "PNG frame");
    frame->SetSize(desc.Width, desc.Height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&format);
    Check(format == GUID_WICPixelFormat32bppBGRA, "PNG format");
    std::vector<BYTE> bgra(size_t(desc.Width) * desc.Height * 4);
    for (unsigned y = 0; y < desc.Height; ++y)
        for (unsigned x = 0; x < desc.Width; ++x)
        {
            const auto *src = data + y * mapped.RowPitch + x * 4;
            auto *dst = bgra.data() + (size_t(y) * desc.Width + x) * 4;
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
        }
    Check(SUCCEEDED(frame->WritePixels(desc.Height, desc.Width * 4, static_cast<UINT>(bgra.size()), bgra.data())),
          "PNG pixels");
    frame->Commit();
    encoder->Commit();
    context->Unmap(staging.Get(), 0);
    std::cout << "Visible pixels: " << changed << '\n';
}
