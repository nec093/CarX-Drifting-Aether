using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.HighDefinition;

namespace Aether.Editor {
	/// <summary>
	/// Builds aether_hdrp_dx12.bundle: every compute shader HDRP's runtime resources reference, compiled for Direct3D 12,
	/// in the variant combination the game uses.
	///
	/// Why: the CarX build ships HDRP's compute shaders without DX12 kernels. Started with -force-d3d12 the game cannot
	/// construct its HDRP pipeline ("Kernel 'KDepthDownsample8DualUav' not found" every frame) and shows nothing. Until
	/// Aether draws the whole frame itself, HDRP has to work on DX12, so the mod swaps these copies in by name
	/// (src/Aether.Managed/GameIntegration/HdrpDx12Fix.cs). Same HDRP version as the game: 16.0.x for Unity 2023.2.
	///
	///   Unity.exe -batchmode -nographics -projectPath [project] -executeMethod Aether.Editor.BuildHdrpDx12Bundle.BatchBuild -aetherOut [dir] -quit
	/// </summary>
	public static class BuildHdrpDx12Bundle {
		private const string BundleName = "aether_hdrp_dx12.bundle";
		private const string RuntimeResources = "Packages/com.unity.render-pipelines.high-definition/Runtime/RenderPipelineResources/HDRenderPipelineRuntimeResources.asset";
		private const string MapPath = "Assets/Aether/Generated/aether_hdrp_compute_map.txt";
		// copied from the repository's unity/hdrp-game-settings.json by scripts/build-hdrp-bundle.ps1
		private const string GameSettingsPath = "Assets/Aether/hdrp-game-settings.json";

		// The game's HDRP asset settings HDRP's build stripper looks at (unity/hdrp-game-settings.json).
		[Serializable]
		private sealed class GameSettings {
			public string punctualShadowFilteringQuality = "Medium";
			public string directionalShadowFilteringQuality = "Medium";
			public string areaShadowFilteringQuality = "Medium";
			public bool supportScreenSpaceShadows;
			public string lightProbeSystem = "LegacyLightProbes";
			public string probeVolumeSHBands = "SphericalHarmonicsL1";
			public string litShaderMode = "Both";
			public string colorBufferFormat = "R11G11B10";
			public bool hdrDisplay;
		}

		public static void BatchBuild() {
			try {
				string[] args = Environment.GetCommandLineArgs();
				int i = Array.IndexOf(args, "-aetherOut");
				string output = i >= 0 && i + 1 < args.Length ? args[i + 1] : "Build/Aether";
				EditorApplication.Exit(Build(output) ? 0 : 1);
			} catch (Exception e) {
				Debug.LogException(e);
				EditorApplication.Exit(1);
			}
		}

		[MenuItem("Aether/Build HDRP DX12 compute bundle")]
		public static void BuildFromMenu() {
			Build("Build/Aether");
		}

		private static bool Build(string outputFolder) {
			// the bundle is only loaded on DX12: DX12 binaries alone
			PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.StandaloneWindows64, false);
			PlayerSettings.SetGraphicsAPIs(BuildTarget.StandaloneWindows64, new[] { GraphicsDeviceType.Direct3D12 });

			if (AssetDatabase.LoadAssetAtPath<ScriptableObject>(RuntimeResources) == null) {
				Debug.LogError("[Aether] HDRP's runtime resources were not found: is com.unity.render-pipelines.high-definition in Packages/manifest.json?");
				return false;
			}
			if (!PrepareHdrpAsset()) {
				return false;
			}
			string[] computes = AssetDatabase.GetDependencies(RuntimeResources, true)
				.Where(p => p.EndsWith(".compute", StringComparison.OrdinalIgnoreCase))
				.Distinct()
				.OrderBy(p => p, StringComparer.Ordinal)
				.ToArray();
			if (computes.Length == 0) {
				Debug.LogError("[Aether] HDRP's runtime resources reference no compute shader");
				return false;
			}
			Debug.Log($"[Aether] {computes.Length} HDRP runtime compute shaders");

			var build = new AssetBundleBuild {
				assetBundleName = BundleName,
				assetNames = computes.Append(WriteComputeMap()).ToArray(),
			};
			Directory.CreateDirectory(outputFolder);
			// ForceRebuild: Unity's incremental bundle build does not notice that the stripping changed (it reuses the
			// previous result when the source assets are the same)
			AssetBundleManifest manifest = BuildPipeline.BuildAssetBundles(outputFolder, new[] { build },
				BuildAssetBundleOptions.ChunkBasedCompression | BuildAssetBundleOptions.StrictMode | BuildAssetBundleOptions.ForceRebuildAssetBundle, BuildTarget.StandaloneWindows64);
			if (manifest == null) {
				Debug.LogError("[Aether] The bundle build failed");
				return false;
			}
			string built = Path.Combine(outputFolder, BundleName);
			Debug.Log($"[Aether] Built {built} ({new FileInfo(built).Length / 1024} KB, Unity {Application.unityVersion})");
			return true;
		}

		// HDRP strips compute shader kernels at build time by the HDRP assets in the build
		// (Editor/BuildProcessors/ShaderStrippers of the HDRP package):
		//  - with no HDRP asset, every kernel is removed (an empty bundle);
		//  - a variant is kept only if an asset wants exactly its combination of the mutually exclusive switches:
		//    punctual / directional / area shadow filtering quality, screen space shadows on or off, the light probe
		//    system, alpha in the colour buffer, HDR output.
		// The build therefore gets one HDRP asset with the game's values of those switches: only the variants the game
		// can ask for are compiled (Deferred.compute alone has 6696 over all combinations). Whole-shader features (water,
		// subsurface scattering, line rendering, the debug shaders) are left on: HDRP looks up kernels of shaders it may
		// not use, and they are small.
		private static bool PrepareHdrpAsset() {
			const string folder = "Assets/Aether/Generated/HdrpAssets";
			const BindingFlags any = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static;
			FieldInfo settingsField = typeof(HDRenderPipelineAsset).GetField("m_RenderPipelineSettings", any);
			MethodInfo newDefault = typeof(RenderPipelineSettings).GetMethod("NewDefault", any);
			if (settingsField == null || newDefault == null) {
				Debug.LogError("[Aether] This HDRP version keeps its asset settings elsewhere (m_RenderPipelineSettings / NewDefault not found)");
				return false;
			}
			if (!File.Exists(GameSettingsPath)) {
				Debug.LogError($"[Aether] {GameSettingsPath} is missing (scripts/build-hdrp-bundle.ps1 copies unity/hdrp-game-settings.json there)");
				return false;
			}
			var game = JsonUtility.FromJson<GameSettings>(File.ReadAllText(GameSettingsPath));

			if (AssetDatabase.IsValidFolder(folder)) {
				AssetDatabase.DeleteAsset(folder);
			}
			Directory.CreateDirectory(folder);
			AssetDatabase.Refresh();

			PlayerSettings.allowHDRDisplaySupport = game.hdrDisplay;
			var s = (RenderPipelineSettings)newDefault.Invoke(null, null);
			s.supportedLitShaderMode = (RenderPipelineSettings.SupportedLitShaderMode)Enum.Parse(typeof(RenderPipelineSettings.SupportedLitShaderMode), game.litShaderMode);
			s.colorBufferFormat = (RenderPipelineSettings.ColorBufferFormat)Enum.Parse(typeof(RenderPipelineSettings.ColorBufferFormat), game.colorBufferFormat);
			s.hdShadowInitParams.punctualShadowFilteringQuality = (HDShadowFilteringQuality)Enum.Parse(typeof(HDShadowFilteringQuality), game.punctualShadowFilteringQuality);
			s.hdShadowInitParams.directionalShadowFilteringQuality = (HDShadowFilteringQuality)Enum.Parse(typeof(HDShadowFilteringQuality), game.directionalShadowFilteringQuality);
			s.hdShadowInitParams.areaShadowFilteringQuality = (HDAreaShadowFilteringQuality)Enum.Parse(typeof(HDAreaShadowFilteringQuality), game.areaShadowFilteringQuality);
			s.hdShadowInitParams.supportScreenSpaceShadows = game.supportScreenSpaceShadows;
			s.lightProbeSystem = (RenderPipelineSettings.LightProbeSystem)Enum.Parse(typeof(RenderPipelineSettings.LightProbeSystem), game.lightProbeSystem);
			s.probeVolumeSHBands = (ProbeVolumeSHBands)Enum.Parse(typeof(ProbeVolumeSHBands), game.probeVolumeSHBands);
			// whole shaders: keep them all
			s.supportSSR = s.supportSSRTransparent = s.supportSSAO = s.supportSSGI = true;
			s.supportSubsurfaceScattering = true;
			s.supportVolumetrics = s.supportVolumetricClouds = true;
			s.supportWater = true;
			s.supportHighQualityLineRendering = true;
			s.supportRuntimeAOVAPI = true; // keeps the debug compute shaders
			s.supportMotionVectors = true;
			s.supportRayTracing = false;

			var asset = ScriptableObject.CreateInstance<HDRenderPipelineAsset>();
			settingsField.SetValue(asset, s);
			AssetDatabase.CreateAsset(asset, $"{folder}/GameLike.asset");
			AssetDatabase.SetLabels(asset, new[] { "HDRP:IncludeInBuild" }); // HDEditorUtils.HDRPAssetBuildLabel
			AssetDatabase.SaveAssets();

			// HDRP's strippers only run with a current HDRP asset and its global settings
			GraphicsSettings.defaultRenderPipeline = asset;
			for (int q = 0; q < QualitySettings.names.Length; ++q) {
				QualitySettings.SetQualityLevel(q, false);
				QualitySettings.renderPipeline = null;
			}
			Type globalSettings = typeof(HDRenderPipelineAsset).Assembly.GetType("UnityEngine.Rendering.HighDefinition.HDRenderPipelineGlobalSettings");
			object global = globalSettings?.GetMethod("Ensure", any)?.Invoke(null, new object[] { true });
			AssetDatabase.SaveAssets();
			AssetDatabase.Refresh();
			Debug.Log($"[Aether] HDRP asset for the build: shadows {game.punctualShadowFilteringQuality}/{game.directionalShadowFilteringQuality}/{game.areaShadowFilteringQuality}, screen space shadows {game.supportScreenSpaceShadows}, {game.lightProbeSystem}, lit {game.litShaderMode}, {game.colorBufferFormat}, HDR {game.hdrDisplay}; global settings: {(global != null ? "ok" : "MISSING")}");
			return GraphicsSettings.currentRenderPipeline != null && global != null;
		}

		// "resourceField=computeShaderName" for HDRP's runtime shader resources: the game renamed some of its copies
		// (e.g. its exposure and motion blur shaders), so the mod also matches by the resource field.
		private static string WriteComputeMap() {
			var lines = new List<string>();
			var resources = AssetDatabase.LoadAssetAtPath<ScriptableObject>(RuntimeResources);
			object shaders = resources.GetType().GetField("shaders")?.GetValue(resources);
			if (shaders != null) {
				foreach (var f in shaders.GetType().GetFields()) {
					if (f.FieldType == typeof(ComputeShader) && f.GetValue(shaders) is ComputeShader cs && cs != null) {
						lines.Add($"{f.Name}={cs.name}");
					}
				}
			}
			Directory.CreateDirectory(Path.GetDirectoryName(MapPath));
			File.WriteAllText(MapPath, string.Join("\n", lines));
			AssetDatabase.ImportAsset(MapPath);
			Debug.Log($"[Aether] HDRP compute map: {lines.Count} entries");
			return MapPath;
		}
	}
}
