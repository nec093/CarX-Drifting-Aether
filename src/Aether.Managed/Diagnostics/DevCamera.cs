using System;
using System.Globalization;
using System.IO;
using UnityEngine;

namespace Aether {
	/// <summary>
	/// A development aid: holds the game's screen camera at a fixed place so that two pictures (two render modes, two
	/// builds) show the same thing. The file dev-camera.txt in the config folder holds "px py pz tx ty tz [fov]"
	/// (position, the point looked at, optional field of view); no file = the game's own camera.
	/// Applied right before the frame is culled and drawn, after the game's own camera scripts.
	/// </summary>
	internal static class DevCamera {
		private static string file_;
		private static float nextRead_;
		private static bool active_;
		private static Vector3 position_, target_;
		private static float fov_;

		public static void Init(string configDir) {
			file_ = Path.Combine(configDir, "dev-camera.txt");
		}

		public static void Apply(Camera camera) {
			if (file_ == null || camera == null) {
				return;
			}
			if (Time.unscaledTime >= nextRead_) {
				nextRead_ = Time.unscaledTime + 1.0f;
				active_ = false;
				try {
					if (File.Exists(file_)) {
						string[] p = File.ReadAllText(file_).Split(new[] { ' ', ',', '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
						if (p.Length >= 6) {
							float[] v = new float[7];
							for (int i = 0; i < 7 && i < p.Length; ++i) {
								v[i] = float.Parse(p[i], CultureInfo.InvariantCulture);
							}
							position_ = new Vector3(v[0], v[1], v[2]);
							target_ = new Vector3(v[3], v[4], v[5]);
							fov_ = p.Length >= 7 ? v[6] : 0;
							active_ = true;
						}
					}
				} catch (Exception) {
				}
			}
			if (active_) {
				camera.transform.position = position_;
				camera.transform.LookAt(target_);
				if (fov_ > 1) {
					camera.fieldOfView = fov_;
				}
			}
		}
	}
}
