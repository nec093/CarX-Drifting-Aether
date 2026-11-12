using System.Collections.Generic;

namespace Aether {
	/// <summary>
	/// What Aether's core needs from whatever loaded it. One implementation per mod loader (Loaders/); the core itself
	/// references none of them.
	/// </summary>
	public interface IAetherHost {
		/// <summary>Name and version of the loader, for the diagnostics.</summary>
		string LoaderName { get; }
		/// <summary>Where Aether's own files are: native\AetherNative.dll, shaders\, the HDRP bundle; logs and captures are written here.</summary>
		string DataDir { get; }
		/// <summary>Where aether.json and the opt-in / opt-out marker files live.</summary>
		string ConfigDir { get; }
		/// <summary>The game's folder (the one with the executable).</summary>
		string GameRoot { get; }
		void LogInfo(string message);
		void LogError(string message);
		/// <summary>Keeps the loader's own hotkeys quiet while Aether's overlay has the keyboard. May do nothing.</summary>
		void SetSuppressInput(bool suppress);
		/// <summary>The other mods the loader has loaded (objects with Name / Version / Author properties), for the diagnostics. May be empty.</summary>
		IEnumerable<object> LoadedMods();
	}
}
