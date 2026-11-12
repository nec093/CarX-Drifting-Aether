using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;

namespace Aether {
	/// <summary>
	/// Aether as the game's render pipeline. While this asset is the active one, HDRP does not exist: Unity disposes the
	/// HDRenderPipeline instance (and with it every render target, buffer and shader variant HDRP owns) when the asset
	/// is replaced, and calls this pipeline once per frame instead.
	/// </summary>
	public sealed class AetherPipelineAsset : RenderPipelineAsset {
		protected override RenderPipeline CreatePipeline() {
			return new AetherPipeline();
		}
	}

	/// <summary>
	/// What is left for Unity to do per camera: cull (which keeps Renderer.isVisible, the LOD choice and GPU skinning
	/// alive), make the camera's target current, and draw the game's own UI canvases on top. The scene itself is Aether's:
	/// for the screen camera one plugin event goes into the context's command stream and the native renderer draws.
	/// </summary>
	public sealed class AetherPipeline : RenderPipeline {
		private static readonly ShaderTagId kUnlitPass = new ShaderTagId("SRPDefaultUnlit");
		private const int kUiLayer = 5;
		private readonly CommandBuffer target_ = new CommandBuffer { name = "Aether target" };
		private readonly List<Camera> list_ = new List<Camera>();

		public static int Frames { get; private set; }

		protected override void Render(ScriptableRenderContext context, Camera[] cameras) {
			list_.Clear();
			list_.AddRange(cameras);
			Render(context, list_);
		}

		// main thread time of the pipeline (milliseconds per frame, averaged over the last second or so)
		private static long cullTicks_, renderTicks_;
		private static int timedFrames_;
		public static string Timing { get; private set; } = "";

		protected override void Render(ScriptableRenderContext context, List<Camera> cameras) {
			long renderStart = System.Diagnostics.Stopwatch.GetTimestamp();
			try {
				RenderCore(context, cameras);
			} finally {
				renderTicks_ += System.Diagnostics.Stopwatch.GetTimestamp() - renderStart;
				if (++timedFrames_ >= 60) {
					double toMs = 1000.0 / System.Diagnostics.Stopwatch.Frequency / timedFrames_;
					Timing = "render " + (renderTicks_ * toMs).ToString("F2") + " (cull " + (cullTicks_ * toMs).ToString("F2") + ")";
					cullTicks_ = renderTicks_ = 0;
					timedFrames_ = 0;
				}
			}
		}

		private void RenderCore(ScriptableRenderContext context, List<Camera> cameras) {
			PipelineSwitch.EnsureVolumes();
			BeginContextRendering(context, cameras);
			AetherCore core = AetherCore.Instance;
			Camera main = AetherCore.PickScreenCamera(cameras);
			bool mainDone = false;
			for (int i = 0; i < cameras.Count; ++i) {
				Camera camera = cameras[i];
				if (camera == null) {
					continue;
				}
				BeginCameraRendering(context, camera);
				if (camera.TryGetCullingParameters(out ScriptableCullingParameters parameters)) {
					// (no shadow caster culling by Unity: the capture finds the casters outside the view itself and the
					// native side culls them per cascade; asking Unity for them as well cost main thread time for nothing)
					long cullStart = System.Diagnostics.Stopwatch.GetTimestamp();
					CullingResults cull = context.Cull(ref parameters);
					cullTicks_ += System.Diagnostics.Stopwatch.GetTimestamp() - cullStart;
					context.SetupCameraProperties(camera);

					bool screen = camera.targetTexture == null;
					target_.Clear();
					target_.SetRenderTarget(BuiltinRenderTextureType.CameraTarget);
					// a camera stacked on top of the scene must not wipe it
					bool clearColor = (camera.clearFlags == CameraClearFlags.Skybox || camera.clearFlags == CameraClearFlags.SolidColor) && !(screen && mainDone);
					bool clearDepth = camera.clearFlags != CameraClearFlags.Nothing;
					if (clearColor || clearDepth) {
						target_.ClearRenderTarget(clearDepth, clearColor, camera == main ? Color.black : camera.backgroundColor);
					}
					context.ExecuteCommandBuffer(target_);

					if (camera == main && core != null) {
						core.DrawScene(context, camera);
						mainDone = true;
					}

					// the game's canvases that live in the world or in front of a camera (overlay canvases are Unity's own job)
					var sorting = new SortingSettings(camera) { criteria = SortingCriteria.CommonTransparent };
					var drawing = new DrawingSettings(kUnlitPass, sorting);
					var filtering = new FilteringSettings(RenderQueueRange.all, camera == main ? 1 << kUiLayer : camera.cullingMask);
					context.DrawRenderers(cull, ref drawing, ref filtering);
				}
				EndCameraRendering(context, camera);
			}
			context.Submit();
			++Frames;
			EndContextRendering(context, cameras);
			HostQualitySettings.Enforce(); // (the game's own handler wrote its menu's VSync and LOD bias while the cameras were rendered)
		}

		protected override void Dispose(bool disposing) {
			PipelineSwitch.ReleaseVolumes();
			target_.Dispose();
			base.Dispose(disposing);
		}
	}

	/// <summary>Puts Aether's pipeline asset in place of the game's (render modes aether / standalone) and back (hdrp, compare).</summary>
	internal static class PipelineSwitch {
		private static AetherPipelineAsset ours_;
		private static RenderPipelineAsset gameDefault_, gameQuality_;
		private static bool saved_;

		/// <summary>The game's own pipeline asset (its settings are still data Aether reads), also while Aether's is active.</summary>
		public static RenderPipelineAsset GameAsset => gameQuality_ != null ? gameQuality_ : gameDefault_;
		public static bool Own => ours_ != null && GraphicsSettings.currentRenderPipeline == ours_;
		public static string Status { get; private set; } = "the game's pipeline";

		// Unity's volume manager (it blends the scene's volumes: sky, exposure, grading, fog, ...) is started and stopped
		// by the active render pipeline, with the project's default profiles. Under Aether's pipeline that is Aether's job;
		// the profiles are the game's own, remembered while its pipeline still ran or read from its settings assets.
		private static VolumeProfile defaultProfile_, qualityProfile_;
		private static List<VolumeProfile> customProfiles_;
		private static bool volumesOurs_;
		/// <summary>Changes whenever the volume manager was started anew: stacks made before that are dead.</summary>
		public static int VolumeGeneration { get; private set; }

		private static void RememberVolumes() {
			try {
				VolumeManager manager = VolumeManager.instance;
				if (manager.isInitialized && !volumesOurs_) {
					defaultProfile_ = manager.globalDefaultProfile != null ? manager.globalDefaultProfile : defaultProfile_;
					qualityProfile_ = manager.qualityDefaultProfile != null ? manager.qualityDefaultProfile : qualityProfile_;
					if (manager.customDefaultProfiles != null) {
						customProfiles_ = new List<VolumeProfile>(manager.customDefaultProfiles);
					}
				}
				if (qualityProfile_ == null && GameAsset is UnityEngine.Rendering.HighDefinition.HDRenderPipelineAsset hd) {
					qualityProfile_ = hd.volumeProfile;
				}
				if (defaultProfile_ == null) {
					// (the game's pipeline never ran: the project's default profile is a field of its global settings asset)
					UnityEngine.Object global = GraphicsSettings.GetSettingsForRenderPipeline<UnityEngine.Rendering.HighDefinition.HDRenderPipeline>();
					defaultProfile_ = HostFrameSettings.FindField<VolumeProfile>(global, "Default") ?? HostFrameSettings.FindField<VolumeProfile>(global, "");
				}
			} catch (Exception e) {
				Native.Log(2, "Render pipeline: the game's default volume profiles could not be read: " + e.Message);
			}
		}

		public static void EnsureVolumes() {
			VolumeManager manager = VolumeManager.instance;
			if (manager.isInitialized) {
				return;
			}
			try {
				manager.Initialize(defaultProfile_, qualityProfile_);
				// The manager only loads the volume component types that declare support for the active pipeline's asset
				// type; the game's volumes are made of the components written for its own pipeline. Load those instead
				// (they are plain data: sky, exposure, tone mapping, fog, ...) and rebuild the default state from them.
				const System.Reflection.BindingFlags hidden = System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Public;
				Type type = typeof(VolumeManager);
				Type gameAsset = GameAsset != null ? GameAsset.GetType() : typeof(UnityEngine.Rendering.HighDefinition.HDRenderPipelineAsset);
				type.GetMethod("LoadBaseTypes", hidden).Invoke(manager, new object[] { gameAsset });
				type.GetMethod("EvaluateVolumeDefaultState", hidden).Invoke(manager, null);
				System.Reflection.FieldInfo defaultStack = type.GetField("m_DefaultStack", hidden);
				if (defaultStack != null) {
					// (the stack game scripts read through VolumeManager.instance.stack was made for the short list)
					VolumeStack stale = defaultStack.GetValue(manager) as VolumeStack;
					VolumeStack fresh = manager.CreateStack();
					defaultStack.SetValue(manager, fresh);
					manager.stack = fresh;
					if (stale != null) {
						manager.DestroyStack(stale);
					}
				}
				if (customProfiles_ != null && customProfiles_.Count > 0) {
					manager.SetCustomDefaultProfiles(customProfiles_);
				}
				volumesOurs_ = true;
				++VolumeGeneration;
				Native.Log(1, "Render pipeline: volume manager started by Aether (" + manager.baseComponentTypeArray.Length + " component types of " + gameAsset.Name + ", default profile " + (defaultProfile_ != null ? defaultProfile_.name : "none") +
					", quality profile " + (qualityProfile_ != null ? qualityProfile_.name : "none") + ", " + (customProfiles_ != null ? customProfiles_.Count : 0) + " custom)");
			} catch (Exception e) {
				Native.Log(3, "Render pipeline: the volume manager could not be started: " + e);
			}
		}

		public static void ReleaseVolumes() {
			if (!volumesOurs_) {
				return;
			}
			volumesOurs_ = false;
			++VolumeGeneration;
			try {
				if (VolumeManager.instance.isInitialized) {
					VolumeManager.instance.Deinitialize(); // the game's pipeline starts it again for itself
				}
			} catch (Exception) {
			}
		}

		public static void Apply(bool own) {
			RenderPipelineAsset current = GraphicsSettings.currentRenderPipeline;
			if (own) {
				if (ours_ == null) {
					ours_ = ScriptableObject.CreateInstance<AetherPipelineAsset>();
					ours_.name = "AetherPipelineAsset";
					ours_.hideFlags = HideFlags.HideAndDontSave;
				}
				if (current == ours_) {
					return;
				}
				// (also after the game changed its quality level: that level's own asset is remembered instead)
				gameDefault_ = GraphicsSettings.defaultRenderPipeline != ours_ ? GraphicsSettings.defaultRenderPipeline : gameDefault_;
				gameQuality_ = QualitySettings.renderPipeline != ours_ ? QualitySettings.renderPipeline : gameQuality_;
				saved_ = true;
				RememberVolumes();
				GraphicsSettings.defaultRenderPipeline = ours_;
				QualitySettings.renderPipeline = ours_;
				Status = "Aether's pipeline (the game's was " + (current != null ? current.GetType().Name + " " + current.name : "none") + ")";
				Native.Log(1, "Render pipeline: " + Status);
			} else if (saved_ && current == ours_) {
				try {
					// the game's pipeline keeps per camera state (with volume stacks) in a static cache: anything put there
					// while Aether's pipeline ran is dead once the volume manager restarts
					typeof(UnityEngine.Rendering.HighDefinition.HDCamera).GetMethod("ClearAll", System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Public)?.Invoke(null, null);
				} catch (Exception) {
				}
				GraphicsSettings.defaultRenderPipeline = gameDefault_;
				QualitySettings.renderPipeline = gameQuality_;
				Status = "the game's pipeline";
				Native.Log(1, "Render pipeline: the game's own pipeline is back (" + (gameQuality_ != null ? gameQuality_.name : gameDefault_ != null ? gameDefault_.name : "none") + ")");
			}
		}
	}
}
