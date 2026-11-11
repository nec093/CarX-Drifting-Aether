using System;
using Unity.Profiling;
using UnityEngine;
using UnityEngine.Profiling;

namespace Aether {
	/// <summary>
	/// Unity's own numbers, sent to the native profiler every frame without allocating: frame timings
	/// (FrameTimingManager), render counters (ProfilerRecorder) and texture memory. A counter this build of the game
	/// does not expose is sent as -1.
	/// </summary>
	internal sealed class UnityStats : IDisposable {
		private readonly double[] values_ = new double[(int)UnityStat.Count];
		private readonly ProfilerRecorder[] recorders_ = new ProfilerRecorder[(int)UnityStat.Count];
		private readonly bool[] hasRecorder_ = new bool[(int)UnityStat.Count];
		private readonly FrameTiming[] timings_ = new FrameTiming[1];
		private bool disposed_;

		public UnityStats() {
			Start(UnityStat.DrawCalls, ProfilerCategory.Render, "Draw Calls Count");
			Start(UnityStat.Batches, ProfilerCategory.Render, "Batches Count");
			Start(UnityStat.SetPassCalls, ProfilerCategory.Render, "SetPass Calls Count");
			Start(UnityStat.Triangles, ProfilerCategory.Render, "Triangles Count");
			Start(UnityStat.Vertices, ProfilerCategory.Render, "Vertices Count");
			Start(UnityStat.RenderTexturesCount, ProfilerCategory.Render, "Render Textures Count");
			Start(UnityStat.RenderTexturesBytes, ProfilerCategory.Render, "Render Textures Bytes");
			Start(UnityStat.UsedTexturesCount, ProfilerCategory.Render, "Used Textures Count");
			Start(UnityStat.UsedTexturesBytes, ProfilerCategory.Render, "Used Textures Bytes");
			Start(UnityStat.UsedBuffersCount, ProfilerCategory.Render, "Used Buffers Count");
			Start(UnityStat.UsedBuffersBytes, ProfilerCategory.Render, "Used Buffers Bytes");
			Start(UnityStat.ShadowCasters, ProfilerCategory.Render, "Shadow Casters Count");
			Start(UnityStat.VisibleSkinnedMeshes, ProfilerCategory.Render, "Visible Skinned Meshes Count");
		}

		private void Start(UnityStat stat, ProfilerCategory category, string name) {
			try {
				recorders_[(int)stat] = ProfilerRecorder.StartNew(category, name);
				hasRecorder_[(int)stat] = true;
			} catch (Exception) {
				hasRecorder_[(int)stat] = false;
			}
		}

		public unsafe void Submit() {
			if (disposed_) {
				return;
			}
			for (int i = 0; i < values_.Length; ++i) {
				values_[i] = -1;
				if (hasRecorder_[i] && recorders_[i].Valid) {
					values_[i] = recorders_[i].LastValue;
				}
			}
			values_[(int)UnityStat.DeltaTimeMs] = Time.unscaledDeltaTime * 1000.0;
			FrameTimingManager.CaptureFrameTimings();
			if (FrameTimingManager.GetLatestTimings(1, timings_) > 0) {
				FrameTiming t = timings_[0];
				values_[(int)UnityStat.CpuMainThreadMs] = t.cpuMainThreadFrameTime;
				values_[(int)UnityStat.CpuRenderThreadMs] = t.cpuRenderThreadFrameTime;
				values_[(int)UnityStat.CpuPresentWaitMs] = t.cpuMainThreadPresentWaitTime;
				values_[(int)UnityStat.GpuFrameMs] = t.gpuFrameTime > 0 ? t.gpuFrameTime : -1;
			}
			values_[(int)UnityStat.TextureStreamingCurrentBytes] = Texture.currentTextureMemory;
			values_[(int)UnityStat.TextureTotalBytes] = Texture.totalTextureMemory;
			values_[(int)UnityStat.TextureTargetBytes] = Texture.targetTextureMemory;
			long driver = Profiler.GetAllocatedMemoryForGraphicsDriver();
			values_[(int)UnityStat.GfxDriverBytes] = driver > 0 ? driver : -1;
			values_[(int)UnityStat.TotalAllocatedBytes] = Profiler.GetTotalAllocatedMemoryLong();
			values_[(int)UnityStat.ScreenWidth] = Screen.width;
			values_[(int)UnityStat.ScreenHeight] = Screen.height;
			fixed (double* p = values_) {
				Native.Aether_SubmitUnityStats(p, values_.Length);
			}
		}

		public void Dispose() {
			if (disposed_) {
				return;
			}
			disposed_ = true;
			for (int i = 0; i < recorders_.Length; ++i) {
				if (hasRecorder_[i]) {
					recorders_[i].Dispose();
				}
			}
		}
	}
}
