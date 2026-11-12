using System;
using UnityEngine;

namespace Aether {
	/// <summary>
	/// Unity's quality values that Aether's own graphics options set (the overlay's Settings tab): LOD bias, anisotropic
	/// filtering, vertical sync.
	///
	/// The game sets the same values from its graphics menu, through volume components of its own (HDRP.Scalability) that
	/// a handler reads from the HDRP camera's volume stack every frame (HDRP.MasterVolumes.OnBeginCameraRendering). With
	/// Aether as the render pipeline there is no HDRP camera and that handler does nothing. The game's menu is not read
	/// by Aether either: the options live in Aether's settings, and HDRP is not coming back.
	/// </summary>
	internal static class HostQualitySettings {
		private static float nextRead_;
		private static float lodBias_ = -1;
		private static int anisotropic_ = -1, vsync_ = -1;
		private static string logged_;

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
