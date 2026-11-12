using System.Collections.Generic;
using System.IO;
using KSL.API;
using UnityEngine;

namespace Aether {
	/// <summary>Entry point under KSL: starts the loader-independent core on its own game object and shows its status in KSL's panel.</summary>
	[KSLMeta("Aether", AetherCore.Version, "nec093")]
	public class AetherMod : BaseMod, IAetherHost {
		private AetherCore core_;

		public string LoaderName => Kino.Loader.Name;
		public string DataDir => Path.Combine(Kino.Paths.Mods, "Aether");
		public string ConfigDir => Path.Combine(Kino.Paths.Config, "Aether");
		public string GameRoot => Kino.Paths.GameRoot;
		public void LogInfo(string message) { Kino.Log.Info(message); }
		public void LogError(string message) { Kino.Log.Error(message); }
		public void SetSuppressInput(bool suppress) { Kino.Input.SetSuppressInput(suppress); }

		public IEnumerable<object> LoadedMods() {
			foreach (object mod in Kino.Loader.GetLoadedMods()) {
				yield return mod;
			}
		}

		private void Start() {
			core_ = AetherCore.Create(this);
		}

		public override void OnUIDraw() {
			if (core_ != null) {
				core_.DrawStatus();
			}
		}
	}
}
