using System;
using System.Reflection;
using UnityEngine.Rendering.HighDefinition;

namespace Aether {
	/// <summary>
	/// The game switches renderer features per camera ("frame settings": a camera's own values where it overrides them,
	/// the project's defaults elsewhere). They say which of the scene's volume settings the game means to be seen
	/// (tone mapping, bloom, ...). Read as plain data from the camera's component and the project's settings asset.
	/// </summary>
	internal struct HostFrameSettings {
		private static bool defaultsRead_;
		private static bool defaultsKnown_;
		private static FrameSettings defaults_;

		private readonly bool custom_;
		private readonly FrameSettings settings_;
		private readonly FrameSettingsOverrideMask mask_;

		public HostFrameSettings(HDAdditionalCameraData camera) {
			custom_ = camera != null && camera.customRenderingSettings;
			settings_ = custom_ ? camera.renderingPathCustomFrameSettings : default;
			mask_ = custom_ ? camera.renderingPathCustomFrameSettingsOverrideMask : default;
			if (!defaultsRead_) {
				defaultsRead_ = true;
				try {
					// the project's defaults for cameras: a private field of the global settings asset
					UnityEngine.Object global = UnityEngine.Rendering.GraphicsSettings.GetSettingsForRenderPipeline<HDRenderPipeline>();
					object found = FindValue(global, typeof(FrameSettings), "Camera", 2);
					if (found != null) {
						defaults_ = (FrameSettings)found;
						defaultsKnown_ = true;
					}
				} catch (Exception) {
				}
			}
		}

		public bool DefaultsKnown => defaultsKnown_;

		/// <summary>A field of `type` whose name contains `nameHas`, in `owner` or (depth) in the objects its fields hold.</summary>
		internal static object FindValue(object owner, Type type, string nameHas, int depth) {
			if (owner == null) {
				return null;
			}
			const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
			for (Type t = owner.GetType(); t != null && t != typeof(object) && t != typeof(UnityEngine.Object); t = t.BaseType) {
				foreach (FieldInfo f in t.GetFields(flags | BindingFlags.DeclaredOnly)) {
					if (f.FieldType == type && f.Name.IndexOf(nameHas, StringComparison.OrdinalIgnoreCase) >= 0) {
						object value = f.GetValue(owner);
						if (value != null && !(value is UnityEngine.Object o && o == null)) {
							return value;
						}
					}
				}
			}
			if (depth > 1) {
				for (Type t = owner.GetType(); t != null && t != typeof(object) && t != typeof(UnityEngine.Object); t = t.BaseType) {
					foreach (FieldInfo f in t.GetFields(flags | BindingFlags.DeclaredOnly)) {
						Type ft = f.FieldType;
						if (ft.IsPrimitive || ft.IsEnum || ft == typeof(string) || ft.IsArray || typeof(UnityEngine.Object).IsAssignableFrom(ft) || ft.Namespace == null || !ft.Namespace.StartsWith("UnityEngine.Rendering")) {
							continue;
						}
						object found = FindValue(f.GetValue(owner), type, nameHas, depth - 1);
						if (found != null) {
							return found;
						}
					}
				}
			}
			return null;
		}

		internal static T FindField<T>(object owner, string nameHas) where T : class {
			return FindValue(owner, typeof(T), nameHas, 2) as T;
		}

		public bool IsEnabled(FrameSettingsField field) {
			if (custom_ && mask_.mask[(uint)field]) {
				return settings_.IsEnabled(field);
			}
			return !defaultsKnown_ || defaults_.IsEnabled(field);
		}
	}
}
