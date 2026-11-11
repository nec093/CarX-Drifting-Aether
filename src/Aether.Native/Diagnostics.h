// Cross-cutting reports assembled from the subsystems (used by the IPC API; the overlay shows the same numbers).
#pragma once
#include "Core/Json.h"

namespace aether {

json BuildStatus();     // one-page summary: versions, renderer state, error counts, what is connected
json BuildVramReport(); // process-wide video memory (DXGI), Aether's tracked share, Unity's reported share

extern const char* const kAetherVersion;

// <data dir>/captures/<prefix>-<date>-<time>.<ext>
std::wstring NewCapturePath(const wchar_t* prefix = L"frame", const wchar_t* extension = L"png");

} // namespace aether
