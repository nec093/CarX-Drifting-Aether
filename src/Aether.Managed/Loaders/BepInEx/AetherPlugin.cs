using System.Collections.Generic;
using System.IO;
using BepInEx;

namespace Aether {
	/// <summary>
	/// Entry point under BepInEx 5 (Mono). Aether's files live in BepInEx\plugins\Aether (this DLL, native\, shaders\,
	/// the HDRP bundle), its settings in BepInEx\config\Aether. No signing, no server.
	/// </summary>
	[BepInPlugin("nec093.aether", "Aether", AetherCore.Version)]
	public class AetherPlugin : BaseUnityPlugin, IAetherHost {
		public string LoaderName => "BepInEx " + typeof(BaseUnityPlugin).Assembly.GetName().Version;
		public string DataDir => Path.Combine(Paths.PluginPath, "Aether");
		public string ConfigDir => Path.Combine(Paths.ConfigPath, "Aether");
		public string GameRoot => Paths.GameRootPath;
		public void LogInfo(string message) { Logger.LogInfo(message); }
		public void LogError(string message) { Logger.LogError(message); }
		public void SetSuppressInput(bool suppress) { }
		public IEnumerable<object> LoadedMods() { yield break; }

		private void Awake() {
			AetherCore.Create(this);
		}
	}
}
