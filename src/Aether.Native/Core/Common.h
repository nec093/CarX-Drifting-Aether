// Aether native runtime: common includes and small helpers.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aether {

using Microsoft::WRL::ComPtr;

std::string WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);
std::string HrToString(HRESULT hr);
std::string Format(const char* fmt, ...);

inline double QpcSeconds() {
	static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return double(now.QuadPart) / double(freq.QuadPart);
}

} // namespace aether
