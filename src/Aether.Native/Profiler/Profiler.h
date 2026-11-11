// Frame statistics: frame interval (render thread), GPU time from timestamp queries, Aether's own pass costs, the
// numbers the managed side reads from Unity, and the benchmark recorder.
#pragma once
#include "../Core/Common.h"
#include "../Core/Json.h"

namespace aether {

// Order of the doubles the managed side sends every frame (Aether_SubmitUnityStats). -1 = not available.
// Mirrored in src/Aether.Managed/Native.cs (UnityStat).
enum UnityStat {
	kUnityDeltaTimeMs = 0,
	kUnityCpuMainThreadMs,   // FrameTimingManager
	kUnityCpuRenderThreadMs,
	kUnityCpuPresentWaitMs,
	kUnityGpuFrameMs,
	kUnityDrawCalls,         // ProfilerRecorder, Render category
	kUnityBatches,
	kUnitySetPassCalls,
	kUnityTriangles,
	kUnityVertices,
	kUnityRenderTexturesCount,
	kUnityRenderTexturesBytes,
	kUnityUsedTexturesCount,
	kUnityUsedTexturesBytes,
	kUnityUsedBuffersCount,
	kUnityUsedBuffersBytes,
	kUnityTextureStreamingCurrentBytes, // Texture.currentTextureMemory
	kUnityTextureTotalBytes,            // Texture.totalTextureMemory (all mips loaded)
	kUnityTextureTargetBytes,           // Texture.targetTextureMemory
	kUnityGfxDriverBytes,               // Profiler.GetAllocatedMemoryForGraphicsDriver
	kUnityTotalAllocatedBytes,          // Profiler.GetTotalAllocatedMemoryLong
	kUnityScreenWidth,
	kUnityScreenHeight,
	kUnityShadowCasters,
	kUnityVisibleSkinnedMeshes,
	kUnityStatCount
};
const char* UnityStatName(int index);

struct PassTiming {
	std::string name;
	double cpuMs = 0;
	double gpuMs = -1; // -1 until the timestamps are back (or GPU timing is off)
	uint32_t drawCalls = 0;
	uint32_t dispatches = 0;
	bool executed = true;
	std::string note;
};

struct FrameSample {
	uint64_t frame = 0;
	double intervalMs = 0;   // time since the previous frame's end on the render thread: the frame time
	double aetherCpuMs = 0;  // CPU time Aether spent recording this frame
	double gpuFrameMs = -1;  // GPU time between the frame-begin and frame-end markers (whole frame, Unity included)
	double aetherGpuMs = -1; // GPU time of Aether's passes
	uint32_t drawCalls = 0;  // Aether's
	uint32_t dispatches = 0;
};

class Profiler {
public:
	static Profiler& Get();
	// ---- render thread
	void EndFrame(const FrameSample& sample, std::vector<PassTiming> passes);
	void GpuResult(uint64_t frame, double gpuFrameMs, double aetherGpuMs, const std::vector<double>& passGpuMs);
	// The scene renderer's frame: its passes (CPU side), draw calls and triangles; GPU times follow a few frames later.
	void SceneFrame(const std::vector<PassTiming>& passes, uint32_t drawCalls, uint32_t triangles);
	void SceneGpuResult(const std::vector<PassTiming>& passes, double totalGpuMs);
	void SceneIdle(); // the scene renderer drew nothing this frame
	// ---- main thread
	void SubmitUnityStats(const double* values, int count);
	// ---- any thread
	json Performance(int historyFrames = 0); // historyFrames > 0 adds the last N frame times
	json Passes();
	std::vector<PassTiming> PassSnapshot();
	void History(std::vector<float>& intervalMs, std::vector<float>& gpuMs, size_t maxCount);
	double UnityValue(int index);
	uint64_t FrameCount();

	// ---- benchmark
	// Starts recording. Returns "" or the reason it could not start.
	std::string StartBenchmark(const std::string& label, double seconds, const std::wstring& outputDir, json context);
	void StopBenchmark(); // finish early (writes the result with what was recorded)
	json BenchmarkStatus(); // {state: idle|running|done, progress, result?}

private:
	void FinishBenchmarkLocked();

	std::mutex mutex_;
	std::vector<FrameSample> ring_;
	size_t head_ = 0;
	uint64_t frames_ = 0;
	std::vector<PassTiming> passes_;
	std::vector<double> lastPassGpuMs_;
	std::vector<PassTiming> scenePasses_;
	uint32_t sceneDrawCalls_ = 0, sceneTriangles_ = 0;
	double sceneGpuMs_ = -1;
	bool sceneActive_ = false;
	std::vector<float> benchSceneGpu_;
	uint64_t passesFrame_ = 0;
	double unity_[kUnityStatCount];
	bool unityValid_ = false;
	double startTime_ = QpcSeconds();

	struct Benchmark {
		bool running = false;
		std::string label;
		double seconds = 0, startTime = 0;
		std::wstring outputDir;
		json context;
		std::vector<float> intervals, gpu, aetherGpu, unityDraws;
		uint64_t vramStart = 0, vramPeak = 0, vramEnd = 0, sharedPeak = 0;
		uint64_t firstFrame = 0;
	} bench_;
	json benchResult_;
};

} // namespace aether
