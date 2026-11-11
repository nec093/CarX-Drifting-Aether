using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace Aether {
	/// <summary>The C interface of AetherNative.dll (src/Aether.Native/include/AetherApi.h).</summary>
	internal static class Native {
		public const int ApiVersion = 2;
		public const string Dll = "AetherNative";

		public const int EventFrameBegin = 1;
		public const int EventFrameEnd = 2;
		public const int EventShutdown = 3;
		public const int EventScene = 4;

		public const int StateNotInitialized = 0;
		public const int StateWaiting = 1;
		public const int StateReady = 2;
		public const int StateFailed = 3;

		// ManagedRequest (Runtime.h)
		public const uint RequestScene = 1;
		public const uint RequestMaterials = 2;
		public const uint RequestRenderTextures = 4;
		public const uint RequestSurvey = 8;

		[DllImport(Dll)] public static extern int Aether_GetApiVersion();
		[DllImport(Dll, CharSet = CharSet.Unicode)] public static extern int Aether_Initialize(string dataDir, string configDir);
		[DllImport(Dll)] public static extern void Aether_Shutdown();
		[DllImport(Dll)] public static extern int Aether_HasUnityInterfaces();
		[DllImport(Dll)] public static extern int Aether_HasD3D12();
		[DllImport(Dll)] public static extern int Aether_GetState();
		[DllImport(Dll)] public static extern IntPtr Aether_GetRenderEventFunc();
		[DllImport(Dll)] public static extern void Aether_Update();
		[DllImport(Dll)] public static extern unsafe void Aether_SubmitUnityStats(double* values, int count);
		[DllImport(Dll)] public static extern uint Aether_TakeManagedRequests();
		[DllImport(Dll)] private static extern void Aether_SetManagedState(byte[] key, byte[] jsonUtf8);
		[DllImport(Dll)] private static extern void Aether_Log(int severity, byte[] component, byte[] messageUtf8);
		[DllImport(Dll)] public static extern int Aether_OverlayVisible();
		[DllImport(Dll)] public static extern void Aether_SetOverlayVisible(int visible);
		[DllImport(Dll)] public static extern int Aether_OverlayWantsInput();

		// the scene data contract (SceneCapture/Contract.cs)
		[DllImport(Dll)] public static extern int Aether_SceneContractVersion();
		[DllImport(Dll)] public static extern int Aether_SceneStructSize(int which);
		[DllImport(Dll)] public static extern unsafe int Aether_SceneUpsertMesh(AeMeshDesc* desc, AeSubMesh* subMeshes);
		[DllImport(Dll)] public static extern unsafe int Aether_SceneUpsertTexture(AeTextureDesc* desc);
		[DllImport(Dll)] public static extern unsafe int Aether_SceneUpsertMaterial(AeMaterialDesc* desc);
		[DllImport(Dll)] public static extern unsafe int Aether_SceneUpsertInstance(AeInstanceDesc* desc, uint* materials);
		[DllImport(Dll)] public static extern void Aether_SceneDestroy(int kind, uint handle);
		[DllImport(Dll)] public static extern void Aether_SceneClear();
		[DllImport(Dll)] public static extern unsafe int Aether_SceneSubmitFrame(AeFrame* frame, AeFrameInstance* instances, uint instanceCount, AeLight* lights, uint lightCount);
		[DllImport(Dll)] public static extern int Aether_GetRenderMode();
		[DllImport(Dll)] public static extern int Aether_DevCaptureFlags();

		[DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr LoadLibraryW(string path);

		private static byte[] Utf8(string s) {
			return Encoding.UTF8.GetBytes((s ?? "") + "\0");
		}

		public static void SetManagedState(string key, string json) {
			Aether_SetManagedState(Utf8(key), Utf8(json));
		}

		public static void Log(int severity, string message) {
			Aether_Log(severity, Utf8("Managed"), Utf8(message));
		}

		/// <summary>How the native DLL got into the process (reported in the "managed" state).</summary>
		public static string LoadStrategy { get; private set; } = "none";

		/// <summary>
		/// Makes sure Unity itself loads AetherNative.dll, so that Unity calls UnityPluginLoad on it (the only way to
		/// IUnityGraphicsD3D12v7). Unity's plugin loader looks in [Game]_Data/Plugins/x86_64: the DLL shipped in
		/// kino/mods/Aether/native is copied there when it is missing or different. Must run before the first P/Invoke.
		/// With the file kino/config/Aether/dev-preload present the DLL is loaded from kino/mods/Aether/native with LoadLibrary
		/// instead (a development experiment: tells whether Unity also calls UnityPluginLoad then).
		/// Returns null on success, else what went wrong.
		/// </summary>
		public static string Prepare(string modDataDir, string configDir, string gameRoot) {
			// in a sub-folder: KSL examines every .dll directly inside kino/mods/<folder> as a possible mod
			string source = Path.Combine(modDataDir, "native", Dll + ".dll");
			if (!File.Exists(source)) {
				return "missing " + source;
			}
			if (File.Exists(Path.Combine(configDir, "dev-preload"))) {
				if (LoadLibraryW(source) == IntPtr.Zero) {
					return "LoadLibrary failed for " + source + " (error " + Marshal.GetLastWin32Error() + ")";
				}
				LoadStrategy = "preload (LoadLibrary from kino/mods/Aether/native)";
				return null;
			}
			string dataFolder = null;
			foreach (string dir in Directory.GetDirectories(gameRoot, "*_Data")) {
				if (Directory.Exists(Path.Combine(dir, "Managed"))) {
					dataFolder = dir;
					break;
				}
			}
			if (dataFolder == null) {
				return "the game's _Data folder was not found under " + gameRoot;
			}
			string pluginDir = Path.Combine(dataFolder, "Plugins", "x86_64");
			string target = Path.Combine(pluginDir, Dll + ".dll");
			try {
				var s = new FileInfo(source);
				var t = new FileInfo(target);
				if (!t.Exists || t.Length != s.Length || t.LastWriteTimeUtc != s.LastWriteTimeUtc) {
					Directory.CreateDirectory(pluginDir);
					File.Copy(source, target, true);
					File.SetLastWriteTimeUtc(target, s.LastWriteTimeUtc);
					LoadStrategy = "Unity plugin folder (copied this run)";
				} else {
					LoadStrategy = "Unity plugin folder";
				}
			} catch (Exception e) {
				return "could not copy " + Dll + ".dll to " + pluginDir + ": " + e.Message;
			}
			return null;
		}
	}

	/// <summary>Order of the values in Aether_SubmitUnityStats (enum UnityStat in Profiler/Profiler.h).</summary>
	internal enum UnityStat {
		DeltaTimeMs = 0,
		CpuMainThreadMs,
		CpuRenderThreadMs,
		CpuPresentWaitMs,
		GpuFrameMs,
		DrawCalls,
		Batches,
		SetPassCalls,
		Triangles,
		Vertices,
		RenderTexturesCount,
		RenderTexturesBytes,
		UsedTexturesCount,
		UsedTexturesBytes,
		UsedBuffersCount,
		UsedBuffersBytes,
		TextureStreamingCurrentBytes,
		TextureTotalBytes,
		TextureTargetBytes,
		GfxDriverBytes,
		TotalAllocatedBytes,
		ScreenWidth,
		ScreenHeight,
		ShadowCasters,
		VisibleSkinnedMeshes,
		Count
	}
}
