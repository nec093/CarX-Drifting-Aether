using System;
using System.Collections.Generic;
using System.IO;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.HighDefinition;

namespace Aether {
	/// <summary>
	/// The game's own render features that cannot run on Direct3D 12 and break HDRP's frame when they try.
	///
	/// Found in this project's verification (2026-11-11, Player.log, after HDRP itself was fixed): the game's HDRP
	/// custom pass GpuCulling.GpuCullingPass throws "ArgumentException: Kernel 'CalculateCullResult' not found" every
	/// frame (its compute shader has no DX12 kernels either). The exception is raised inside HDRP's render graph, the
	/// rest of the frame is skipped, and the window keeps showing the last picture (the loading screen) at ~17 fps.
	/// The shader is the game's own (no source), so it cannot be rebuilt like HDRP's: on DX12 the pass is switched off
	/// (CustomPass.enabled), which lets the frame complete. What that pass would have drawn or culled is then missing
	/// or unculled: a known limitation while HDRP is the renderer.
	/// Opt out with the file kino/config/Aether/keep-game-passes.
	/// </summary>
	internal static class GameDx12Workarounds {
		// Custom pass types (full names) seen failing on DX12 in this game build (2.28.3): the GPU culling pass
		// ('CalculateCullResult') and the particle pass ('BlurMS'). Others are found at run time (OnLog).
		private static readonly HashSet<string> BrokenPasses = new HashSet<string> { "GpuCulling.GpuCullingPass", "ParticleRenderPass" };
		private static readonly HashSet<string> disabled_ = new HashSet<string>();
		private static bool off_;
		private static volatile bool found_;

		public static string Status { get; private set; } = "nothing switched off";

		/// <summary>True once after a new failing pass type was recognised in Unity's log (the caller then runs Apply).</summary>
		public static bool TakeFound() {
			if (found_) {
				found_ = false;
				return true;
			}
			return false;
		}

		public static void Init(string configDir) {
			off_ = SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D12 || File.Exists(Path.Combine(configDir, "keep-game-passes"));
			if (off_) {
				Status = "not applied";
				return;
			}
			Application.logMessageReceived += OnLog;
		}

		public static void Shutdown() {
			Application.logMessageReceived -= OnLog;
		}

		// A custom pass of the game that throws "Kernel '...' not found" from inside HDRP's custom pass execution names
		// itself in the stack trace: the frame right above CustomPass.ExecuteInternal is "<pass type>.<method> (".
		private static void OnLog(string condition, string stackTrace, LogType type) {
			if (type != LogType.Exception || condition == null || stackTrace == null || condition.IndexOf("Kernel '", StringComparison.Ordinal) < 0 || condition.IndexOf("not found", StringComparison.Ordinal) < 0) {
				return;
			}
			int execute = stackTrace.IndexOf("HighDefinition.CustomPass+", StringComparison.Ordinal);
			if (execute < 0) {
				return;
			}
			int lineEnd = stackTrace.LastIndexOf('\n', execute);
			if (lineEnd <= 0) {
				return;
			}
			int lineStart = stackTrace.LastIndexOf('\n', lineEnd - 1) + 1;
			string frame = stackTrace.Substring(lineStart, lineEnd - lineStart).Trim(); // "at Ns.Type.Method (args) ..."
			if (frame.StartsWith("at ")) {
				frame = frame.Substring(3);
			}
			int paren = frame.IndexOf(" (", StringComparison.Ordinal);
			int dot = paren > 0 ? frame.LastIndexOf('.', paren) : -1;
			if (dot <= 0) {
				return;
			}
			if (BrokenPasses.Add(frame.Substring(0, dot))) {
				found_ = true;
			}
		}

		// ---- video playback on Direct3D 12
		// docs/architecture/dx12-integration.md: the game's advertising SDK (Anzu) plays its video adverts with a
		// Unity VideoPlayer. On DX12 (which neither the game nor Anzu's native renderer - D3D9 / D3D11 / OpenGL only -
		// is made for), when the clip changes Unity uploads a video frame (A8_UNORM, 1080x608) to a plane texture that
		// was replaced meanwhile: an invalid copy that the debug layer reports and that loses the device or crashes the
		// player. On DX12 the video players are therefore switched off. Image adverts and the rest of Anzu are left alone.
		//   keep-game-videos   (file in Aether's config folder) switches nothing off
		//   disable-game-ads   additionally disables Anzu's own components (what the investigation's runs I - K used)
		private static bool videosOff_, adsOff_;
		private static float nextVideoCheck_;
		private static readonly HashSet<string> videoTypes_ = new HashSet<string>();
		private static int videosDisabled_;

		public static string VideoStatus { get; private set; } = "not applied";

		public static void InitVideos(string configDir) {
			bool dx12 = SystemInfo.graphicsDeviceType == GraphicsDeviceType.Direct3D12;
			videosOff_ = dx12 && !File.Exists(Path.Combine(configDir, "keep-game-videos"));
			adsOff_ = videosOff_ && File.Exists(Path.Combine(configDir, "disable-game-ads"));
			VideoStatus = videosOff_ ? "nothing switched off yet" : "not applied";
		}

		/// <summary>Every two seconds: enabled video players (and, if asked for, Anzu's components) get disabled.</summary>
		public static void UpdateVideos() {
			if (!videosOff_ || Time.unscaledTime < nextVideoCheck_) {
				return;
			}
			nextVideoCheck_ = Time.unscaledTime + 2.0f;
			try {
				int count = 0;
				foreach (Behaviour b in UnityEngine.Object.FindObjectsByType<Behaviour>(FindObjectsInactive.Exclude, FindObjectsSortMode.None)) {
					if (b == null || !b.enabled) {
						continue;
					}
					string type = b.GetType().FullName ?? "";
					if (type == "UnityEngine.Video.VideoPlayer" || (adsOff_ && type.IndexOf("Anzu", StringComparison.OrdinalIgnoreCase) >= 0)) {
						b.enabled = false;
						videoTypes_.Add(type);
						++count;
					}
				}
				if (count > 0) {
					videosDisabled_ += count;
					VideoStatus = "switched off " + videosDisabled_ + " components: " + string.Join(", ", videoTypes_);
					Native.Log(2, "Video playback on DX12: " + VideoStatus);
				}
			} catch (Exception e) {
				VideoStatus = "failed: " + e.GetType().Name + ": " + e.Message;
				videosOff_ = false;
			}
		}
		/// <summary>Looks for the broken passes in the loaded scenes (cheap enough for a few calls after a scene load).</summary>
		public static int Apply() {
			if (off_) {
				return 0;
			}
			int count = 0;
			try {
				foreach (CustomPassVolume volume in UnityEngine.Object.FindObjectsByType<CustomPassVolume>(FindObjectsInactive.Include, FindObjectsSortMode.None)) {
					foreach (CustomPass pass in volume.customPasses) {
						if (pass != null && pass.enabled && BrokenPasses.Contains(pass.GetType().FullName)) {
							pass.enabled = false;
							++count;
							disabled_.Add(pass.GetType().FullName + " on " + volume.name + " (" + volume.injectionPoint + ")");
						}
					}
				}
			} catch (Exception e) {
				Status = "failed: " + e.GetType().Name + ": " + e.Message;
				return 0;
			}
			if (count > 0) {
				Status = "switched off (no Direct3D 12 kernels): " + string.Join("; ", disabled_);
			}
			return count;
		}
	}
}
