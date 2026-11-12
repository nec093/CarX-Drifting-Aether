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
	std::string debugView = "none";  // none | albedo | normal | roughness | metallic | depth | lighting | shadow | materialClass | sun | ambient | reflection
	// aether: Aether is the game's render pipeline (the game's own pipeline is never created). standalone: the same (older name).
	// hdrp: the game's own renderer, for reference pictures and measurements. compare: the game's renderer left, Aether right.
	std::string renderMode = "aether";
	bool shadows = true;             // directional shadow cascades
	bool ssao = true;                // screen space ambient occlusion
	bool antialiasing = true;        // temporal antialiasing
	bool bloom = true;               // bloom as the game's volumes set it
	bool motionBlur = true;          // motion blur as the game's volumes set it
	bool particles = true;           // the game's particle systems and tyre smoke
	bool ssr = true;                 // screen space reflections as the game's volumes set them
	bool volumetricFog = true;       // fog lit by the sun through the shadow map (light shafts), where the scene has fog
	bool followSceneNight = true;    // the physical sky turns to night when the game's scene does (its day / night switch)
	bool probeGi = true;             // indirect light from a grid of probes around the camera that trace rays (needs the ray tracing geometry)
	bool reflectionProbe = true;     // the surroundings drawn into a cube around the camera every frame, for reflections
	bool rayTracedReflections = true; // smooth surfaces mirror what rays through the scene's triangles find (also off screen)
	bool indirectLight = true;       // light bounced off nearby surfaces (screen space, with the ambient occlusion pass)
	bool sky = true;                 // sky background and environment reflections
	// Aether's own graphics options (the overlay's Settings tab). The game's graphics menu is not read.
	std::string skyMode = "physical"; // physical: Aether's atmosphere, lit by the sun at timeOfDay | scene: the scene's own sky texture and sun
	float timeOfDay = 14.0f;          // hours, 0 .. 24 (physical sky)
	float sunHeading = 0.0f;          // degrees: turns the sun's path about the up axis (physical sky)
	float cloudCoverage = 0.4f;       // 0 .. 1: how much of the sky the clouds take (physical sky; 0 = none)
	float cloudSpeed = 1.0f;          // 0 .. 4: how fast the clouds drift
	float shadowSoftness = 1.5f; // How far the sun's shadow edges are blurred, in shadow map texels (0 = hard)
	float motionBlurStrength = 1.0f; // Motion blur strength, as a factor on what the scene asks for
	float bloomStrength = 0.3f; // Bloom strength, as a factor on what the scene asks for
	float indirectStrength = 1.0f; // 0 .. 3: factor on the bounced light
	// 0 .. 4: skinned meshes whose pose is read back per frame for ray tracing (0: skinned things keep their first pose in
	// reflections). 0 by default: read back every frame, Unity's frame fence falls further and further behind (measured:
	// after about 45 s forty of Aether's 48 command lists wait for it and most frames are skipped: the picture flickers).
	float skinnedRayUpdates = 0.0f;
	float moonlight = 0.5f;        // 0 .. 4: the moon's light at night, as a factor (1: a night one can see the yard in; 0.25: nearly black)
	float fogStrength = 1.0f;      // 0 .. 20: factor on the scene's fog density
	float smokeOpacity = 1.0f; // Tyre smoke opacity factor
	float lodBias = 2.8f; // Unity's LOD bias: higher keeps detailed models up to a greater distance
	int anisotropicLevel = 9; // Anisotropic texture filtering level
	int vsync = 1; // Vertical sync: 0 off, 1 every refresh, 2 every second refresh
	std::string tonemap = "host";    // host (the curve of the game renderer) | agx | aces | neutral | none
	float exposureEV = 0.0f;         // exposure bias in stops on top of the scene's exposure
	int vramBudgetMB = 2048;         // budget for Aether-owned GPU memory (0 = none): crossing it is reported, nothing is freed
	int vramWarnPercent = 80;        // warning threshold in percent of the budget
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
const std::vector<std::string>& RenderModeNames();
const std::vector<std::string>& DebugViewNames();

} // namespace aether
