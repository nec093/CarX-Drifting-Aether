using System;
using UnityEngine;

namespace Aether {
	/// <summary>
	/// Unity's quality values that Aether's own graphics options set (the overlay's Settings tab): LOD bias, anisotropic
	/// filtering, vertical sync.
	///
	/// The game sets the same values from its graphics menu, through volume components of its own (HDRP.Scalability) that
	/// a handler reads from the camera's volume stack every frame (HDRP.MasterVolumes.OnBeginCameraRendering). That
	/// handler hangs on RenderPipelineManager.beginCameraRendering, which Aether's pipeline raises too: it still runs,
	/// and writes the game menu's LOD bias and VSync into QualitySettings in the middle of every frame's rendering.
	/// (Measured: with Aether's VSync off, the count was back at 1 on every one of 1959 frames, and the frame rate
	/// stayed at the display's.) The game's menu is not what decides (the options live in Aether's settings), so
	/// Aether's values are written again after the cameras were rendered, every frame: Enforce().
	/// </summary>
	internal static class HostQualitySettings {
		private static float nextRead_;
		private static float lodBias_ = -1;
		private static int anisotropic_ = -1, vsync_ = -1;
		private static string logged_;

		/// <summary>
		/// Writes Aether's LOD bias and VSync again (the values Apply read last). Called by the pipeline after every
		/// frame's cameras, that is after the game's handler wrote its own, and before the frame is presented.
		/// </summary>
		public static void Enforce() {
			if (vsync_ >= 0 && QualitySettings.vSyncCount != vsync_) {
				QualitySettings.vSyncCount = vsync_;
			}
			if (lodBias_ > 0 && Mathf.Abs(lodBias_ - QualitySettings.lodBias) > 1e-3f) {
				QualitySettings.lodBias = lodBias_;
			}
		}

		/// <summary>Applies Aether's options (read from the native settings twice a second). Main thread.</summary>
		public static void Apply() {
			if (Time.unscaledTime < nextRead_) {
				return;
			}
			nextRead_ = Time.unscaledTime + 0.5f;
			try {
				float lodBias = Mathf.Clamp((float)Native.Aether_SettingNumber("lodBias", 2.8), 0.3f, 8.0f);
				int anisotropic = Mathf.Clamp((int)Native.Aether_SettingNumber("anisotropicLevel", 9), 1, 16);
				int vsync = Mathf.Clamp((int)Native.Aether_SettingNumber("vsync", 1), 0, 2);
				if (Mathf.Abs(lodBias - QualitySettings.lodBias) > 1e-3f) {
					QualitySettings.lodBias = lodBias;
				}
				if (anisotropic != anisotropic_) {
					Texture.SetGlobalAnisotropicFilteringLimits(anisotropic, anisotropic);
				}
				if (vsync != QualitySettings.vSyncCount) {
					QualitySettings.vSyncCount = vsync;
				}
				lodBias_ = lodBias;
				anisotropic_ = anisotropic;
				vsync_ = vsync;
				string state = "LOD bias " + lodBias_.ToString("F2") + ", anisotropic level " + anisotropic_ + ", VSync " + vsync_;
				if (state != logged_) {
					logged_ = state;
					Native.Log(1, "Graphics options applied: " + state);
				}
			} catch (Exception e) {
				if (logged_ != e.Message) {
					logged_ = e.Message;
					Native.Log(2, "Graphics options: " + e.Message);
				}
			}
		}
	}
}
