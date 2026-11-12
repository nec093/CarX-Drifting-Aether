using System;
using System.Reflection;

namespace Aether {
	/// <summary>
	/// Keeps the game from acting on the mouse / keyboard while Aether's overlay uses them.
	///
	/// Why the window procedure alone is not enough: the game reads its input through Rewired (NavigationInputModule
	/// takes the pointer and the click from ReInput.controllers.Mouse), and Rewired reads raw input on its own, not from
	/// the window messages Aether's window procedure swallows. A click on the overlay therefore also reached the menu
	/// entry behind it. Rewired's own switch (Controller.enabled: a disabled controller returns no input) is used, found
	/// by reflection so Aether has no reference to the game's assemblies; where Rewired is not there, nothing happens.
	/// </summary>
	internal static class GameInputBlock {
		private static bool resolved_;
		private static PropertyInfo ready_, controllers_, mouse_, keyboard_, enabled_;
		private static int applied_; // bit 0: the mouse is off, bit 1: the keyboard is off

		/// <summary>flags: bit 0 the overlay has the mouse, bit 1 the keyboard (Aether_OverlayInputFlags). Main thread.</summary>
		public static void Apply(int flags) {
			flags &= 3;
			if (flags == applied_ || !Resolve()) {
				return;
			}
			try {
				if (!(bool)ready_.GetValue(null)) {
					return; // Rewired is not up yet (or is being torn down): tried again next frame
				}
				object helper = controllers_.GetValue(null);
				if (helper == null) {
					return;
				}
				Set(mouse_.GetValue(helper), (flags & 1) == 0);
				Set(keyboard_.GetValue(helper), (flags & 2) == 0);
				applied_ = flags;
			} catch (Exception e) {
				Native.Log(2, "Game input: could not switch the game's mouse / keyboard: " + (e.InnerException ?? e).Message);
				ready_ = null; // not tried again
			}
		}

		private static void Set(object controller, bool enabled) {
			if (controller != null) {
				enabled_.SetValue(controller, enabled);
			}
		}

		private static bool Resolve() {
			if (!resolved_) {
				resolved_ = true;
				try {
					Type reInput = Type.GetType("Rewired.ReInput, Rewired_Core");
					Type controller = Type.GetType("Rewired.Controller, Rewired_Core");
					if (reInput != null && controller != null) {
						PropertyInfo ready = reInput.GetProperty("isReady", BindingFlags.Public | BindingFlags.Static);
						controllers_ = reInput.GetProperty("controllers", BindingFlags.Public | BindingFlags.Static);
						if (controllers_ != null) {
							mouse_ = controllers_.PropertyType.GetProperty("Mouse", BindingFlags.Public | BindingFlags.Instance);
							keyboard_ = controllers_.PropertyType.GetProperty("Keyboard", BindingFlags.Public | BindingFlags.Instance);
						}
						enabled_ = controller.GetProperty("enabled", BindingFlags.Public | BindingFlags.Instance);
						if (ready != null && mouse_ != null && keyboard_ != null && enabled_ != null && enabled_.CanWrite) {
							ready_ = ready;
						}
					}
					Native.Log(1, ready_ != null ? "Game input: Rewired found; its mouse / keyboard are switched off while the overlay uses them" : "Game input: Rewired not found; the overlay only filters window messages");
				} catch (Exception e) {
					Native.Log(2, "Game input: " + e.Message);
				}
			}
			return ready_ != null;
		}
	}
}
