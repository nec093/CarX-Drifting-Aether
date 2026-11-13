using System;
using System.Collections.Generic;
using UnityEngine;

namespace Aether {
	/// <summary>
	/// The free camera: switched on and off by its hotkey (F11 by default) in the game window, where Aether's window
	/// procedure also gathers its input (Overlay::HandleFreeCamera) and keeps it from the game. While it is on, the
	/// game's screen camera is put where the free camera is, right before the frame is culled and drawn (after the
	/// game's own camera scripts, like DevCamera). It starts where the game's camera was.
	/// WASD / arrows move along the view, E / Space and Q / C up and down, the mouse looks round, the wheel changes the
	/// field of view, R puts it back, Shift is faster, Ctrl slower.
	/// The game's UI is hidden while it is on: every root canvas that is switched on is switched off (and the ones that
	/// appear later, looked for twice a second), and the same ones are switched on again when it goes off.
	/// </summary>
	internal static class FreeCamera {
		private const int KeyForward = 1 << 8, KeyLeft = 1 << 9, KeyBack = 1 << 10, KeyRight = 1 << 11, KeyDown = 1 << 12, KeyUp = 1 << 13, KeyFast = 1 << 14, KeySlow = 1 << 15, KeyResetFov = 1 << 16;
		private static readonly float[] input_ = new float[5];
		private static bool active_;
		private static Vector3 position_;
		private static float yaw_, pitch_, fov_, startFov_;
		private static bool failed_;
		private static readonly List<Canvas> hidden_ = new List<Canvas>();
		private static float nextUiSweep_;
		private static bool cursorWasVisible_ = true;

		/// <summary>Main thread, once per screen frame (the game's screen camera, or null).</summary>
		public static void Apply(Camera camera) {
			if (camera == null || failed_) {
				return;
			}
			int flags;
			try {
				flags = Native.Aether_FreeCamera(active_ ? fov_ : camera.fieldOfView, input_);
			} catch (Exception e) {
				failed_ = true;
				Native.Log(2, "Free camera: " + e.Message);
				return;
			}
			if ((flags & 1) == 0) {
				if (active_) {
					ShowUi();
					Cursor.visible = cursorWasVisible_;
				}
				active_ = false;
				return;
			}
			if (Time.unscaledTime >= nextUiSweep_ || !active_) {
				nextUiSweep_ = Time.unscaledTime + 0.5f;
				HideUi();
			}
			Transform t = camera.transform;
			if (!active_) {
				active_ = true;
				cursorWasVisible_ = Cursor.visible;
				Cursor.visible = false; // (the cursor is held in the window's middle for looking round)
				position_ = t.position;
				Vector3 e = t.eulerAngles;
				yaw_ = e.y;
				pitch_ = e.x > 180.0f ? e.x - 360.0f : e.x;
				fov_ = startFov_ = camera.fieldOfView;
			}
			if ((flags & 2) != 0) {
				Cursor.visible = false; // (every frame: the game's menus show it again)
				float sensitivity = input_[4];
				yaw_ = Mathf.Repeat(yaw_ + input_[0] * sensitivity, 360.0f);
				pitch_ = Mathf.Clamp(pitch_ + input_[1] * sensitivity, -89.0f, 89.0f); // (mouse down = look down)
				if (input_[2] != 0.0f) {
					fov_ = Mathf.Clamp(fov_ * Mathf.Pow(0.9f, input_[2]), 5.0f, 120.0f); // (wheel up = narrower)
				}
				if ((flags & KeyResetFov) != 0) {
					fov_ = startFov_;
				}
				float dt = Mathf.Min(Time.unscaledDeltaTime, 0.1f);
				float speed = input_[3] * ((flags & KeyFast) != 0 ? 5.0f : 1.0f) * ((flags & KeySlow) != 0 ? 0.2f : 1.0f);
				Quaternion rotation = Quaternion.Euler(pitch_, yaw_, 0.0f);
				Vector3 move = Vector3.zero;
				if ((flags & KeyForward) != 0) move += rotation * Vector3.forward;
				if ((flags & KeyBack) != 0) move -= rotation * Vector3.forward;
				if ((flags & KeyRight) != 0) move += rotation * Vector3.right;
				if ((flags & KeyLeft) != 0) move -= rotation * Vector3.right;
				if ((flags & KeyUp) != 0) move += Vector3.up;
				if ((flags & KeyDown) != 0) move -= Vector3.up;
				if (move.sqrMagnitude > 1e-6f) {
					position_ += move.normalized * (speed * dt);
				}
			}
			t.SetPositionAndRotation(position_, Quaternion.Euler(pitch_, yaw_, 0.0f));
			camera.fieldOfView = fov_;
		}

		private static void HideUi() {
			try {
				Canvas[] canvases = UnityEngine.Object.FindObjectsByType<Canvas>(FindObjectsSortMode.None);
				for (int i = 0; i < canvases.Length; ++i) {
					Canvas c = canvases[i];
					if (c != null && c.enabled && c.isRootCanvas) {
						c.enabled = false;
						hidden_.Add(c);
					}
				}
			} catch (Exception e) {
				Native.Log(2, "Free camera: could not hide the game's UI: " + e.Message);
			}
		}

		private static void ShowUi() {
			for (int i = 0; i < hidden_.Count; ++i) {
				try {
					if (hidden_[i] != null) {
						hidden_[i].enabled = true;
					}
				} catch (Exception) {
				}
			}
			hidden_.Clear();
		}
	}
}
