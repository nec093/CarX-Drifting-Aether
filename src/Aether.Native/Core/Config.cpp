#include "Config.h"
#include "Log.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace aether {

static const std::vector<std::string> kQualities = {"low", "medium", "high", "ultra"};
static const std::vector<std::string> kFeatures = {"testTriangle", "gpuTiming", "shadows", "sky", "ssao", "antialiasing", "bloom", "motionBlur", "ssr", "particles", "volumetricFog", "indirectLight", "reflectionProbe", "rayTracedReflections", "followSceneNight", "probeGi"};
static const std::vector<std::string> kRenderModes = {"hdrp", "aether", "compare", "standalone"};
static const std::vector<std::string> kSkyModes = {"physical", "scene"};
static const std::vector<std::string> kTonemaps = {"host", "agx", "aces", "neutral", "none"};
static const std::vector<std::string> kControlModes = {"deny", "ask", "allow"};
static const std::vector<std::string> kLogLevels = {"debug", "info", "warning", "error"};
static const std::vector<std::string> kDebugViews = {"none", "albedo", "normal", "roughness", "metallic", "depth", "lighting", "shadow", "materialClass", "sun", "ambient", "reflection", "bvh", "bvhCost"};

const std::vector<std::string>& RenderFeatureNames() { return kFeatures; }
const std::vector<std::string>& QualityNames() { return kQualities; }
const std::vector<std::string>& RenderModeNames() { return kRenderModes; }
const std::vector<std::string>& DebugViewNames() { return kDebugViews; }

Settings& Settings::Get() {
	static Settings* s = new Settings();
	return *s;
}

static json ToJsonData(const SettingsData& d) {
	return {
		{"overlayHotkey", d.overlayHotkey},
		{"overlayVisibleAtStart", d.overlayVisibleAtStart},
		{"overlayScale", d.overlayScale},
		{"quality", d.quality},
		{"testTriangle", d.testTriangle},
		{"gpuTiming", d.gpuTiming},
		{"debugView", d.debugView},
		{"renderMode", d.renderMode},
		{"shadows", d.shadows},
		{"sky", d.sky},
		{"ssao", d.ssao},
		{"antialiasing", d.antialiasing},
		{"bloom", d.bloom},
		{"motionBlur", d.motionBlur},
		{"ssr", d.ssr},
		{"particles", d.particles},
		{"volumetricFog", d.volumetricFog},
		{"indirectLight", d.indirectLight},
		{"reflectionProbe", d.reflectionProbe},
		{"probeGi", d.probeGi},
		{"followSceneNight", d.followSceneNight},
		{"rayTracedReflections", d.rayTracedReflections},
		{"indirectStrength", d.indirectStrength},
		{"fogStrength", d.fogStrength},
		{"moonlight", d.moonlight},
		{"skinnedRayUpdates", d.skinnedRayUpdates},
		{"tonemap", d.tonemap},
		{"skyMode", d.skyMode},
		{"timeOfDay", d.timeOfDay},
		{"sunHeading", d.sunHeading},
		{"cloudCoverage", d.cloudCoverage},
		{"cloudSpeed", d.cloudSpeed},
		{"exposureEV", d.exposureEV},
		{"vramBudgetMB", d.vramBudgetMB},
		{"vramWarnPercent", d.vramWarnPercent},
		{"mcpControl", d.mcpControl},
		{"mcpApprovalTimeoutSec", d.mcpApprovalTimeoutSec},
		{"shadowSoftness", d.shadowSoftness},
		{"motionBlurStrength", d.motionBlurStrength},
		{"bloomStrength", d.bloomStrength},
		{"smokeOpacity", d.smokeOpacity},
		{"lodBias", d.lodBias},
		{"anisotropicLevel", d.anisotropicLevel},
		{"vsync", d.vsync},
		{"logLevel", d.logLevel},
	};
}

json Settings::Schema() {
	auto oneOf = [](const std::vector<std::string>& v, const char* text) { return json{{"type", "string"}, {"enum", v}, {"description", text}}; };
	return {
		{"overlayHotkey", {{"type", "integer"}, {"minimum", 8}, {"maximum", 254}, {"description", "Windows virtual key code that shows / hides the overlay (default 121 = F10)"}}},
		{"overlayVisibleAtStart", {{"type", "boolean"}, {"description", "Show the overlay when the game starts"}}},
		{"overlayScale", {{"type", "number"}, {"minimum", 0.5}, {"maximum", 3.0}, {"description", "Overlay UI scale"}}},
		{"quality", oneOf(kQualities, "Render quality preset")},
		{"testTriangle", {{"type", "boolean"}, {"description", "Draw the DX12 integration test triangle"}}},
		{"gpuTiming", {{"type", "boolean"}, {"description", "Measure GPU time with timestamp queries"}}},
		{"debugView", oneOf(kDebugViews, "Debug view of Aether's renderer (used when renderMode is aether or compare)")},
		{"renderMode", oneOf(kRenderModes, "Who draws the scene: aether (Aether is the game's render pipeline; the game's own is not created), standalone (the same, older name), hdrp (the game's own renderer, for reference), compare (left half the game's renderer, right half Aether)")},
		{"shadows", {{"type", "boolean"}, {"description", "Directional shadow cascades"}}},
		{"sky", {{"type", "boolean"}, {"description", "Sky background and environment reflections"}}},
		{"ssao", {{"type", "boolean"}, {"description", "Screen space ambient occlusion (strength, radius and effect on direct light follow the game's own settings)"}}},
		{"antialiasing", {{"type", "boolean"}, {"description", "Temporal antialiasing"}}},
		{"bloom", {{"type", "boolean"}, {"description", "Bloom (intensity and scatter follow the game's own settings)"}}},
		{"motionBlur", {{"type", "boolean"}, {"description", "Motion blur (intensity follows the game's own settings)"}}},
		{"volumetricFog", {{"type", "boolean"}, {"description", "Fog lit by the sun through the shadow map (light shafts), where the scene has fog"}}},
		{"indirectLight", {{"type", "boolean"}, {"description", "Light bounced off nearby surfaces (screen space)"}}},
		{"reflectionProbe", {{"type", "boolean"}, {"description", "Reflections of the surroundings, also of what is off screen: a cube drawn around the camera every frame"}}},
		{"followSceneNight", {{"type", "boolean"}, {"description", "The physical sky turns to night (about midnight, moon up) while the game's own day / night switch is on night"}}},
		{"probeGi", {{"type", "boolean"}, {"description", "Indirect light from a grid of probes around the camera: each traces rays through the scene's triangles every frame (roofs shut out the sky, lit surfaces light their surroundings)"}}},
		{"rayTracedReflections", {{"type", "boolean"}, {"description", "Reflections on smooth surfaces from rays traced through the scene's triangles (what is off screen too)"}}},
		{"moonlight", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 4.0}, {"description", "The moon's light at night, as a factor"}}},
		{"fogStrength", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 20.0}, {"description", "Factor on the scene's fog density"}}},
		{"skinnedRayUpdates", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 4.0}, {"description", "Skinned meshes whose pose is read back from the GPU per frame for ray traced reflections (0 = none, the default: reading every frame makes the game skip frames after a while)"}}},
		{"indirectStrength", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 3.0}, {"description", "Factor on the bounced light"}}},
		{"particles", {{"type", "boolean"}, {"description", "Particles (the game's particle systems and tyre smoke)"}}},
		{"ssr", {{"type", "boolean"}, {"description", "Screen space reflections (on where the game's volumes have them on; needs temporal antialiasing)"}}},
		{"tonemap", oneOf(kTonemaps, "Tone mapping curve")},
		{"skyMode", oneOf(kSkyModes, "Sky and sun: physical (Aether's atmosphere at timeOfDay) or scene (the scene's own sky texture and sun)")},
		{"timeOfDay", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 24.0}, {"description", "Time of day in hours for the physical sky"}}},
		{"cloudCoverage", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}, {"description", "How much of the sky the clouds take (physical sky; 0 = no clouds)"}}},
		{"cloudSpeed", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 4.0}, {"description", "How fast the clouds drift"}}},
		{"sunHeading", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 360.0}, {"description", "Turns the sun's path about the up axis, in degrees (physical sky)"}}},
		{"shadowSoftness", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 4.0}, {"description", "How far the sun's shadow edges are blurred, in shadow map texels (0 = hard)"}}},
		{"motionBlurStrength", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 2.0}, {"description", "Motion blur strength, as a factor on what the scene asks for"}}},
		{"bloomStrength", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 2.0}, {"description", "Bloom strength, as a factor on what the scene asks for"}}},
		{"smokeOpacity", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 2.0}, {"description", "Tyre smoke opacity factor"}}},
		{"lodBias", {{"type", "number"}, {"minimum", 0.3}, {"maximum", 8.0}, {"description", "Unity's LOD bias: higher keeps detailed models up to a greater distance"}}},
		{"anisotropicLevel", {{"type", "integer"}, {"minimum", 1}, {"maximum", 16}, {"description", "Anisotropic texture filtering level"}}},
		{"vsync", {{"type", "integer"}, {"minimum", 0}, {"maximum", 2}, {"description", "Vertical sync: 0 off, 1 every refresh, 2 every second refresh"}}},
		{"exposureEV", {{"type", "number"}, {"minimum", -10.0}, {"maximum", 10.0}, {"description", "Exposure bias in stops on top of the scene's exposure"}}},
		{"vramBudgetMB", {{"type", "integer"}, {"minimum", 0}, {"maximum", 65536}, {"description", "Budget for Aether-owned GPU memory in MB (0 = none). Crossing it is reported; nothing is freed automatically"}}},
		{"vramWarnPercent", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}, {"description", "Warning threshold in percent of the VRAM budget"}}},
		{"mcpControl", oneOf(kControlModes, "What a control request from an AI agent needs: deny, ask (in-game approval), allow")},
		{"mcpApprovalTimeoutSec", {{"type", "integer"}, {"minimum", 5}, {"maximum", 300}, {"description", "Seconds an approval prompt waits before it counts as denied"}}},
		{"logLevel", oneOf(kLogLevels, "Lowest severity written to the log")},
	};
}

static bool OneOf(const std::vector<std::string>& v, const std::string& s) {
	return std::find(v.begin(), v.end(), s) != v.end();
}

// Applies one value to d. Returns "" or the reason for the refusal. No locking, no side effects besides d.
static std::string Apply(SettingsData& d, const std::string& key, const json& value) {
	auto wantBool = [&](bool& out) -> std::string {
		if (!value.is_boolean()) {
			return key + " must be a boolean";
		}
		out = value.get<bool>();
		return {};
	};
	auto wantInt = [&](int& out, int lo, int hi) -> std::string {
		if (!value.is_number_integer()) {
			return key + " must be an integer";
		}
		long long v = value.get<long long>();
		if (v < lo || v > hi) {
			return Format("%s must be between %d and %d", key.c_str(), lo, hi);
		}
		out = int(v);
		return {};
	};
	auto wantEnum = [&](std::string& out, const std::vector<std::string>& allowed) -> std::string {
		if (!value.is_string() || !OneOf(allowed, value.get<std::string>())) {
			std::string list;
			for (const auto& a : allowed) {
				list += (list.empty() ? "" : ", ") + a;
			}
			return key + " must be one of: " + list;
		}
		out = value.get<std::string>();
		return {};
	};

	if (key == "overlayHotkey") return wantInt(d.overlayHotkey, 8, 254);
	if (key == "overlayVisibleAtStart") return wantBool(d.overlayVisibleAtStart);
	if (key == "overlayScale") {
		if (!value.is_number()) {
			return "overlayScale must be a number";
		}
		double v = value.get<double>();
		if (!(v >= 0.5 && v <= 3.0)) {
			return "overlayScale must be between 0.5 and 3";
		}
		d.overlayScale = float(v);
		return {};
	}
	if (key == "quality") return wantEnum(d.quality, kQualities);
	if (key == "testTriangle") return wantBool(d.testTriangle);
	if (key == "gpuTiming") return wantBool(d.gpuTiming);
	if (key == "debugView") return wantEnum(d.debugView, kDebugViews);
	if (key == "renderMode") return wantEnum(d.renderMode, kRenderModes);
	if (key == "shadows") return wantBool(d.shadows);
	if (key == "sky") return wantBool(d.sky);
	if (key == "ssao") return wantBool(d.ssao);
	if (key == "antialiasing") return wantBool(d.antialiasing);
	if (key == "bloom") return wantBool(d.bloom);
	if (key == "motionBlur") return wantBool(d.motionBlur);
	if (key == "ssr") return wantBool(d.ssr);
	if (key == "particles") return wantBool(d.particles);
	if (key == "volumetricFog") return wantBool(d.volumetricFog);
	if (key == "indirectLight") return wantBool(d.indirectLight);
	if (key == "reflectionProbe") return wantBool(d.reflectionProbe);
	if (key == "probeGi") return wantBool(d.probeGi);
	if (key == "followSceneNight") return wantBool(d.followSceneNight);
	if (key == "rayTracedReflections") return wantBool(d.rayTracedReflections);
	if (key == "skinnedRayUpdates") {
		if (!value.is_number() || !(value.get<double>() >= 0.0 && value.get<double>() <= 4.0)) {
			return "skinnedRayUpdates must be a number between 0 and 4";
		}
		d.skinnedRayUpdates = float(value.get<double>());
		return {};
	}
	if (key == "moonlight") {
		if (!value.is_number() || !(value.get<double>() >= 0.0 && value.get<double>() <= 4.0)) {
			return "moonlight must be a number between 0 and 4";
		}
		d.moonlight = float(value.get<double>());
		return {};
	}
	if (key == "fogStrength") {
		if (!value.is_number() || !(value.get<double>() >= 0.0 && value.get<double>() <= 20.0)) {
			return "fogStrength must be a number between 0 and 20";
		}
		d.fogStrength = float(value.get<double>());
		return {};
	}
	if (key == "indirectStrength") {
		if (!value.is_number() || !(value.get<double>() >= 0.0 && value.get<double>() <= 3.0)) {
			return "indirectStrength must be a number between 0 and 3";
		}
		d.indirectStrength = float(value.get<double>());
		return {};
	}
	if (key == "tonemap") return wantEnum(d.tonemap, kTonemaps);
	if (key == "skyMode") return wantEnum(d.skyMode, kSkyModes);
	if (key == "cloudCoverage" || key == "cloudSpeed") {
		const double hi = key == "cloudCoverage" ? 1.0 : 4.0;
		if (!value.is_number() || !(value.get<double>() >= 0.0 && value.get<double>() <= hi)) {
			return key + " is out of range";
		}
		(key == "cloudCoverage" ? d.cloudCoverage : d.cloudSpeed) = float(value.get<double>());
		return {};
	}
	if (key == "timeOfDay" || key == "sunHeading") {
		const double hi = key == "timeOfDay" ? 24.0 : 360.0;
		if (!value.is_number() || !(value.get<double>() >= 0.0 && value.get<double>() <= hi)) {
			return key + " is out of range";
		}
		(key == "timeOfDay" ? d.timeOfDay : d.sunHeading) = float(value.get<double>());
		return {};
	}
	if (key == "exposureEV") {
		if (!value.is_number() || !(value.get<double>() >= -10.0 && value.get<double>() <= 10.0)) {
			return "exposureEV must be a number between -10 and 10";
		}
		d.exposureEV = float(value.get<double>());
		return {};
	}
	if (key == "shadowSoftness") {
		if (!value.is_number() || !(value.get<double>() >= 0.0 && value.get<double>() <= 4.0)) {
			return "shadowSoftness must be a number between 0.0 and 4.0";
		}
		d.shadowSoftness = float(value.get<double>());
		return {};
	}
	if (key == "motionBlurStrength") {
		if (!value.is_number() || !(value.get<double>() >= 0.0 && value.get<double>() <= 2.0)) {
			return "motionBlurStrength must be a number between 0.0 and 2.0";
		}
		d.motionBlurStrength = float(value.get<double>());
		return {};
	}
	if (key == "bloomStrength") {
		if (!value.is_number() || !(value.get<double>() >= 0.0 && value.get<double>() <= 2.0)) {
			return "bloomStrength must be a number between 0.0 and 2.0";
		}
		d.bloomStrength = float(value.get<double>());
		return {};
	}
	if (key == "smokeOpacity") {
		if (!value.is_number() || !(value.get<double>() >= 0.0 && value.get<double>() <= 2.0)) {
			return "smokeOpacity must be a number between 0.0 and 2.0";
		}
		d.smokeOpacity = float(value.get<double>());
		return {};
	}
	if (key == "lodBias") {
		if (!value.is_number() || !(value.get<double>() >= 0.3 && value.get<double>() <= 8.0)) {
			return "lodBias must be a number between 0.3 and 8.0";
		}
		d.lodBias = float(value.get<double>());
		return {};
	}
	if (key == "anisotropicLevel") return wantInt(d.anisotropicLevel, 1, 16);
	if (key == "vsync") return wantInt(d.vsync, 0, 2);
	if (key == "vramBudgetMB") return wantInt(d.vramBudgetMB, 0, 65536);
	if (key == "vramWarnPercent") return wantInt(d.vramWarnPercent, 1, 100);
	if (key == "mcpControl") return wantEnum(d.mcpControl, kControlModes);
	if (key == "mcpApprovalTimeoutSec") return wantInt(d.mcpApprovalTimeoutSec, 5, 300);
	if (key == "logLevel") return wantEnum(d.logLevel, kLogLevels);
	return "unknown setting: " + key;
}

void Settings::Load(const std::wstring& file) {
	std::lock_guard lock(mutex_);
	file_ = file;
	std::ifstream in(file, std::ios::binary);
	if (!in) {
		AE_LOG(Info, "Config", "No settings file yet, using defaults (%s)", WideToUtf8(file).c_str());
		return;
	}
	std::stringstream buffer;
	buffer << in.rdbuf();
	json j = json::parse(buffer.str(), nullptr, false, true);
	if (j.is_discarded() || !j.is_object()) {
		AE_LOG_CODE(Error, "Config", "E_CONFIG_PARSE", "Settings file is not valid JSON, using defaults (%s)", WideToUtf8(file).c_str());
		return;
	}
	for (auto it = j.begin(); it != j.end(); ++it) {
		std::string why = Apply(data_, it.key(), it.value());
		if (!why.empty()) {
			AE_LOG_CODE(Warning, "Config", "W_CONFIG_VALUE", "Ignored setting from file: %s", why.c_str());
		}
	}
	revision_.fetch_add(1);
}

bool Settings::Save() {
	std::lock_guard lock(mutex_);
	if (file_.empty()) {
		return false;
	}
	size_t slash = file_.find_last_of(L"\\/");
	if (slash != std::wstring::npos) {
		CreateDirectoryW(file_.substr(0, slash).c_str(), nullptr);
	}
	std::wstring tmp = file_ + L".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out) {
			AE_LOG_CODE(Error, "Config", "E_CONFIG_WRITE", "Cannot write settings (%s)", WideToUtf8(tmp).c_str());
			return false;
		}
		out << Dump(ToJsonData(data_), 2) << "\n";
	}
	return MoveFileExW(tmp.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

SettingsData Settings::Snapshot() {
	std::lock_guard lock(mutex_);
	return data_;
}

json Settings::ToJson() {
	std::lock_guard lock(mutex_);
	return ToJsonData(data_);
}

std::string Settings::Set(const std::string& key, const json& value, const char* source) {
	{
		std::lock_guard lock(mutex_);
		SettingsData copy = data_;
		std::string why = Apply(copy, key, value);
		if (!why.empty()) {
			return why;
		}
		data_ = copy;
	}
	revision_.fetch_add(1);
	AE_LOG(Info, "Config", "%s = %s (by %s)", key.c_str(), Dump(value).c_str(), source);
	Save();
	return {};
}

} // namespace aether
