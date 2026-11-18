#pragma once
#include <Windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

// Encodes a D3D11 texture to JPEG bytes in memory. Used by FFmpeg recording so
// captured frames never have to be written to the disk first.
namespace JpegCapture
{
	static WICPixelFormatGUID SourcePixelFormat(DXGI_FORMAT format)
	{
		switch (format)
		{
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			return GUID_WICPixelFormat32bppRGBA;
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return GUID_WICPixelFormat32bppBGRA;
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			return GUID_WICPixelFormat32bppBGR;
		default:
			return GUID_WICPixelFormat32bppRGBA;
		}
	}

	static HRESULT Encode(ID3D11DeviceContext* context, ID3D11Texture2D* source, std::vector<uint8_t>& output)
	{
		output.clear();
		if (!context || !source)
			return E_INVALIDARG;

		Microsoft::WRL::ComPtr<ID3D11Device> device;
		context->GetDevice(device.GetAddressOf());
		if (!device)
			return E_NOINTERFACE;

		D3D11_TEXTURE2D_DESC desc{};
		source->GetDesc(&desc);

		// A CPU-readable copy of the texture for WIC.
		D3D11_TEXTURE2D_DESC stagingDesc = desc;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		stagingDesc.BindFlags = 0;
		stagingDesc.MiscFlags = 0;
		stagingDesc.SampleDesc.Count = 1;
		stagingDesc.SampleDesc.Quality = 0;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
		HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf());
		if (FAILED(hr))
			return hr;
		context->CopyResource(staging.Get(), source);

		D3D11_MAPPED_SUBRESOURCE mapped{};
		hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
		if (FAILED(hr))
			return hr;

		Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
		hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(factory.GetAddressOf()));
		if (FAILED(hr))
		{
			context->Unmap(staging.Get(), 0);
			return hr;
		}

		HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, 0);
		if (!memory)
		{
			context->Unmap(staging.Get(), 0);
			return E_OUTOFMEMORY;
		}
		Microsoft::WRL::ComPtr<IStream> stream;
		hr = CreateStreamOnHGlobal(memory, TRUE, stream.GetAddressOf());
		if (FAILED(hr))
		{
			GlobalFree(memory);
			context->Unmap(staging.Get(), 0);
			return hr;
		}

		Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
		hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.GetAddressOf());
		if (SUCCEEDED(hr))
			hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);

		Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
		Microsoft::WRL::ComPtr<IPropertyBag2> properties;
		if (SUCCEEDED(hr))
			hr = encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf());
		if (SUCCEEDED(hr))
			hr = frame->Initialize(properties.Get());
		if (SUCCEEDED(hr))
			hr = frame->SetSize(desc.Width, desc.Height);
		if (SUCCEEDED(hr))
			hr = frame->SetResolution(72, 72);

		// The JPEG encoder wants 24bpp BGR, so convert from the texture's format.
		WICPixelFormatGUID targetFormat = GUID_WICPixelFormat24bppBGR;
		if (SUCCEEDED(hr))
			hr = frame->SetPixelFormat(&targetFormat);

		if (SUCCEEDED(hr))
		{
			const WICPixelFormatGUID sourceFormat = SourcePixelFormat(desc.Format);
			if (memcmp(&sourceFormat, &targetFormat, sizeof(WICPixelFormatGUID)) == 0)
			{
				hr = frame->WritePixels(desc.Height, mapped.RowPitch, mapped.RowPitch * desc.Height,
					static_cast<BYTE*>(mapped.pData));
			}
			else
			{
				Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
				hr = factory->CreateBitmapFromMemory(desc.Width, desc.Height, sourceFormat,
					mapped.RowPitch, mapped.RowPitch * desc.Height,
					static_cast<BYTE*>(mapped.pData), bitmap.GetAddressOf());
				if (SUCCEEDED(hr))
				{
					Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
					hr = factory->CreateFormatConverter(converter.GetAddressOf());
					BOOL canConvert = FALSE;
					if (SUCCEEDED(hr))
						hr = converter->CanConvert(sourceFormat, targetFormat, &canConvert);
					if (SUCCEEDED(hr) && !canConvert)
						hr = E_FAIL;
					if (SUCCEEDED(hr))
						hr = converter->Initialize(bitmap.Get(), targetFormat, WICBitmapDitherTypeNone,
							nullptr, 0.0, WICBitmapPaletteTypeMedianCut);
					if (SUCCEEDED(hr))
					{
						WICRect rect{ 0, 0, static_cast<INT>(desc.Width), static_cast<INT>(desc.Height) };
						hr = frame->WriteSource(converter.Get(), &rect);
					}
				}
			}
		}

		context->Unmap(staging.Get(), 0);
		if (FAILED(hr))
			return hr;

		hr = frame->Commit();
		if (FAILED(hr))
			return hr;
		hr = encoder->Commit();
		if (FAILED(hr))
			return hr;

		const SIZE_T size = GlobalSize(memory);
		void* data = GlobalLock(memory);
		if (data && size > 0)
			output.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
		GlobalUnlock(memory);
		return S_OK;
	}
}
