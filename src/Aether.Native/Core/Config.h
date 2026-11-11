// Settings: one JSON file (kino/config/Aether/aether.json). Every change goes through Settings::Set, which validates
// the value; the overlay and the MCP control tools use the same entry point.
#pragma once
#include "Common.h"
#include "Json.h"

namespace aether {

struct SettingsData {
	// overlay
	int overlayHotkey = 0x79;        // VK_F10
	bool overlayVisibleAtStart = false;
	float overlayScale = 1.0f;       // 0.5 .. 3
	// renderer
	std::string quality = "high";    // low | medium | high | ultra
	bool testTriangle = true;        // Phase 1 PoC pass
	bool gpuTiming = true;           // timestamp queries around the frame
	std::string debugView = "none";  // none (albedo, normal, ... arrive with the renderer)
	// mcp
	std::string mcpControl = "ask";  // deny | ask | allow: what a control request from an AI agent needs
	int mcpApprovalTimeoutSec = 30;  // 5 .. 300
	// logging
	std::string logLevel = "info";   // debug | info | warning | error
};

class Settings {
public:
	static Settings& Get();
	void Load(const std::wstring& file);
	bool Save();
	SettingsData Snapshot();
	json ToJson();
	// Sets one value by its JSON name. Returns "" on success, else why the value was refused.
	std::string Set(const std::string& key, const json& value, const char* source);
	static json Schema(); // name -> {type, allowed / min / max, description}
	uint64_t Revision() const { return revision_.load(); }

private:
	std::mutex mutex_;
	SettingsData data_;
	std::wstring file_;
	std::atomic<uint64_t> revision_{1};
};

// Render features that exist as named switches (set_render_feature). Phase 1 has the PoC pass and GPU timing.
const std::vector<std::string>& RenderFeatureNames();
const std::vector<std::string>& QualityNames();

} // namespace aether
