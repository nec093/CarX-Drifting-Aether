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

		// The game's own day / night switch (DayNightController: it loads a lighting scene for the day or for the
		// night). Found by reflection: Aether does not link against the game's assembly.
		private static bool nightLookedUp_;
		private static System.Reflection.PropertyInfo nightInstance_, nightApplied_, nightIsDay_;
		private static int night_ = -1;

		// The other way round: when Aether's time of day setting passes into the night or out of it, the game's own
		// night lighting (the map's lamps, the cars' lights) is switched with it. Only on such a change, so the game's
		// own button keeps working in between, and only when the game says its night mode is installed (it is a DLC:
		// Aether does not open what the player does not own).
		private static int timeNight_ = -1;
		private static System.Reflection.MethodInfo nightSet_;
		private static System.Reflection.PropertyInfo nightInstalled_;

		private static void PushTimeToGame(int sceneNight) {
			try {
				double hour = Native.Aether_SettingNumber("timeOfDay", 14.0);
				int timeNight = hour >= 19.7 || hour < 4.3 ? 1 : 0;
				bool changed = timeNight_ >= 0 && timeNight != timeNight_;
				timeNight_ = timeNight;
				if (!changed || timeNight == sceneNight || Native.Aether_SettingNumber("followSceneNight", 1) < 0.5 || nightInstance_ == null) {
					return;
				}
				if (nightSet_ == null) {
					nightSet_ = nightInstance_.DeclaringType.GetMethod("SetDayNightMode", new[] { typeof(bool) });
					Type manager = Type.GetType("GameManager, Assembly-CSharp");
					nightInstalled_ = manager != null ? manager.GetProperty("NightModeInstalled", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static) : null;
				}
				object controller = nightInstance_.GetValue(null, null);
				if (nightSet_ == null || nightInstalled_ == null || controller == null || controller.Equals(null)) {
					return;
				}
				// In the garage the game's own menu has a day / night button that anyone may press (no DLC asked there):
				// that very handler is called, so the button's label, the saved preference and the car's lamps follow.
				Type menuType = Type.GetType("UIMainMenuContext, Assembly-CSharp");
				UnityEngine.Object menu = menuType != null ? UnityEngine.Object.FindObjectOfType(menuType) : null;
				if (menu != null) {
					System.Reflection.BindingFlags hidden = System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Public;
					System.Reflection.FieldInfo active = menuType.GetField("m_nightModeActive", hidden);
					System.Reflection.MethodInfo toggle = menuType.GetMethod("ToggleDayNightMode", hidden);
					if (active != null && toggle != null) {
						if ((bool)active.GetValue(menu) != (timeNight != 0)) {
							toggle.Invoke(menu, null);
							Native.Log(1, "Day / night: the time of day setting pressed the garage's " + (timeNight != 0 ? "night" : "day") + " button");
						}
						return;
					}
				}
				// Elsewhere (on a track) night is the game's DLC.
				if (!(bool)nightInstalled_.GetValue(null, null)) {
					Native.Log(1, "Day / night: the game's night mode is not installed; the map's lights stay as they are");
					return;
				}
				nightSet_.Invoke(controller, new object[] { timeNight == 0 });
				Native.Log(1, "Day / night: the time of day setting switched the game's lighting to " + (timeNight != 0 ? "night" : "day"));
			} catch (Exception e) {
				Native.Log(2, "Day / night: " + e.Message);
			}
		}

		private static void ReadSceneNight() {
			int night = 0;
			try {
				if (!nightLookedUp_) {
					nightLookedUp_ = true;
					Type type = Type.GetType("DayNightController, Assembly-CSharp");
					if (type != null) {
						nightInstance_ = type.GetProperty("instance", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
						nightApplied_ = type.GetProperty("IsAppliedMode");
						nightIsDay_ = type.GetProperty("IsDayLight");
					}
					Native.Log(1, nightInstance_ != null && nightApplied_ != null && nightIsDay_ != null ? "Day / night: following the game's DayNightController" : "Day / night: the game's DayNightController was not found (the sky keeps its own time)");
				}
				if (nightInstance_ != null && nightApplied_ != null && nightIsDay_ != null) {
					object controller = nightInstance_.GetValue(null, null);
					if (controller != null && !controller.Equals(null) && (bool)nightApplied_.GetValue(controller, null)) {
						night = (bool)nightIsDay_.GetValue(controller, null) ? 0 : 1;
					}
				}
			} catch (Exception) {
				night = 0;
			}
			PushTimeToGame(night);
			if (night != night_) {
				night_ = night;
				Native.Aether_SetSceneNight(night);
				Native.Log(1, "Day / night: the scene is at " + (night != 0 ? "night" : "day"));
			}
		}

		/// <summary>Applies Aether's options (read from the native settings twice a second). Main thread.</summary>
		public static void Apply() {
			if (Time.unscaledTime < nextRead_) {
				return;
			}
			nextRead_ = Time.unscaledTime + 0.5f;
			ReadSceneNight();
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
