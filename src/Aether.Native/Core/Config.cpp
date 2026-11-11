#include "Config.h"
#include "Log.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace aether {

static const std::vector<std::string> kQualities = {"low", "medium", "high", "ultra"};
static const std::vector<std::string> kFeatures = {"testTriangle", "gpuTiming"};
static const std::vector<std::string> kControlModes = {"deny", "ask", "allow"};
static const std::vector<std::string> kLogLevels = {"debug", "info", "warning", "error"};
static const std::vector<std::string> kDebugViews = {"none"};

const std::vector<std::string>& RenderFeatureNames() { return kFeatures; }
const std::vector<std::string>& QualityNames() { return kQualities; }

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
		{"mcpControl", d.mcpControl},
		{"mcpApprovalTimeoutSec", d.mcpApprovalTimeoutSec},
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
		{"debugView", oneOf(kDebugViews, "Debug view")},
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
