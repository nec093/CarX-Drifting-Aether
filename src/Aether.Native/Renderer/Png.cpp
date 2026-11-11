#include "Png.h"

#include <wincodec.h>

namespace aether {

bool ConvertToRgba8(DXGI_FORMAT format, const uint8_t* src, uint32_t rowPitch, uint32_t width, uint32_t height, std::vector<uint8_t>& out) {
	enum { kRgba, kBgra, kR10 } kind;
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: kind = kRgba; break;
	case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: kind = kBgra; break;
	case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: kind = kR10; break;
	default: return false;
	}
	out.resize(size_t(width) * height * 4);
	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t* s = src + size_t(y) * rowPitch;
		uint8_t* d = out.data() + size_t(y) * width * 4;
		for (uint32_t x = 0; x < width; ++x, s += 4, d += 4) {
			if (kind == kRgba) {
				d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
			} else if (kind == kBgra) {
				d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
			} else {
				uint32_t v;
				memcpy(&v, s, 4);
				d[0] = uint8_t(((v >> 0) & 0x3FF) >> 2);
				d[1] = uint8_t(((v >> 10) & 0x3FF) >> 2);
				d[2] = uint8_t(((v >> 20) & 0x3FF) >> 2);
			}
			d[3] = 255;
		}
	}
	return true;
}

bool SavePng(const std::wstring& file, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba, std::string& error) {
	HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	bool ok = false;
	{
		ComPtr<IWICImagingFactory> factory;
		ComPtr<IWICStream> stream;
		ComPtr<IWICBitmapEncoder> encoder;
		ComPtr<IWICBitmapFrameEncode> frame;
		WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
		HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
		if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
		if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE);
		if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
		if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
		if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
		if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
		if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
		if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
		if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat32bppRGBA)) {
			// the PNG encoder may only take BGRA: swap in a copy
			if (IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
				std::vector<uint8_t> bgra(rgba);
				for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
					std::swap(bgra[i], bgra[i + 2]);
				}
				hr = frame->WritePixels(height, width * 4, UINT(bgra.size()), bgra.data());
			} else {
				hr = E_FAIL;
			}
		} else if (SUCCEEDED(hr)) {
			hr = frame->WritePixels(height, width * 4, UINT(rgba.size()), const_cast<BYTE*>(rgba.data()));
		}
		if (SUCCEEDED(hr)) hr = frame->Commit();
		if (SUCCEEDED(hr)) hr = encoder->Commit();
		ok = SUCCEEDED(hr);
		if (!ok) {
			error = "PNG encode failed: " + HrToString(hr) + " (" + WideToUtf8(file) + ")";
		}
	}
	if (SUCCEEDED(init)) {
		CoUninitialize();
	}
	return ok;
}

} // namespace aether
