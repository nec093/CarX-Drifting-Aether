using System;
using System.Collections;
using System.IO;
using KSL.API;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.SceneManagement;

namespace Aether {
	/// <summary>
	/// The KSL entry point. It gets the native runtime (AetherNative.dll) loaded by Unity, drives it with the frame's
	/// render events, and publishes what only the managed side can see (Unity's statistics, the scene, the materials).
	/// Rendering, profiling, the overlay and the MCP connection all live in the native runtime.
	/// </summary>
	[KSLMeta("Aether", "0.1.0", "nec093")]
	public class AetherMod : BaseMod {
		private bool running_;
		private string status_ = "not started";
		private IntPtr renderEvent_;
		private Coroutine endOfFrame_;
		private readonly UnityStats stats_ = new UnityStats();
		private readonly SceneReporter reporter_ = new SceneReporter();
		private bool suppressingInput_;
		private float nextManagedPublish_;
		private bool shutDown_;

		private void Start() {
			try {
				Launch();
			} catch (Exception e) {
				// a missing or mismatched DLL must never take the game (or KSL) down
				status_ = "failed to start: " + e.GetType().Name + ": " + e.Message;
				Kino.Log.Error("[Aether] " + status_);
				running_ = false;
			}
		}

		private void Launch() {
			if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D12) {
				status_ = "Aether needs DirectX 12 (the game runs on " + SystemInfo.graphicsDeviceType + "). Add -force-d3d12 to the Steam launch options.";
				Kino.Log.Error("[Aether] " + status_);
				return;
			}
			string dataDir = Path.Combine(Kino.Paths.Mods, "Aether");
			string configDir = Path.Combine(Kino.Paths.Config, "Aether");
			Directory.CreateDirectory(configDir);
			// first of all: without it the game's own renderer does not start on DX12 and the window stays black
			HdrpDx12Fix.Apply(dataDir, configDir);
			GameDx12Workarounds.Init(configDir);
			ApplyWorkarounds();
			string problem = Native.Prepare(dataDir, configDir, Kino.Paths.GameRoot);
			if (problem != null) {
				status_ = "native runtime not available: " + problem;
				Kino.Log.Error("[Aether] " + status_);
				return;
			}
			int version = Native.Aether_GetApiVersion(); // the first P/Invoke: Unity loads the DLL and calls UnityPluginLoad
			if (version != Native.ApiVersion) {
				status_ = "AetherNative.dll has API version " + version + ", this mod needs " + Native.ApiVersion + ": install both files of the same release";
				Kino.Log.Error("[Aether] " + status_);
				return;
			}
			if (Native.Aether_Initialize(dataDir, configDir) == 0) {
				status_ = "the native runtime refused to start";
				Kino.Log.Error("[Aether] " + status_);
				return;
			}
			bool interfaces = Native.Aether_HasUnityInterfaces() != 0, d3d12 = Native.Aether_HasD3D12() != 0;
			Kino.Log.Info("[Aether] native runtime loaded (" + Native.LoadStrategy + "), Unity interfaces: " + interfaces + ", D3D12 interface: " + d3d12);
			Native.Log(1, "Managed mod started under " + Kino.Loader.Name + " (" + Native.LoadStrategy + ")");
			Native.Log(HdrpDx12Fix.Failed ? 3 : 1, "HDRP DX12 fix: " + HdrpDx12Fix.Status);
			PublishManaged();
			if (!interfaces || !d3d12) {
				status_ = interfaces ? "Unity did not hand out its D3D12 interface (see kino/mods/Aether/logs/aether.jsonl)"
					: "Unity did not call UnityPluginLoad on AetherNative.dll (" + Native.LoadStrategy + "): no access to the D3D12 device";
				Kino.Log.Error("[Aether] " + status_);
				// stay loaded: the log and the MCP connection still work for diagnosis, nothing is rendered
				running_ = true;
				return;
			}
			renderEvent_ = Native.Aether_GetRenderEventFunc();
			endOfFrame_ = StartCoroutine(EndOfFrameLoop());
			SceneManager.sceneLoaded += OnSceneLoaded;
			SceneManager.sceneUnloaded += OnSceneUnloaded;
			running_ = true;
			status_ = "running";
		}

		// the game's passes that break the frame on DX12 appear with their scenes: look now and a few more times
		private float workaroundUntil_;
		private float nextWorkaround_;

		private void ApplyWorkarounds() {
			workaroundUntil_ = Time.unscaledTime + 15.0f;
			nextWorkaround_ = 0;
		}

		private void OnSceneLoaded(Scene scene, LoadSceneMode mode) {
			ApplyWorkarounds();
			if (running_) {
				Native.Log(1, "Scene loaded: " + scene.name + " (" + mode + ")");
				reporter_.SceneChanged();
			}
		}

		private void OnSceneUnloaded(Scene scene) {
			if (running_) {
				Native.Log(1, "Scene unloaded: " + scene.name);
				reporter_.SceneChanged();
			}
		}

		private IEnumerator EndOfFrameLoop() {
			var wait = new WaitForEndOfFrame();
			while (true) {
				yield return wait;
				if (running_ && renderEvent_ != IntPtr.Zero) {
					// after every camera and the UI, before Present: Aether's passes go on top of the finished frame
					GL.IssuePluginEvent(renderEvent_, Native.EventFrameEnd);
				}
			}
		}

		private void Update() {
			if (GameDx12Workarounds.TakeFound()) {
				ApplyWorkarounds(); // a pass type not seen before failed for want of a DX12 kernel: look for it now
			}
			if (Time.unscaledTime < workaroundUntil_ && Time.unscaledTime >= nextWorkaround_) {
				nextWorkaround_ = Time.unscaledTime + 0.5f;
				if (GameDx12Workarounds.Apply() > 0) {
					Kino.Log.Info("[Aether] game passes: " + GameDx12Workarounds.Status);
					if (running_) {
						Native.Log(2, "Game passes: " + GameDx12Workarounds.Status);
					}
				}
			}
			if (!running_) {
				return;
			}
			if (renderEvent_ != IntPtr.Zero) {
				GL.IssuePluginEvent(renderEvent_, Native.EventFrameBegin);
			}
			Native.Aether_Update();
			stats_.Submit();

			bool wantsInput = Native.Aether_OverlayWantsInput() != 0;
			if (wantsInput != suppressingInput_) {
				suppressingInput_ = wantsInput;
				Kino.Input.SetSuppressInput(wantsInput); // KSL's own hotkeys stay quiet while the overlay has the keyboard
			}

			uint requests = Native.Aether_TakeManagedRequests();
			if (reporter_.TakeSceneDirty()) {
				requests |= Native.RequestScene;
				nextManagedPublish_ = 0;
			}
			if (requests != 0) {
				reporter_.Publish(requests);
			}
			if (Time.unscaledTime >= nextManagedPublish_) {
				nextManagedPublish_ = Time.unscaledTime + 10.0f;
				PublishManaged();
			}
		}

		private void PublishManaged() {
			try {
				Native.SetManagedState("managed", reporter_.GameInfo(status_));
			} catch (Exception e) {
				Kino.Log.Error("[Aether] could not publish the game info: " + e.Message);
			}
		}

		private void Shutdown() {
			if (shutDown_) {
				return;
			}
			shutDown_ = true;
			SceneManager.sceneLoaded -= OnSceneLoaded;
			SceneManager.sceneUnloaded -= OnSceneUnloaded;
			GameDx12Workarounds.Shutdown();
			if (!running_) {
				return;
			}
			running_ = false;
			if (endOfFrame_ != null) {
				StopCoroutine(endOfFrame_);
			}
			if (suppressingInput_) {
				Kino.Input.SetSuppressInput(false);
			}
			stats_.Dispose();
			try {
				if (renderEvent_ != IntPtr.Zero) {
					// the render thread frees Aether's device objects; Aether_Shutdown waits for it (briefly)
					GL.IssuePluginEvent(renderEvent_, Native.EventShutdown);
				}
				Native.Aether_Shutdown();
			} catch (Exception e) {
				Kino.Log.Error("[Aether] shutdown: " + e.Message);
			}
			Kino.Log.Info("[Aether] stopped");
		}

		private void OnApplicationQuit() {
			Shutdown();
		}

		private void OnDestroy() {
			Shutdown();
		}

		public override void OnUIDraw() {
			GUILayout.Label("Aether " + Meta.Version + ": " + status_);
			GUILayout.Label("HDRP on DX12: " + HdrpDx12Fix.Status);
			GUILayout.Label("Game passes on DX12: " + GameDx12Workarounds.Status);
			if (!running_) {
				return;
			}
			int state = Native.Aether_GetState();
			GUILayout.Label("Renderer: " + (state == Native.StateReady ? "ready" : state == Native.StateFailed ? "stopped after an error" : "waiting for the first frame"));
			bool visible = Native.Aether_OverlayVisible() != 0;
			if (GUILayout.Button(visible ? "Hide the overlay (F10)" : "Show the overlay (F10)")) {
				Native.Aether_SetOverlayVisible(visible ? 0 : 1);
			}
			GUILayout.Label("Logs, captures and benchmarks: kino/mods/Aether");
		}
	}
}
