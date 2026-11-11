using System.Text.Json.Nodes;
using Aether.Mcp.Ipc;

namespace Aether.Mcp.Tools;

/// <summary>One MCP tool: its schemas and how it maps to the runtime's API.</summary>
public sealed record Tool(
	string Name,
	string Description,
	string InputSchema,
	string OutputSchema,
	bool Control = false,
	int TimeoutSeconds = 15,
	Func<RuntimeClient, JsonObject, CancellationToken, Task<JsonNode?>>? Handler = null);

/// <summary>
/// The tools the Aether MCP server offers. Read tools only read snapshots. Control tools need the server's
/// --allow-control switch and then the game's own rule (setting mcpControl: deny / ask / allow); they change settings or
/// start captures and benchmarks through the same validated entry points as the in-game overlay. No tool can reach the
/// D3D12 device, a resource's lifetime, or run code.
/// </summary>
public static class ToolCatalog {
	private const string NoInput = """{"type":"object","properties":{},"additionalProperties":false}""";
	// control tools may wait for the player's approval in the game (up to mcpApprovalTimeoutSec, max 300 s)
	private const int ControlTimeout = 330;

	public static readonly IReadOnlyList<Tool> All = [
		new("get_status",
			"One-page summary of the Aether runtime in the running game: version, renderer state, GPU, error counts, whether the previous session ended uncleanly, what the game reports about Unity / the render pipeline / other mods. Start here.",
			NoInput,
			"""{"type":"object","properties":{"aetherVersion":{"type":"string"},"phase":{"type":"integer"},"renderer":{"type":"object","description":"state (waiting for the first frame | ready | failed), host, backBuffer, lastError"},"gpu":{"type":["object","null"]},"deviceState":{"type":"string"},"overlayVisible":{"type":"boolean"},"frameCount":{"type":"integer"},"logErrorCount":{"type":"integer"},"shaderErrorCount":{"type":"integer"},"aetherResourceCount":{"type":"integer"},"aetherResourceBytes":{"type":"integer"},"previousSessionEndedUncleanly":{"type":"boolean"},"mcpControl":{"type":"string","enum":["deny","ask","allow"]},"managed":{"type":["object","null"],"description":"reported by the C# mod: loader, unityVersion, graphicsDeviceType, renderPipeline (with HDRP settings), mods"}},"required":["aetherVersion","renderer"]}"""),

		new("get_gpu_info",
			"GPU adapter (vendor, name, device id, driver, dedicated / shared memory) and the D3D12 features determined at run time: shader model, resource binding tier, resource heap tier, wave ops, raytracing tier, mesh shaders, VRS, enhanced barriers, BC texture formats, device state.",
			NoInput,
			"""{"type":"object","properties":{"available":{"type":"boolean"},"adapter":{"type":"object","properties":{"vendor":{"type":"string"},"name":{"type":"string"},"deviceId":{"type":"string"},"driverVersion":{"type":"string"},"dedicatedVideoMemoryBytes":{"type":"integer"},"sharedSystemMemoryBytes":{"type":"integer"}}},"features":{"type":"object","description":"shaderModel, maxFeatureLevel, resourceBindingTier, resourceHeapTier, waveOps, raytracingTier, meshShaderTier, variableShadingRateTier, enhancedBarriers, textureFormats{BC1..BC7}, ..."},"deviceState":{"type":"string","description":"ok, or removed: <reason>"}},"required":["available"]}"""),

		new("get_performance",
			"Frame statistics: FPS, frame time (avg / min / max / percentiles), 1% and 0.1% low FPS, GPU frame time from timestamp queries, Aether's own CPU / GPU cost and draw calls, per-pass timings, and Unity's counters (CPU main / render thread, present wait, draw calls, batches, triangles).",
			"""{"type":"object","properties":{"historyFrames":{"type":"integer","minimum":0,"maximum":2000,"description":"Also return the last N frame times in ms (oldest first). Default 0."}},"additionalProperties":false}""",
			"""{"type":"object","properties":{"available":{"type":"boolean"},"fps":{"type":"number"},"frameTimeMs":{"type":"object","description":"avg, min, max, p50, p95, p99 over the last ~2 s"},"onePercentLowFps":{"type":"number"},"pointOnePercentLowFps":{"type":"number"},"gpuFrameTimeMs":{"type":["object","null"]},"aether":{"type":"object","description":"cpuMs, gpuMs, drawCalls, dispatchCalls"},"unity":{"type":"object","description":"cpuMainThreadMs, cpuRenderThreadMs, cpuPresentWaitMs, gpuFrameMs, drawCalls, batches, setPassCalls, triangles, vertices (only those the game exposes)"},"passes":{"type":"array","items":{"type":"object"}},"frameTimeHistoryMs":{"type":"array","items":{"type":"number"}}},"required":["available"]}"""),

		new("get_vram_usage",
			"Video memory: the process's dedicated and shared usage and budget (DXGI, includes Unity / HDRP), Aether's own tracked share by category (textures, buffers, render targets, shadow maps, reflection, history), what Unity reports for its render textures / textures / buffers, and the part that is not Aether.",
			NoInput,
			"""{"type":"object","properties":{"process":{"type":"object","description":"dedicatedUsageBytes, dedicatedBudgetBytes, sharedUsageBytes, sharedBudgetBytes"},"adapterDedicatedBytes":{"type":"integer"},"aether":{"type":"object","description":"gpuBytes, systemMemoryBytes, textureBytes, bufferBytes, renderTargetBytes, shadowMapBytes, reflectionBytes, historyBytes, resourceCount, categories, allocations, releases"},"unity":{"type":"object","description":"renderTexturesBytes, usedTexturesBytes, usedBuffersBytes, textureStreamingCurrentBytes, ..."},"notAether":{"type":"object","description":"dedicatedBytes: process usage minus Aether's GPU resources"}},"required":["process","aether"]}"""),

		new("get_resource_list",
			"GPU resources Aether created (name, category, bytes, dimensions, format, heap, age), largest first, with allocation / release counters and recent events. Optionally Unity's render targets too (the HDRP side), largest first.",
			"""{"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":2000,"description":"Default 200."},"category":{"type":"string","enum":["texture","buffer","renderTarget","depthBuffer","shadowMap","reflection","history","readback","upload","other"]},"includeUnityRenderTextures":{"type":"boolean","description":"Also list Unity's RenderTextures (walks them in the game: a short hitch). Default false."}},"additionalProperties":false}""",
			"""{"type":"object","properties":{"owner":{"type":"string"},"count":{"type":"integer"},"returned":{"type":"integer"},"resources":{"type":"array","items":{"type":"object","properties":{"id":{"type":"integer"},"name":{"type":"string"},"category":{"type":"string"},"bytes":{"type":"integer"},"width":{"type":"integer"},"height":{"type":"integer"},"dxgiFormat":{"type":"integer"},"heap":{"type":"string"},"ageSeconds":{"type":"number"}}}},"summary":{"type":"object"},"unityRenderTextures":{"type":"object"}},"required":["count","resources"]}"""),

		new("get_render_graph",
			"The current render graph: passes in execution order with what they read and write and last frame's timings, and the resources involved (the host's back buffer with its entry / exit state, Aether's resources).",
			NoInput,
			"""{"type":"object","properties":{"schema":{"type":"string"},"phase":{"type":"integer"},"frame":{"type":"integer"},"renderer":{"type":"object"},"passes":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"type":{"type":"string"},"owner":{"type":"string"},"reads":{"type":"array"},"writes":{"type":"array"},"lastFrame":{"type":"object"}}}},"resources":{"type":"array","items":{"type":"object"}}},"required":["passes","resources"]}"""),

		new("get_shader_errors",
			"Shaders that failed to compile, with the compiler's output (file, line, message). Optionally every shader with its status, compile time and warnings.",
			"""{"type":"object","properties":{"includeAllShaders":{"type":"boolean","description":"Default false."}},"additionalProperties":false}""",
			"""{"type":"object","properties":{"errorCount":{"type":"integer"},"shadersWithWarnings":{"type":"integer"},"shaderCount":{"type":"integer"},"errors":{"type":"array","items":{"type":"object","properties":{"key":{"type":"string"},"file":{"type":"string"},"entry":{"type":"string"},"target":{"type":"string"},"compilerOutput":{"type":"string"}}}},"all":{"type":"object"}},"required":["errorCount","errors"]}"""),

		new("get_material_info",
			"The scene's materials grouped by shader: material and renderer counts, render queue range, transparent / alpha-test counts, shader keywords, sample material names. Walks every renderer in the game (a short hitch) unless refresh is false.",
			"""{"type":"object","properties":{"refresh":{"type":"boolean","description":"Collect now (default true). False returns the last collection."},"detail":{"type":"string","enum":["summary","survey","aether"],"description":"summary (default): materials grouped by shader. survey: every shader's property layout with one material's values, mesh vertex layouts, lights in HDRP units, environment. aether: what Aether's material translation produced (class, flags, bound textures, unmapped properties; needs render mode aether or compare)."},"limit":{"type":"integer","minimum":1,"maximum":600,"description":"detail=aether: how many materials (default 200)."}},"additionalProperties":false}""",
			"""{"type":"object","properties":{"available":{"type":"boolean"},"ageSeconds":{"type":"number"},"refreshed":{"type":"boolean"},"data":{"type":"object","description":"scene, rendererCount, materialCount, shaderCount, shaders[{shader, materials, rendererSlots, renderQueueMin, renderQueueMax, transparentMaterials, alphaTestMaterials, textureProperties, keywords[], sampleMaterials[]}]"}},"required":["available"]}"""),

		new("get_scene_info",
			"The loaded scenes, cameras (with HDRP camera settings), lights by type, renderer counts, reflection probes, HDRP volumes and the overrides they hold, UI canvases, environment; plus the game's Unity / graphics / render pipeline settings.",
			NoInput,
			"""{"type":"object","properties":{"available":{"type":"boolean"},"ageSeconds":{"type":"number"},"refreshed":{"type":"boolean"},"data":{"type":"object","description":"activeScene, loadedScenes[], cameras[], lights{}, renderers{}, reflectionProbes[], volumes[], canvases[], environment{}"},"game":{"type":["object","null"]}},"required":["available"]}"""),

		new("get_logs",
			"Aether's structured log (timestamp, severity, component, error code, message, resource, details). Use afterSeq with the returned lastSeq to read only what is new.",
			"""{"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":1000,"description":"Default 100 (the newest)."},"minSeverity":{"type":"string","enum":["debug","info","warning","error","fatal"],"description":"Default info."},"afterSeq":{"type":"integer","minimum":0},"component":{"type":"string","description":"Exact component name, e.g. Renderer, Shader, Ipc, Overlay, Unity."}},"additionalProperties":false}""",
			"""{"type":"object","properties":{"lastSeq":{"type":"integer"},"errorCount":{"type":"integer"},"count":{"type":"integer"},"entries":{"type":"array","items":{"type":"object","properties":{"seq":{"type":"integer"},"timestamp":{"type":"string"},"severity":{"type":"string"},"component":{"type":"string"},"code":{"type":"string"},"message":{"type":"string"},"resource":{"type":"string"},"extra":{"type":"object"}}}},"logDirectory":{"type":"string"}},"required":["lastSeq","entries"]}"""),

		new("get_crash_report",
			"What went wrong: the current session's failure report (device removed / hung with DRED data when available, a fault inside Aether, the failing HRESULT, GPU, settings, log tail), the previous session's report, and whether the previous session ended without a clean shutdown.",
			NoInput,
			"""{"type":"object","properties":{"currentSession":{"type":["object","null"]},"previousSession":{"type":["object","null"]},"previousSessionEndedUncleanly":{"type":"boolean"},"previousSessionLogTail":{"type":"array"},"rendererState":{"type":"object"},"deviceState":{"type":"string"}},"required":["previousSessionEndedUncleanly"]}"""),

		new("get_settings",
			"Aether's settings with their schema (type, allowed values, range, description), the render feature switches and the quality presets.",
			NoInput,
			"""{"type":"object","properties":{"values":{"type":"object"},"schema":{"type":"object"},"renderFeatures":{"type":"array","items":{"type":"string"}},"qualityPresets":{"type":"array","items":{"type":"string"}}},"required":["values","schema"]}"""),

		new("get_benchmark",
			"The benchmark recorder's state: idle, running (elapsed, frames) or done with the full result of the last run.",
			NoInput,
			"""{"type":"object","properties":{"state":{"type":"string","enum":["idle","running","done"]},"result":{"type":"object"}},"required":["state"]}"""),

		// ---------------------------------------------------------------- control
		new("set_render_quality",
			"[control] Sets the render quality preset. Needs the server's --allow-control and the game's permission (the player may be asked in game).",
			"""{"type":"object","properties":{"quality":{"type":"string","enum":["low","medium","high","ultra"]}},"required":["quality"],"additionalProperties":false}""",
			"""{"type":"object","properties":{"quality":{"type":"string"},"note":{"type":"string"}},"required":["quality"]}""",
			Control: true, TimeoutSeconds: ControlTimeout),

		new("set_render_feature",
			"[control] Switches a render feature on or off (get_settings lists the features). Needs --allow-control and the game's permission.",
			"""{"type":"object","properties":{"feature":{"type":"string","description":"One of get_settings.renderFeatures, e.g. testTriangle, gpuTiming."},"enabled":{"type":"boolean"}},"required":["feature","enabled"],"additionalProperties":false}""",
			"""{"type":"object","properties":{"feature":{"type":"string"},"enabled":{"type":"boolean"}},"required":["feature","enabled"]}""",
			Control: true, TimeoutSeconds: ControlTimeout),

		new("set_render_mode",
			"[control] Chooses who draws the scene: hdrp (the game's renderer), aether (Aether's forward+ renderer replaces the picture, the game's UI stays on top), compare (left half HDRP, right half Aether). Optionally a debug view of Aether's renderer. Needs --allow-control and the game's permission.",
			"""{"type":"object","properties":{"mode":{"type":"string","enum":["hdrp","aether","compare"]},"debugView":{"type":"string","enum":["none","albedo","normal","roughness","metallic","depth","lighting","shadow","materialClass"]}},"additionalProperties":false}""",
			"""{"type":"object","properties":{"mode":{"type":"string"},"debugView":{"type":"string"},"note":{"type":"string"}},"required":["mode","debugView"]}""",
			Control: true, TimeoutSeconds: ControlTimeout),

		new("reload_shader",
			"[control] Recompiles Aether's shaders from the files on disk and rebuilds its pipelines; returns the compiler's output for every shader. Needs --allow-control and the game's permission.",
			NoInput,
			"""{"type":"object","properties":{"reloaded":{"type":"boolean"},"errorCount":{"type":"integer"},"errors":{"type":"array"},"all":{"type":"object"}},"required":["errorCount"]}""",
			Control: true, TimeoutSeconds: ControlTimeout),

		new("capture_frame",
			"[control] Saves the next frame's back buffer as a PNG in kino/mods/Aether/captures and returns the file path (optionally the image itself). Needs --allow-control and the game's permission.",
			"""{"type":"object","properties":{"withOverlay":{"type":"boolean","description":"Include Aether's overlay in the picture. Default true."},"includeImage":{"type":"boolean","description":"Also return the PNG as image content (can be several MB). Default false."}},"additionalProperties":false}""",
			"""{"type":"object","properties":{"id":{"type":"integer"},"state":{"type":"string","enum":["pending","encoding","done"]},"file":{"type":"string"},"width":{"type":"integer"},"height":{"type":"integer"}},"required":["state","file"]}""",
			Control: true, TimeoutSeconds: ControlTimeout, Handler: CaptureFrame),

		new("capture_render_graph",
			"[control] Writes the current render graph (passes, resources, timings) to a JSON file in kino/mods/Aether/captures and returns it. Needs --allow-control and the game's permission.",
			NoInput,
			"""{"type":"object","properties":{"file":{"type":"string"},"graph":{"type":"object"}},"required":["file","graph"]}""",
			Control: true, TimeoutSeconds: ControlTimeout),

		new("run_benchmark",
			"[control] Records frame times, GPU time, VRAM and draw calls for the given time in the current scene, writes the result as JSON to kino/mods/Aether/benchmarks and returns it (average FPS, 1% low, frame time percentiles, VRAM start / end / peak, resource statistics, render graph). Needs --allow-control and the game's permission.",
			"""{"type":"object","properties":{"seconds":{"type":"integer","minimum":1,"maximum":600,"description":"Default 10."},"label":{"type":"string","maxLength":64,"description":"Name of the run (file name prefix). Default mcp."},"wait":{"type":"boolean","description":"Wait for the result (default true). False returns at once; poll get_benchmark."}},"additionalProperties":false}""",
			"""{"type":"object","properties":{"state":{"type":"string","enum":["running","done"]},"result":{"type":"object","description":"schema aether.benchmark/1: label, frames, averageFps, onePercentLowFps, pointOnePercentLowFps, frameTimeMs, gpuFrameTimeMs, dedicatedVramBytes{start,end,peak}, aetherResources, unityAtEnd, context, file"}},"required":["state"]}""",
			Control: true, TimeoutSeconds: ControlTimeout, Handler: RunBenchmark),
	];

	private static async Task<JsonNode?> CaptureFrame(RuntimeClient client, JsonObject arguments, CancellationToken cancel) {
		bool includeImage = arguments["includeImage"]?.GetValue<bool>() ?? false;
		var parameters = new JsonObject();
		if (arguments["withOverlay"] is JsonNode overlay) {
			parameters["withOverlay"] = overlay.DeepClone();
		}
		JsonNode? status = await client.Call("capture_frame", parameters, true, TimeSpan.FromSeconds(ControlTimeout), cancel);
		// the runtime answers "encoding" when the PNG was not written within its own wait: ask a few more times
		for (int i = 0; i < 40 && status?["state"]?.GetValue<string>() is "pending" or "encoding"; ++i) {
			await Task.Delay(250, cancel);
			status = await client.Call("get_capture", new JsonObject { ["id"] = status!["id"]!.DeepClone() }, false, TimeSpan.FromSeconds(10), cancel);
		}
		if (status?["state"]?.GetValue<string>() == "failed") {
			throw new RuntimeException("E_CAPTURE_FAILED", status["error"]?.GetValue<string>() ?? "capture failed");
		}
		if (includeImage && status is JsonObject obj && obj["state"]?.GetValue<string>() == "done" && obj["file"]?.GetValue<string>() is string file && File.Exists(file)) {
			obj["__imagePath"] = file; // picked up by the server to attach the image content
		}
		return status;
	}

	private static async Task<JsonNode?> RunBenchmark(RuntimeClient client, JsonObject arguments, CancellationToken cancel) {
		bool wait = arguments["wait"]?.GetValue<bool>() ?? true;
		int seconds = arguments["seconds"]?.GetValue<int>() ?? 10;
		var parameters = new JsonObject { ["seconds"] = seconds };
		if (arguments["label"] is JsonNode label) {
			parameters["label"] = label.DeepClone();
		}
		await client.Call("run_benchmark", parameters, true, TimeSpan.FromSeconds(ControlTimeout), cancel);
		if (!wait) {
			return new JsonObject { ["state"] = "running" };
		}
		DateTime deadline = DateTime.UtcNow.AddSeconds(seconds + 30);
		while (true) {
			await Task.Delay(500, cancel);
			JsonNode? status = await client.Call("get_benchmark", null, false, TimeSpan.FromSeconds(10), cancel);
			if (status?["state"]?.GetValue<string>() == "done") {
				return status;
			}
			if (DateTime.UtcNow > deadline) {
				throw new RuntimeException("E_TIMEOUT", "The benchmark did not finish in time (is the game paused or minimized? Frames are needed for it to advance).");
			}
		}
	}
}
