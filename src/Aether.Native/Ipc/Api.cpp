// The runtime's API: what an MCP tool call turns into. Read methods answer from snapshots. Control methods go through
// Authorize (the MCP server's --allow-control switch, the mcpControl setting, the in-game approval) and then through
// the same validated entry points the overlay uses. Nothing here touches the device or a resource's lifetime.
#include "../Renderer/Bvh.h"
#include "PipeServer.h"
#include "../Core/Config.h"
#include "../Core/Log.h"
#include "../Device/GpuInfo.h"
#include "../Diagnostics.h"
#include "../Profiler/Profiler.h"
#include "../Renderer/Renderer.h"
#include "../Renderer/SceneRenderer.h"
#include "../Resources/GpuResources.h"
#include "../Scene/SceneStore.h"
#include "../Resources/ResourceRegistry.h"
#include "../Runtime.h"
#include "../Shaders/ShaderManager.h"

#include <fstream>

namespace aether {

namespace {

struct ApiError {
	std::string code, message;
};

json Ok(const json& id, json result) {
	return {{"id", id}, {"ok", true}, {"result", std::move(result)}};
}

json Fail(const json& id, const ApiError& e) {
	return {{"id", id}, {"ok", false}, {"error", {{"code", e.code}, {"message", e.message}}}};
}

// Typed parameter access: a wrong type is an error, not a silent default.
bool GetInt(const json& params, const char* name, long long lo, long long hi, long long fallback, long long& out, ApiError& error) {
	out = fallback;
	if (!params.contains(name) || params[name].is_null()) {
		return true;
	}
	if (!params[name].is_number_integer()) {
		error = {"E_INVALID_PARAMS", std::string(name) + " must be an integer"};
		return false;
	}
	out = params[name].get<long long>();
	if (out < lo || out > hi) {
		error = {"E_INVALID_PARAMS", Format("%s must be between %lld and %lld", name, lo, hi)};
		return false;
	}
	return true;
}

bool GetBool(const json& params, const char* name, bool fallback, bool& out, ApiError& error) {
	out = fallback;
	if (!params.contains(name) || params[name].is_null()) {
		return true;
	}
	if (!params[name].is_boolean()) {
		error = {"E_INVALID_PARAMS", std::string(name) + " must be a boolean"};
		return false;
	}
	out = params[name].get<bool>();
	return true;
}

bool GetString(const json& params, const char* name, const std::string& fallback, std::string& out, ApiError& error, size_t maxLength = 256) {
	out = fallback;
	if (!params.contains(name) || params[name].is_null()) {
		return true;
	}
	if (!params[name].is_string()) {
		error = {"E_INVALID_PARAMS", std::string(name) + " must be a string"};
		return false;
	}
	out = params[name].get<std::string>();
	if (out.size() > maxLength) {
		error = {"E_INVALID_PARAMS", Format("%s is longer than %zu characters", name, maxLength)};
		return false;
	}
	return true;
}

bool Authorize(const json& request, const std::string& method, const json& params, ApiError& error) {
	if (!request.value("control", false)) {
		error = {"E_CONTROL_DISABLED", "this connection is read-only: start the Aether MCP server with --allow-control to use control tools"};
		return false;
	}
	SettingsData s = Settings::Get().Snapshot();
	if (s.mcpControl == "deny") {
		error = {"E_CONTROL_DENIED", "control requests are switched off in the game (Aether overlay > Settings > Control requests)"};
		return false;
	}
	if (s.mcpControl == "ask") {
		std::string why;
		if (!Runtime::Get().RequestApproval(method, Dump(params), s.mcpApprovalTimeoutSec, why)) {
			error = {"E_NOT_APPROVED", why};
			return false;
		}
	}
	AE_LOG(Info, "Ipc", "Control request accepted: %s %s", method.c_str(), Dump(params).c_str());
	return true;
}

bool SeverityFromName(const std::string& name, Severity& out) {
	static const char* names[] = {"debug", "info", "warning", "error", "fatal"};
	for (int i = 0; i < 5; ++i) {
		if (name == names[i]) {
			out = Severity(i);
			return true;
		}
	}
	return false;
}

json WithAge(const char* key, bool refreshed) {
	double age = -1;
	json value = Runtime::Get().ManagedState(key, &age);
	if (value.is_null()) {
		return {{"available", false}, {"reason", "the game side has not published this yet (is the game past its first frames?)"}};
	}
	return {{"available", true}, {"ageSeconds", age}, {"refreshed", refreshed}, {"data", value}};
}

} // namespace

json HandleRequest(const json& request) {
	const json id = request.contains("id") ? request["id"] : json(nullptr);
	if (!request.contains("method") || !request["method"].is_string()) {
		return Fail(id, {"E_BAD_REQUEST", "method is missing"});
	}
	const std::string method = request["method"].get<std::string>();
	const json params = request.contains("params") && request["params"].is_object() ? request["params"] : json::object();
	Runtime& rt = Runtime::Get();
	ApiError error;

	// ------------------------------------------------------------------ read only
	if (method == "ping" || method == "get_status") {
		return Ok(id, BuildStatus());
	}
	if (method == "get_gpu_info") {
		return Ok(id, GpuInfo::Get().ToJson());
	}
	if (method == "get_performance") {
		long long history;
		if (!GetInt(params, "historyFrames", 0, 2000, 0, history, error)) {
			return Fail(id, error);
		}
		json perf = Profiler::Get().Performance(int(history));
		perf["passes"] = Profiler::Get().Passes()["passes"];
		perf["benchmark"] = Profiler::Get().BenchmarkStatus().value("state", "idle");
		return Ok(id, perf);
	}
	if (method == "get_vram_usage") {
		return Ok(id, BuildVramReport());
	}
	if (method == "get_resource_list") {
		long long limit = 0;
		std::string category;
		bool includeUnity = false;
		if (!GetInt(params, "limit", 1, 2000, 200, limit, error) || !GetString(params, "category", "", category, error) || !GetBool(params, "includeUnityRenderTextures", false, includeUnity, error)) {
			return Fail(id, error);
		}
		json out = ResourceRegistry::Get().List(size_t(limit), category);
		out["summary"] = ResourceRegistry::Get().Summary();
		if (includeUnity) {
			bool refreshed = rt.RefreshManaged(kRequestRenderTextures, "renderTextures", 3000);
			out["unityRenderTextures"] = WithAge("renderTextures", refreshed);
		}
		return Ok(id, out);
	}
	if (method == "get_render_graph") {
		return Ok(id, Renderer::Get().RenderGraph());
	}
	if (method == "get_shader_errors") {
		bool all;
		if (!GetBool(params, "includeAllShaders", false, all, error)) {
			return Fail(id, error);
		}
		json out = ShaderManager::Get().Errors();
		if (all) {
			out["all"] = ShaderManager::Get().List();
		}
		return Ok(id, out);
	}
	if (method == "get_material_info") {
		bool refresh = true;
		std::string detail;
		if (!GetBool(params, "refresh", true, refresh, error) || !GetString(params, "detail", "summary", detail, error)) {
			return Fail(id, error);
		}
		if (detail == "survey") {
			// shader property layouts with sample values, vertex layouts, lights, environment (a heavier walk)
			bool refreshed = refresh && rt.RefreshManaged(kRequestSurvey, "survey", 10000);
			return Ok(id, WithAge("survey", refreshed));
		}
		if (detail == "aether") {
			// what Aether's translation made of the materials (class, flags, bound textures, unmapped properties)
			long long limit = 0;
			if (!GetInt(params, "limit", 1, 600, 200, limit, error)) {
				return Fail(id, error);
			}
			json summary = SceneStore::Get().Summary();
			return Ok(id, {{"available", summary.contains("materials")}, {"summary", summary.contains("materials") ? summary["materials"] : json(nullptr)},
				{"materials", SceneStore::Get().MaterialReport(size_t(limit))},
				{"note", "Filled while the render mode is aether or compare. class unsupported = drawn with the diagnostic material."}});
		}
		if (detail != "summary") {
			return Fail(id, {"E_INVALID_PARAMS", "detail must be summary, survey or aether"});
		}
		bool refreshed = refresh && rt.RefreshManaged(kRequestMaterials, "materials", 5000);
		return Ok(id, WithAge("materials", refreshed));
	}
	if (method == "get_scene_info") {
		bool refreshed = rt.RefreshManaged(kRequestScene, "scene", 2000);
		json out = WithAge("scene", refreshed);
		out["game"] = rt.ManagedState("managed");
		// what Aether's own renderer captured and drew (render mode aether / compare)
		out["aetherScene"] = SceneStore::Get().Summary();
		out["aetherRenderer"] = SceneRenderer::Get().Stats();
		out["rayTracing"] = SceneBvh::Get().Stats();
		return Ok(id, out);
	}
	if (method == "get_logs") {
		long long limit = 0, afterSeq = 0;
		std::string severity, component;
		if (!GetInt(params, "limit", 1, 1000, 100, limit, error) || !GetInt(params, "afterSeq", 0, LLONG_MAX, 0, afterSeq, error) ||
			!GetString(params, "minSeverity", "info", severity, error) || !GetString(params, "component", "", component, error)) {
			return Fail(id, error);
		}
		Severity min;
		if (!SeverityFromName(severity, min)) {
			return Fail(id, {"E_INVALID_PARAMS", "minSeverity must be one of: debug, info, warning, error, fatal"});
		}
		json entries = json::array();
		for (const LogEntry& e : Log::Get().Snapshot(size_t(limit), min, uint64_t(afterSeq), component)) {
			entries.push_back(ToJson(e));
		}
		return Ok(id, {{"lastSeq", Log::Get().LastSeq()}, {"errorCount", Log::Get().ErrorCount()}, {"count", entries.size()}, {"entries", entries}, {"logDirectory", WideToUtf8(Log::Get().Directory())}});
	}
	if (method == "get_crash_report") {
		json out = rt.CrashReports();
		out["rendererState"] = Renderer::Get().State();
		out["deviceState"] = GpuInfo::Get().DeviceState();
		return Ok(id, out);
	}
	if (method == "get_settings") {
		return Ok(id, {{"values", Settings::Get().ToJson()}, {"schema", Settings::Schema()}, {"renderFeatures", RenderFeatureNames()}, {"qualityPresets", QualityNames()}});
	}
	if (method == "get_benchmark") {
		return Ok(id, Profiler::Get().BenchmarkStatus());
	}
	if (method == "get_capture") {
		long long captureId;
		if (!GetInt(params, "id", 1, LLONG_MAX, 0, captureId, error)) {
			return Fail(id, error);
		}
		return Ok(id, Renderer::Get().CaptureStatus(uint64_t(captureId)));
	}

	// ------------------------------------------------------------------ control
	if (method == "set_render_quality") {
		std::string quality;
		if (!GetString(params, "quality", "", quality, error)) {
			return Fail(id, error);
		}
		const auto& names = QualityNames();
		if (std::find(names.begin(), names.end(), quality) == names.end()) {
			return Fail(id, {"E_INVALID_PARAMS", "quality must be one of: low, medium, high, ultra"});
		}
		if (!Authorize(request, method, params, error)) {
			return Fail(id, error);
		}
		std::string why = Settings::Get().Set("quality", quality, "mcp");
		if (!why.empty()) {
			return Fail(id, {"E_INVALID_PARAMS", why});
		}
		return Ok(id, {{"quality", quality}, {"note", "stored; Aether's own renderer uses the preset from Phase 2"}});
	}
	if (method == "set_render_feature") {
		std::string feature;
		bool enabled;
		if (!GetString(params, "feature", "", feature, error)) {
			return Fail(id, error);
		}
		if (!params.contains("enabled") || !params["enabled"].is_boolean()) {
			return Fail(id, {"E_INVALID_PARAMS", "enabled must be a boolean"});
		}
		enabled = params["enabled"].get<bool>();
		const auto& names = RenderFeatureNames();
		if (std::find(names.begin(), names.end(), feature) == names.end()) {
			std::string list;
			for (const auto& n : names) {
				list += (list.empty() ? "" : ", ") + n;
			}
			return Fail(id, {"E_INVALID_PARAMS", "feature must be one of: " + list});
		}
		if (!Authorize(request, method, params, error)) {
			return Fail(id, error);
		}
		std::string why = Settings::Get().Set(feature, enabled, "mcp");
		if (!why.empty()) {
			return Fail(id, {"E_INVALID_PARAMS", why});
		}
		return Ok(id, {{"feature", feature}, {"enabled", enabled}});
	}
	if (method == "set_graphics_option") {
		// one of Aether's own graphics options (the same ones the overlay's Settings tab shows)
		static const char* const kOptions[] = {"skyMode", "timeOfDay", "sunHeading", "cloudCoverage", "cloudSpeed", "shadowSoftness", "indirectStrength", "fogStrength", "moonlight", "skinnedRayUpdates", "motionBlurStrength", "bloomStrength", "aoStrength", "aoRadius", "lensFlareStrength", "smokeOpacity", "lodBias", "anisotropicLevel", "vsync", "exposureEV", "tonemap"};
		std::string option;
		if (!GetString(params, "option", "", option, error)) {
			return Fail(id, error);
		}
		bool known = false;
		std::string list;
		for (const char* o : kOptions) {
			known |= option == o;
			list += (list.empty() ? "" : ", ") + std::string(o);
		}
		if (!known || !params.contains("value") || !(params["value"].is_number() || params["value"].is_string())) {
			return Fail(id, {"E_INVALID_PARAMS", "option must be one of: " + list + "; value a number or a string"});
		}
		if (!Authorize(request, method, params, error)) {
			return Fail(id, error);
		}
		std::string why = Settings::Get().Set(option, params["value"], "mcp");
		if (!why.empty()) {
			return Fail(id, {"E_INVALID_PARAMS", why});
		}
		return Ok(id, {{"option", option}, {"value", Settings::Get().ToJson()[option]}});
	}
	if (method == "set_render_mode") {
		std::string mode, debugView;
		if (!GetString(params, "mode", "", mode, error) || !GetString(params, "debugView", "", debugView, error)) {
			return Fail(id, error);
		}
		const auto& modes = RenderModeNames();
		const auto& views = DebugViewNames();
		if (!mode.empty() && std::find(modes.begin(), modes.end(), mode) == modes.end()) {
			return Fail(id, {"E_INVALID_PARAMS", "mode must be one of: hdrp, aether, compare"});
		}
		if (!debugView.empty() && std::find(views.begin(), views.end(), debugView) == views.end()) {
			return Fail(id, {"E_INVALID_PARAMS", "debugView must be one of: none, albedo, normal, roughness, metallic, depth, lighting, shadow, materialClass, sun, ambient, reflection, bvh, bvhCost"});
		}
		if (mode.empty() && debugView.empty()) {
			return Fail(id, {"E_INVALID_PARAMS", "give mode, debugView or both"});
		}
		if (!Authorize(request, method, params, error)) {
			return Fail(id, error);
		}
		if (!mode.empty()) {
			Settings::Get().Set("renderMode", mode, "mcp");
		}
		if (!debugView.empty()) {
			Settings::Get().Set("debugView", debugView, "mcp");
		}
		SettingsData now = Settings::Get().Snapshot();
		return Ok(id, {{"mode", now.renderMode}, {"debugView", now.debugView}, {"note", "aether: Aether is the game's render pipeline (the game's own is not created). hdrp: the game's own renderer, for reference. compare: left half the game's renderer, right half Aether."}});
	}
	if (method == "reload_shader") {
		if (!Authorize(request, method, params, error)) {
			return Fail(id, error);
		}
		uint64_t before = Profiler::Get().FrameCount();
		ShaderManager::Get().RequestReload();
		// the renderer rebuilds at its next frame: give it a few frames, then report what the compiler said
		for (int i = 0; i < 100 && Profiler::Get().FrameCount() < before + 3; ++i) {
			Sleep(10);
		}
		json out = ShaderManager::Get().Errors();
		out["reloaded"] = Profiler::Get().FrameCount() >= before + 3;
		out["all"] = ShaderManager::Get().List();
		return Ok(id, out);
	}
	if (method == "capture_frame") {
		bool withOverlay;
		long long timeoutMs;
		if (!GetBool(params, "withOverlay", true, withOverlay, error) || !GetInt(params, "timeoutMs", 100, 30000, 5000, timeoutMs, error)) {
			return Fail(id, error);
		}
		if (!Renderer::Get().Ready()) {
			return Fail(id, {"E_NOT_READY", "the renderer is not running (no frame yet, or it stopped after an error)"});
		}
		if (!Authorize(request, method, params, error)) {
			return Fail(id, error);
		}
		uint64_t capture = Renderer::Get().RequestCapture(NewCapturePath(), withOverlay);
		double deadline = QpcSeconds() + double(timeoutMs) / 1000.0;
		json status;
		for (;;) {
			status = Renderer::Get().CaptureStatus(capture);
			std::string state = status.value("state", "");
			if (state == "done" || state == "failed" || QpcSeconds() >= deadline) {
				break;
			}
			Sleep(10);
		}
		if (status.value("state", "") == "failed") {
			return Fail(id, {"E_CAPTURE_FAILED", status.value("error", "capture failed")});
		}
		return Ok(id, status); // state "pending" / "encoding" after a timeout: ask again with get_capture
	}
	if (method == "capture_render_graph") {
		if (!Authorize(request, method, params, error)) {
			return Fail(id, error);
		}
		json graph = Renderer::Get().RenderGraph();
		std::wstring path = NewCapturePath(L"rendergraph", L"json");
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out) {
			return Fail(id, {"E_WRITE_FAILED", "cannot write " + WideToUtf8(path)});
		}
		out << Dump(graph, 2) << "\n";
		return Ok(id, {{"file", WideToUtf8(path)}, {"graph", graph}});
	}
	if (method == "run_benchmark") {
		long long seconds;
		std::string label;
		bool vsync = true;
		if (!GetInt(params, "seconds", 1, 600, 10, seconds, error) || !GetString(params, "label", "mcp", label, error, 64) || !GetBool(params, "vsync", true, vsync, error)) {
			return Fail(id, error);
		}
		if (!Renderer::Get().Ready()) {
			return Fail(id, {"E_NOT_READY", "the renderer is not running (no frame yet, or it stopped after an error)"});
		}
		if (!Authorize(request, method, params, error)) {
			return Fail(id, error);
		}
		json context = {{"startedBy", "mcp"}, {"scene", rt.ManagedState("scene")}, {"game", rt.ManagedState("managed")}, {"settings", Settings::Get().ToJson()}, {"renderGraph", Renderer::Get().RenderGraph()}};
		context["vsync"] = vsync ? "as the game has it" : "off for the run (QualitySettings.vSyncCount 0, no frame rate cap)";
		Profiler::Get().SetBenchmarkUncapped(!vsync);
		std::string why = Profiler::Get().StartBenchmark(label, double(seconds), rt.Subdir(L"benchmarks"), std::move(context));
		if (!why.empty()) {
			Profiler::Get().SetBenchmarkUncapped(false);
			return Fail(id, {"E_BENCHMARK", why});
		}
		return Ok(id, {{"started", true}, {"seconds", seconds}, {"label", label}, {"vsync", vsync}, {"note", "poll get_benchmark until state is done"}});
	}
	if (method == "stop_benchmark") {
		if (!Authorize(request, method, params, error)) {
			return Fail(id, error);
		}
		Profiler::Get().StopBenchmark();
		return Ok(id, Profiler::Get().BenchmarkStatus());
	}

	return Fail(id, {"E_UNKNOWN_METHOD", "unknown method: " + method});
}

} // namespace aether
