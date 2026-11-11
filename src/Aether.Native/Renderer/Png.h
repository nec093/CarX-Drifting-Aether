#pragma once
#include "../Core/Common.h"

namespace aether {

// Back buffer rows -> tightly packed RGBA8 (alpha forced to 255). False for a format it does not know.
bool ConvertToRgba8(DXGI_FORMAT format, const uint8_t* src, uint32_t rowPitch, uint32_t width, uint32_t height, std::vector<uint8_t>& out);
// Writes a PNG with WIC. Not for the render thread.
bool SavePng(const std::wstring& file, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba, std::string& error);

} // namespace aether
