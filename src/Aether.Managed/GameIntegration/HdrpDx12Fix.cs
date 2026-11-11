using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.HighDefinition;

namespace Aether {
	/// <summary>
	/// Makes the game's own renderer (HDRP) work on Direct3D 12.
	///
	/// Found in this project's PoC (2026-11-11, Player.log): started with -force-d3d12 the game throws
	/// "ArgumentException: Kernel 'KDepthDownsample8DualUav' not found" from HDRenderPipeline's constructor
	/// (MipGenerator) every frame: its build has HDRP's compute shaders without DX12 kernels, the pipeline is never
	/// constructed, and nothing is drawn (a black window, no UI). Unity retries the construction every frame.
	///
	/// The fix replaces the compute shaders in HDRP's runtime resources
	/// (HDRenderPipelineGlobalSettings.m_RenderPipelineResources.shaders, checked against the game's HDRP assembly) with
	/// copies of the same HDRP version compiled for DX12, from kino/mods/Aether/hdrp/aether_hdrp_dx12.bundle
	/// (scripts/build-hdrp-bundle.ps1). The next construction attempt then succeeds. Nothing else of HDRP is touched.
	/// This is scaffolding: it is needed for as long as HDRP draws any part of the frame.
	/// </summary>
	internal static class HdrpDx12Fix {
		public const string BundleFile = "aether_hdrp_dx12.bundle";
		private static AssetBundle bundle_;

		/// <summary>One line for the status display and the "managed" state.</summary>
		public static string Status { get; private set; } = "not applied";
		public static bool Failed => Status.StartsWith("failed") || Status.StartsWith("missing");

		public static void Apply(string modDataDir, string configDir) {
			try {
				Status = Run(modDataDir, configDir);
			} catch (Exception e) {
				Status = "failed: " + e.GetType().Name + ": " + e.Message;
			}
			// (no call into the native runtime here: this runs before AetherNative.dll is in place)
			if (Failed) {
				KSL.API.Kino.Log.Error("[Aether] HDRP DX12 fix: " + Status);
			} else {
				KSL.API.Kino.Log.Info("[Aether] HDRP DX12 fix: " + Status);
			}
		}

		private static string Run(string modDataDir, string configDir) {
			if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D12) {
				return "not needed (not Direct3D 12)";
			}
			if (File.Exists(Path.Combine(configDir, "no-hdrp-fix"))) {
				return "switched off (kino/config/Aether/no-hdrp-fix)";
			}
			RenderPipelineGlobalSettings settings = GraphicsSettings.GetSettingsForRenderPipeline<HDRenderPipeline>();
			if (settings == null) {
				return "failed: HDRP's global settings were not found";
			}
			object resources = settings.GetType().GetField("m_RenderPipelineResources", BindingFlags.NonPublic | BindingFlags.Instance)?.GetValue(settings);
			object shaders = resources?.GetType().GetField("shaders", BindingFlags.Public | BindingFlags.Instance)?.GetValue(resources);
			if (shaders == null) {
				return "failed: HDRP's runtime shader resources were not found (another HDRP version?)";
			}

			string path = Path.Combine(modDataDir, "hdrp", BundleFile);
			if (!File.Exists(path)) {
				return "missing " + path + ": on Direct3D 12 the game's renderer cannot start without it (black picture)";
			}
			if (bundle_ == null) {
				bundle_ = AssetBundle.LoadFromFile(path);
			}
			if (bundle_ == null) {
				return "failed: " + path + " could not be loaded (built with another Unity version?)";
			}
			var byName = new Dictionary<string, ComputeShader>();
			foreach (ComputeShader cs in bundle_.LoadAllAssets<ComputeShader>()) {
				byName[cs.name] = cs;
			}
			// resource field -> the HDRP original's name (the game renamed some of its copies)
			var byField = new Dictionary<string, string>();
			TextAsset map = bundle_.LoadAsset<TextAsset>("aether_hdrp_compute_map");
			if (map != null) {
				foreach (string line in map.text.Split('\n')) {
					int eq = line.IndexOf('=');
					if (eq > 0) {
						byField[line.Substring(0, eq).Trim()] = line.Substring(eq + 1).Trim();
					}
				}
			}

			int replaced = 0, alreadyOurs = 0;
			var missing = new List<string>();
			foreach (FieldInfo field in shaders.GetType().GetFields(BindingFlags.Public | BindingFlags.Instance)) {
				if (field.FieldType != typeof(ComputeShader)) {
					continue;
				}
				var current = field.GetValue(shaders) as ComputeShader;
				if (current == null) {
					continue; // the game's build has none here: leave it
				}
				ComputeShader replacement = null;
				if (!byName.TryGetValue(current.name, out replacement) && byField.TryGetValue(field.Name, out string original)) {
					byName.TryGetValue(original, out replacement);
				}
				if (replacement == null) {
					missing.Add(field.Name + " (" + current.name + ")");
				} else if (replacement == current) {
					++alreadyOurs;
				} else {
					field.SetValue(shaders, replacement);
					++replaced;
				}
			}
			string result = "replaced " + replaced + " HDRP compute shaders with Direct3D 12 builds (" + byName.Count + " in the bundle" + (alreadyOurs > 0 ? ", " + alreadyOurs + " already replaced" : "") + ")";
			if (missing.Count > 0) {
				result += "; no copy for " + missing.Count + ": " + string.Join(", ", missing.GetRange(0, Math.Min(8, missing.Count)));
			}
			return result;
		}
	}
}
