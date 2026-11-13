using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.SceneManagement;

namespace Aether {
	/// <summary>
	/// Aether's managed core, independent of the mod loader that started it (Loaders/). It gets the native runtime (AetherNative.dll) loaded by Unity, drives it with the frame's
	/// render events, and publishes what only the managed side can see (Unity's statistics, the scene, the materials).
	/// Rendering, profiling, the overlay and the MCP connection all live in the native runtime.
	/// </summary>

	public class AetherCore : MonoBehaviour {
		public const string Version = "0.1.0";
		/// <summary>Set by the loader's entry point before this component is created.</summary>
		public static IAetherHost Host;

		private bool running_;
		private string status_ = "not started";
		private IntPtr renderEvent_;
		private Coroutine endOfFrame_;
		private readonly UnityStats stats_ = new UnityStats();
		private readonly SceneReporter reporter_ = new SceneReporter();
		private readonly SceneCapture capture_ = new SceneCapture();
		private string sceneContract_ = "not checked";
		private bool sceneContractOk_;
		private bool suppressingInput_;
		private float nextManagedPublish_;
		private bool shutDown_;
		public static AetherCore Instance { get; private set; }
		public float ShadowDistance => capture_.ShadowDistance;
		private bool dropScene_; // the capture stopped: one more scene event lets the native side release what it held

		/// <summary>Starts the core on a game object of its own that survives scene changes. Main thread.</summary>
		public static AetherCore Create(IAetherHost host) {
			Host = host;
			var go = new GameObject("Aether");
			UnityEngine.Object.DontDestroyOnLoad(go);
			go.hideFlags = HideFlags.HideAndDontSave;
			return go.AddComponent<AetherCore>();
		}

		private void Start() {
			Instance = this;
			try {
				Launch();
			} catch (Exception e) {
				// a missing or mismatched DLL must never take the game (or its mod loader) down
				status_ = "failed to start: " + e.GetType().Name + ": " + e.Message;
				Host.LogError("[Aether] " + status_);
				running_ = false;
			}
		}

		private void Launch() {
			if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D12) {
				status_ = "Aether needs DirectX 12 (the game runs on " + SystemInfo.graphicsDeviceType + "). Add -force-d3d12 to the Steam launch options.";
				Host.LogError("[Aether] " + status_);
				return;
			}
			string dataDir = Host.DataDir;
			string configDir = Host.ConfigDir;
			Directory.CreateDirectory(configDir);
			// (the game's own renderer does not start on DX12 without this fix; it is applied only when that renderer is
			// going to run: render modes hdrp and compare, or when Aether itself cannot start)
			HdrpDx12Fix.Prepare(dataDir, configDir);
			GameDx12Workarounds.Init(configDir);
			GameDx12Workarounds.InitVideos(configDir);
			DevCamera.Init(configDir);
			ParticleCapture.ConfigDir = configDir;
			ApplyWorkarounds();
			string problem = Native.Prepare(dataDir, configDir, Host.GameRoot);
			if (problem != null) {
				HdrpDx12Fix.Ensure();
				status_ = "native runtime not available: " + problem;
				Host.LogError("[Aether] " + status_);
				return;
			}
			int version = Native.Aether_GetApiVersion(); // the first P/Invoke: Unity loads the DLL and calls UnityPluginLoad
			if (version != Native.ApiVersion) {
				HdrpDx12Fix.Ensure();
				status_ = "AetherNative.dll has API version " + version + ", this mod needs " + Native.ApiVersion + ": install both files of the same release";
				Host.LogError("[Aether] " + status_);
				return;
			}
			if (Native.Aether_Initialize(dataDir, configDir) == 0) {
				HdrpDx12Fix.Ensure();
				status_ = "the native runtime refused to start";
				Host.LogError("[Aether] " + status_);
				return;
			}
			bool interfaces = Native.Aether_HasUnityInterfaces() != 0, d3d12 = Native.Aether_HasD3D12() != 0;
			Host.LogInfo("[Aether] native runtime loaded (" + Native.LoadStrategy + "), Unity interfaces: " + interfaces + ", D3D12 interface: " + d3d12);
			Native.Log(1, "Managed mod started under " + Host.LoaderName + " (" + Native.LoadStrategy + ")");
			PublishManaged();
			if (!interfaces || !d3d12) {
				HdrpDx12Fix.Ensure();
				status_ = interfaces ? "Unity did not hand out its D3D12 interface (see kino/mods/Aether/logs/aether.jsonl)"
					: "Unity did not call UnityPluginLoad on AetherNative.dll (" + Native.LoadStrategy + "): no access to the D3D12 device";
				Host.LogError("[Aether] " + status_);
				// stay loaded: the log and the MCP connection still work for diagnosis, nothing is rendered
				running_ = true;
				return;
			}
			renderEvent_ = Native.Aether_GetRenderEventFunc();
			endOfFrame_ = StartCoroutine(EndOfFrameLoop());
			// Aether's scene renderer: both sides must agree on the contract's layout before anything is sent
			string mismatch = SceneContract.Verify();
			sceneContractOk_ = mismatch == null;
			sceneContract_ = mismatch ?? "ok (version " + AeConst.ContractVersion + ")";
			if (!sceneContractOk_) {
				HdrpDx12Fix.Ensure();
				Host.LogError("[Aether] scene contract mismatch: " + mismatch);
				Native.Log(3, "Scene contract mismatch, the scene renderer stays off: " + mismatch);
			} else {
				// right now, before the first frame: in the Aether modes the game's own pipeline is never created
				ApplyPipeline(Native.Aether_GetRenderMode());
				RenderPipelineManager.endContextRendering += OnEndContextRendering;
			}
			RenderPipelineManager.beginContextRendering += OnBeginContextRendering;
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

		// After the game's render pipeline drew a set of cameras (still inside the render loop, before the overlay UI):
		// when that set holds the game's screen camera, this is where Aether's scene frame goes.
		private void OnBeginContextRendering(ScriptableRenderContext context, List<Camera> cameras) {
			Camera screen = PickScreenCamera(cameras);
			DevCamera.Apply(screen); // (does nothing without dev-camera.txt)
			FreeCamera.Apply(screen); // (does nothing while the free camera is off)
		}

		// (render mode compare only: the game's pipeline draws the frame, Aether's picture goes over half of it)
		private void OnEndContextRendering(ScriptableRenderContext context, List<Camera> cameras) {
			if (PipelineSwitch.Own) {
				return; // Aether's own pipeline calls DrawScene itself
			}
			Camera main = PickScreenCamera(cameras);
			if (main != null) {
				DrawScene(context, main);
			}
		}

		/// <summary>The game's camera that draws the screen: the one Aether's scene frame is for.</summary>
		public static Camera PickScreenCamera(List<Camera> cameras) {
			Camera main = null;
			for (int i = 0; i < cameras.Count; ++i) {
				Camera c = cameras[i];
				if (c != null && c.cameraType == CameraType.Game && c.targetTexture == null && c.isActiveAndEnabled && (main == null || c.depth > main.depth)) {
					main = c;
				}
			}
			return main; // null: only off-screen cameras (previews, icons)
		}

		/// <summary>Describes this frame of `main` to the native renderer and puts its draw into the context's command stream.</summary>
		public void DrawScene(ScriptableRenderContext context, Camera main) {
			if (!running_ || renderEvent_ == IntPtr.Zero || !sceneContractOk_) {
				return;
			}
			try {
				if (dropScene_) {
					dropScene_ = false;
					IssueSceneEvent(context); // lets the native side drop its handles
				}
				// the capture is started, stopped and kept up to date in Update; here only this frame is described
				if (capture_.Active && capture_.SubmitFrame(main)) {
					IssueSceneEvent(context);
				}
			} catch (Exception e) {
				// never let the capture break the game's render loop
				sceneContractOk_ = false;
				sceneContract_ = "stopped after " + e.GetType().Name + ": " + e.Message;
				Host.LogError("[Aether] scene capture stopped: " + e);
				Native.Log(3, "Scene capture stopped after an exception: " + e.GetType().Name + ": " + e.Message);
				try {
					capture_.Stop();
				} catch (Exception) {
				}
			}
		}

		private CommandBuffer sceneEventBuffer_;

		// Inside the render pipeline's loop the event goes through the pipeline's own command stream (a command buffer
		// executed and submitted on the context): it then runs on the render thread in order with everything the pipeline
		// recorded. GL.IssuePluginEvent bypasses that stream; it is kept behind an experiment switch for comparison.
		private void IssueSceneEvent(ScriptableRenderContext context) {
			if ((Native.Aether_DevCaptureFlags() & 8) != 0) {
				GL.IssuePluginEvent(renderEvent_, Native.EventScene);
				return;
			}
			if (sceneEventBuffer_ == null) {
				sceneEventBuffer_ = new CommandBuffer { name = "Aether scene" };
				sceneEventBuffer_.IssuePluginEvent(renderEvent_, Native.EventScene);
			}
			context.ExecuteCommandBuffer(sceneEventBuffer_);
			context.Submit();
		}

		private void OnSceneLoaded(Scene scene, LoadSceneMode mode) {
			ApplyWorkarounds();
			capture_.SceneChanged();
			if (running_) {
				Native.Log(1, "Scene loaded: " + scene.name + " (" + mode + ")");
				reporter_.SceneChanged();
			}
		}

		private void OnSceneUnloaded(Scene scene) {
			capture_.SceneChanged();
			if (running_) {
				Native.Log(1, "Scene unloaded: " + scene.name);
				reporter_.SceneChanged();
			}
		}

		private IEnumerator EndOfFrameLoop() {
			var wait = new WaitForEndOfFrame();
			while (true) {
				yield return wait;
				if (uncapped_) {
					UpdateFrameCap(); // after the game's own scripts ran this frame, before the present
				}
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
					Host.LogInfo("[Aether] game passes: " + GameDx12Workarounds.Status);
					if (running_) {
						Native.Log(2, "Game passes: " + GameDx12Workarounds.Status);
					}
				}
			}
			if (!running_) {
				return;
			}
			GameDx12Workarounds.UpdateVideos();
			if (renderEvent_ != IntPtr.Zero) {
				GL.IssuePluginEvent(renderEvent_, Native.EventFrameBegin);
			}
			Native.Aether_Update();
			stats_.Submit();
			UpdateFrameCap();
			UpdateCapture();

			int inputFlags = Native.Aether_OverlayInputFlags();
			GameInputBlock.Apply(inputFlags); // the game's own input (Rewired) does not see what the overlay uses
			bool wantsInput = inputFlags != 0;
			if (wantsInput != suppressingInput_) {
				suppressingInput_ = wantsInput;
				Host.SetSuppressInput(wantsInput); // the loader's own hotkeys stay quiet while the overlay has the keyboard
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

		// aether (1) and standalone (3) are the same thing: Aether is the render pipeline and the game's does not exist.
		// hdrp (0) and compare (2) run the game's own pipeline, for reference pictures and measurements.
		private static void ApplyPipeline(int mode) {
			bool own = mode == 1 || mode == 3;
			if (!own) {
				HdrpDx12Fix.Ensure();
			}
			PipelineSwitch.Apply(own);
		}

		// Outside the render loop: the scene capture's scans and native pointer queries.
		private void UpdateCapture() {
			if (!sceneContractOk_ || renderEvent_ == IntPtr.Zero) {
				return;
			}
			try {
				int mode = Native.Aether_GetRenderMode();
				ApplyPipeline(mode);
				if (mode == 0) {
					if (capture_.Active) {
						capture_.Stop();
						dropScene_ = true;
					}
					return;
				}
				capture_.Start();
				capture_.Tick();
			} catch (Exception e) {
				sceneContractOk_ = false;
				sceneContract_ = "stopped after " + e.GetType().Name + ": " + e.Message;
				Host.LogError("[Aether] scene capture stopped: " + e);
				Native.Log(3, "Scene capture stopped after an exception: " + e.GetType().Name + ": " + e.Message);
				try {
					capture_.Stop();
				} catch (Exception) {
				}
			}
		}

		// A benchmark can ask for the game's VSync and frame rate cap to be off while it records (the numbers are
		// otherwise pinned to the display's refresh rate). Both are put back when it ends.
		private bool uncapped_;
		private int capReasserted_;
		private int savedVSync_, savedTargetFrameRate_;

		private void UpdateFrameCap() {
			bool want = Native.Aether_BenchmarkWantsUncapped() != 0;
			if (want == uncapped_) {
				if (want && (QualitySettings.vSyncCount != 0 || Application.targetFrameRate != -1)) {
					// the game puts its own values back every frame: keep them off for the length of the run
					QualitySettings.vSyncCount = 0;
					Application.targetFrameRate = -1;
					++capReasserted_;
				}
				return;
			}
			uncapped_ = want;
			if (want) {
				savedVSync_ = QualitySettings.vSyncCount;
				savedTargetFrameRate_ = Application.targetFrameRate;
				QualitySettings.vSyncCount = 0;
				Application.targetFrameRate = -1;
				Native.Log(1, "Benchmark: VSync off and no frame rate cap for the run (was vSyncCount " + savedVSync_ + ", targetFrameRate " + savedTargetFrameRate_ + ")");
			} else {
				QualitySettings.vSyncCount = savedVSync_;
				Application.targetFrameRate = savedTargetFrameRate_;
				Native.Log(1, "Benchmark: VSync and frame rate cap restored (the game had re-applied its own values " + capReasserted_ + " times during the run)");
				capReasserted_ = 0;
			}
		}

		private void LateUpdate() {
			if (uncapped_) {
				UpdateFrameCap();
			}
		}

		private void PublishManaged() {
			try {
				Native.SetManagedState("managed", reporter_.GameInfo(status_, "contract " + sceneContract_ + (capture_.Active ? "; " + capture_.Status() : "; idle (render mode hdrp)")));
			} catch (Exception e) {
				Host.LogError("[Aether] could not publish the game info: " + e.Message);
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
			if (!quitting_) {
				// (the mod is taken out while the game goes on: the game gets its own pipeline back. Not when the game is
				// closing: Unity would start creating that pipeline for the last few frames.)
				ApplyPipeline(0);
			}
			RenderPipelineManager.endContextRendering -= OnEndContextRendering;
			RenderPipelineManager.beginContextRendering -= OnBeginContextRendering;
			if (!running_) {
				return;
			}
			running_ = false;
			if (endOfFrame_ != null) {
				StopCoroutine(endOfFrame_);
			}
			if (suppressingInput_) {
				Host.SetSuppressInput(false);
			}
			GameInputBlock.Apply(0);
			stats_.Dispose();
			try {
				capture_.Stop();
			} catch (Exception) {
			}
			try {
				if (renderEvent_ != IntPtr.Zero) {
					// the render thread frees Aether's device objects; Aether_Shutdown waits for it (briefly)
					GL.IssuePluginEvent(renderEvent_, Native.EventShutdown);
				}
				Native.Aether_Shutdown();
			} catch (Exception e) {
				Host.LogError("[Aether] shutdown: " + e.Message);
			}
			Host.LogInfo("[Aether] stopped");
		}

		private bool quitting_;

		private void OnApplicationQuit() {
			quitting_ = true;
			Shutdown();
		}

		private void OnDestroy() {
			Shutdown();
		}

		public void DrawStatus() {
			GUILayout.Label("Aether " + Version + ": " + status_);
			GUILayout.Label("HDRP on DX12: " + HdrpDx12Fix.Status);
			GUILayout.Label("Game passes on DX12: " + GameDx12Workarounds.Status);
			GUILayout.Label("Video playback on DX12: " + GameDx12Workarounds.VideoStatus);
			GUILayout.Label("Render pipeline: " + PipelineSwitch.Status);
			GUILayout.Label("Scene capture: " + (capture_.Active ? capture_.Status() : "idle (render mode hdrp)"));
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
