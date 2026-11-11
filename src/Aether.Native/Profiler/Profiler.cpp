#include "Profiler.h"
#include "../Core/Log.h"
#include "../Device/GpuInfo.h"
#include "../Resources/ResourceRegistry.h"

#include <algorithm>
#include <fstream>
#include <numeric>

namespace aether {

static const size_t kHistory = 2048;

static const char* kUnityNames[kUnityStatCount] = {
	"deltaTimeMs", "cpuMainThreadMs", "cpuRenderThreadMs", "cpuPresentWaitMs", "gpuFrameMs",
	"drawCalls", "batches", "setPassCalls", "triangles", "vertices",
	"renderTexturesCount", "renderTexturesBytes", "usedTexturesCount", "usedTexturesBytes", "usedBuffersCount", "usedBuffersBytes",
	"textureStreamingCurrentBytes", "textureTotalBytes", "textureTargetBytes", "gfxDriverBytes", "totalAllocatedBytes",
	"screenWidth", "screenHeight", "shadowCasters", "visibleSkinnedMeshes",
};

const char* UnityStatName(int index) {
	return index >= 0 && index < kUnityStatCount ? kUnityNames[index] : "";
}

Profiler& Profiler::Get() {
	static Profiler* p = new Profiler();
	return *p;
}

static double Percentile(std::vector<float>& sorted, double p) {
	if (sorted.empty()) {
		return 0;
	}
	double pos = p * double(sorted.size() - 1);
	size_t lo = size_t(pos);
	size_t hi = std::min(lo + 1, sorted.size() - 1);
	double t = pos - double(lo);
	return sorted[lo] * (1.0 - t) + sorted[hi] * t;
}

// The "x% low" frame rate: the average frame rate over the slowest x% of the frames.
static double LowFps(const std::vector<float>& sortedAscendingMs, double fraction) {
	if (sortedAscendingMs.empty()) {
		return 0;
	}
	size_t n = std::max<size_t>(1, size_t(double(sortedAscendingMs.size()) * fraction));
	double sum = 0;
	for (size_t i = sortedAscendingMs.size() - n; i < sortedAscendingMs.size(); ++i) {
		sum += sortedAscendingMs[i];
	}
	double avgMs = sum / double(n);
	return avgMs > 0 ? 1000.0 / avgMs : 0;
}

static json Stats(std::vector<float> values) {
	if (values.empty()) {
		return nullptr;
	}
	std::sort(values.begin(), values.end());
	double sum = std::accumulate(values.begin(), values.end(), 0.0);
	return {{"avg", sum / double(values.size())}, {"min", values.front()}, {"max", values.back()}, {"p50", Percentile(values, 0.5)}, {"p95", Percentile(values, 0.95)}, {"p99", Percentile(values, 0.99)}};
}

void Profiler::EndFrame(const FrameSample& sample, std::vector<PassTiming> passes) {
	VideoMemory vm;
	bool sampleVram = false;
	{
		std::lock_guard lock(mutex_);
		sampleVram = bench_.running && (frames_ % 10) == 0;
	}
	if (sampleVram) {
		vm = GpuInfo::Get().QueryVideoMemory(); // outside the lock: a DXGI call
	}

	std::lock_guard lock(mutex_);
	if (ring_.size() < kHistory) {
		ring_.push_back(sample);
	} else {
		ring_[head_] = sample;
		head_ = (head_ + 1) % kHistory;
	}
	++frames_;
	passes_ = std::move(passes);
	passesFrame_ = sample.frame;
	// GPU times come back a few frames late: show the latest known time of each pass that ran
	if (lastPassGpuMs_.size() == passes_.size()) {
		for (size_t i = 0; i < passes_.size(); ++i) {
			if (passes_[i].executed) {
				passes_[i].gpuMs = lastPassGpuMs_[i];
			}
		}
	}

	if (bench_.running) {
		// the first interval after the start spans the pre-benchmark frame: skip it
		if (sample.frame > bench_.firstFrame && sample.intervalMs > 0) {
			bench_.intervals.push_back(float(sample.intervalMs));
			if (unityValid_ && unity_[kUnityDrawCalls] >= 0) {
				bench_.unityDraws.push_back(float(unity_[kUnityDrawCalls]));
			}
		}
		if (vm.valid) {
			bench_.vramPeak = std::max(bench_.vramPeak, vm.localUsage);
			bench_.sharedPeak = std::max(bench_.sharedPeak, vm.nonLocalUsage);
			bench_.vramEnd = vm.localUsage;
			if (bench_.vramStart == 0) {
				bench_.vramStart = vm.localUsage;
			}
		}
		if (QpcSeconds() - bench_.startTime >= bench_.seconds) {
			FinishBenchmarkLocked();
		}
	}
}

void Profiler::GpuResult(uint64_t frame, double gpuFrameMs, double aetherGpuMs, const std::vector<double>& passGpuMs) {
	std::lock_guard lock(mutex_);
	size_t n = ring_.size();
	for (size_t i = 0; i < n && i < 64; ++i) {
		FrameSample& s = ring_[(head_ + n - 1 - i) % n];
		if (s.frame == frame) {
			s.gpuFrameMs = gpuFrameMs;
			s.aetherGpuMs = aetherGpuMs;
			break;
		}
	}
	lastPassGpuMs_ = passGpuMs;
	if (passGpuMs.size() == passes_.size()) {
		for (size_t i = 0; i < passes_.size(); ++i) {
			if (passes_[i].executed) {
				passes_[i].gpuMs = passGpuMs[i];
			}
		}
	}
	if (bench_.running && frame > bench_.firstFrame) {
		if (gpuFrameMs >= 0) {
			bench_.gpu.push_back(float(gpuFrameMs));
		}
		if (aetherGpuMs >= 0) {
			bench_.aetherGpu.push_back(float(aetherGpuMs));
		}
	}
}

void Profiler::SubmitUnityStats(const double* values, int count) {
	std::lock_guard lock(mutex_);
	for (int i = 0; i < kUnityStatCount; ++i) {
		unity_[i] = (values && i < count) ? values[i] : -1.0;
	}
	unityValid_ = values != nullptr;
}

double Profiler::UnityValue(int index) {
	std::lock_guard lock(mutex_);
	return unityValid_ && index >= 0 && index < kUnityStatCount ? unity_[index] : -1.0;
}

uint64_t Profiler::FrameCount() {
	std::lock_guard lock(mutex_);
	return frames_;
}

void Profiler::History(std::vector<float>& intervalMs, std::vector<float>& gpuMs, size_t maxCount) {
	std::lock_guard lock(mutex_);
	size_t n = ring_.size();
	size_t count = std::min(n, maxCount);
	intervalMs.resize(count);
	gpuMs.resize(count);
	for (size_t i = 0; i < count; ++i) {
		const FrameSample& s = ring_[(head_ + n - count + i) % n];
		intervalMs[i] = float(s.intervalMs);
		gpuMs[i] = float(std::max(0.0, s.gpuFrameMs));
	}
}

json Profiler::Performance(int historyFrames) {
	std::lock_guard lock(mutex_);
	size_t n = ring_.size();
	// the window: the last 2 seconds for the averages, the whole history (up to kHistory frames) for the lows
	std::vector<float> all, recent, gpu, aetherGpu, aetherCpu;
	double now = 0, recentSum = 0;
	uint32_t drawCalls = 0, dispatches = 0;
	for (size_t i = 0; i < n; ++i) {
		const FrameSample& s = ring_[(head_ + n - 1 - i) % n];
		if (s.intervalMs <= 0) {
			continue;
		}
		all.push_back(float(s.intervalMs));
		now += s.intervalMs;
		if (now <= 2000.0 || recent.size() < 10) {
			recent.push_back(float(s.intervalMs));
			recentSum += s.intervalMs;
			if (s.gpuFrameMs >= 0) {
				gpu.push_back(float(s.gpuFrameMs));
			}
			if (s.aetherGpuMs >= 0) {
				aetherGpu.push_back(float(s.aetherGpuMs));
			}
			aetherCpu.push_back(float(s.aetherCpuMs));
		}
		if (i == 0) {
			drawCalls = s.drawCalls;
			dispatches = s.dispatches;
		}
	}
	json out;
	out["frameCount"] = frames_;
	out["uptimeSeconds"] = QpcSeconds() - startTime_;
	if (recent.empty()) {
		out["available"] = false;
		out["reason"] = "no frame has been rendered through Aether yet";
		return out;
	}
	out["available"] = true;
	double avgMs = recentSum / double(recent.size());
	out["fps"] = avgMs > 0 ? 1000.0 / avgMs : 0;
	out["frameTimeMs"] = Stats(recent);
	std::vector<float> sorted = all;
	std::sort(sorted.begin(), sorted.end());
	out["historyFrames"] = all.size();
	out["onePercentLowFps"] = LowFps(sorted, 0.01);
	out["pointOnePercentLowFps"] = LowFps(sorted, 0.001);
	out["gpuFrameTimeMs"] = Stats(gpu);       // null when GPU timing is off / not back yet
	out["aether"] = {{"cpuMs", Stats(aetherCpu)}, {"gpuMs", Stats(aetherGpu)}, {"drawCalls", drawCalls}, {"dispatchCalls", dispatches}};
	json unity = json::object();
	if (unityValid_) {
		for (int i = 0; i < kUnityStatCount; ++i) {
			if (unity_[i] >= 0) {
				unity[kUnityNames[i]] = unity_[i];
			}
		}
	}
	out["unity"] = unity; // only the values this build of the game exposes
	out["notes"] = "frameTimeMs is the interval between frame ends on the render thread. gpuFrameTimeMs spans Unity's frame and Aether's passes. unity.cpuPresentWaitMs is the present time when Unity's FrameTimingManager reports it.";
	if (historyFrames > 0) {
		json history = json::array();
		size_t count = std::min<size_t>(size_t(historyFrames), all.size());
		for (size_t i = 0; i < count; ++i) {
			history.push_back(all[count - 1 - i]); // oldest first
		}
		out["frameTimeHistoryMs"] = history;
	}
	return out;
}

std::vector<PassTiming> Profiler::PassSnapshot() {
	std::lock_guard lock(mutex_);
	return passes_;
}

json Profiler::Passes() {
	std::lock_guard lock(mutex_);
	json list = json::array();
	for (const PassTiming& p : passes_) {
		json j = {{"name", p.name}, {"executed", p.executed}, {"cpuMs", p.cpuMs}, {"drawCalls", p.drawCalls}, {"dispatchCalls", p.dispatches}};
		j["gpuMs"] = p.gpuMs >= 0 ? json(p.gpuMs) : json(nullptr);
		if (!p.note.empty()) {
			j["note"] = p.note;
		}
		list.push_back(j);
	}
	return {{"frame", passesFrame_}, {"passes", list}};
}

std::string Profiler::StartBenchmark(const std::string& label, double seconds, const std::wstring& outputDir, json context) {
	VideoMemory vm = GpuInfo::Get().QueryVideoMemory();
	std::lock_guard lock(mutex_);
	if (bench_.running) {
		return "a benchmark is already running";
	}
	if (frames_ == 0) {
		return "no frame has been rendered through Aether yet";
	}
	bench_ = Benchmark();
	bench_.running = true;
	bench_.label = label;
	bench_.seconds = seconds;
	bench_.startTime = QpcSeconds();
	bench_.outputDir = outputDir;
	bench_.context = std::move(context);
	bench_.firstFrame = ring_.empty() ? 0 : ring_[(head_ + ring_.size() - 1) % ring_.size()].frame + 1;
	if (vm.valid) {
		bench_.vramStart = bench_.vramPeak = bench_.vramEnd = vm.localUsage;
		bench_.sharedPeak = vm.nonLocalUsage;
	}
	benchResult_ = nullptr;
	AE_LOG(Info, "Benchmark", "Started '%s' for %.1f s", label.c_str(), seconds);
	return {};
}

void Profiler::StopBenchmark() {
	std::lock_guard lock(mutex_);
	if (bench_.running) {
		FinishBenchmarkLocked();
	}
}

void Profiler::FinishBenchmarkLocked() {
	bench_.running = false;
	double elapsed = QpcSeconds() - bench_.startTime;
	std::vector<float> sorted = bench_.intervals;
	std::sort(sorted.begin(), sorted.end());
	double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
	SYSTEMTIME t;
	GetLocalTime(&t);
	std::string stamp = Format("%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	json r;
	r["schema"] = "aether.benchmark/1";
	r["label"] = bench_.label;
	r["timestamp"] = stamp;
	r["requestedSeconds"] = bench_.seconds;
	r["elapsedSeconds"] = elapsed;
	r["frames"] = sorted.size();
	r["averageFps"] = sum > 0 ? 1000.0 * double(sorted.size()) / sum : 0;
	r["onePercentLowFps"] = LowFps(sorted, 0.01);
	r["pointOnePercentLowFps"] = LowFps(sorted, 0.001);
	r["frameTimeMs"] = Stats(bench_.intervals);
	r["gpuFrameTimeMs"] = Stats(bench_.gpu);
	r["aetherGpuTimeMs"] = Stats(bench_.aetherGpu);
	r["unityDrawCalls"] = Stats(bench_.unityDraws);
	r["dedicatedVramBytes"] = {{"start", bench_.vramStart}, {"end", bench_.vramEnd}, {"peak", bench_.vramPeak}};
	r["sharedGpuMemoryPeakBytes"] = bench_.sharedPeak;
	r["aetherResources"] = ResourceRegistry::Get().Summary();
	r["gpu"] = GpuInfo::Get().Brief();
	json unity = json::object();
	if (unityValid_) {
		for (int i = 0; i < kUnityStatCount; ++i) {
			if (unity_[i] >= 0) {
				unity[kUnityNames[i]] = unity_[i];
			}
		}
	}
	r["unityAtEnd"] = unity;
	r["context"] = bench_.context;

	if (!bench_.outputDir.empty()) {
		CreateDirectoryW(bench_.outputDir.c_str(), nullptr);
		std::string safe;
		for (char c : bench_.label) {
			safe += (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
		}
		std::wstring path = bench_.outputDir + L"\\" + Utf8ToWide((safe.empty() ? std::string("benchmark") : safe) + "-" + stamp + ".json");
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (out) {
			r["file"] = WideToUtf8(path);
			out << Dump(r, 2) << "\n";
		} else {
			AE_LOG_CODE(Error, "Benchmark", "E_BENCHMARK_WRITE", "Cannot write %s", WideToUtf8(path).c_str());
		}
	}
	AE_LOG(Info, "Benchmark", "Finished '%s': %zu frames, %.1f fps average, %.1f fps 1%% low", bench_.label.c_str(), sorted.size(), r["averageFps"].get<double>(), r["onePercentLowFps"].get<double>());
	benchResult_ = std::move(r);
	bench_.intervals.clear();
	bench_.gpu.clear();
	bench_.aetherGpu.clear();
	bench_.unityDraws.clear();
}

json Profiler::BenchmarkStatus() {
	std::lock_guard lock(mutex_);
	if (bench_.running) {
		double elapsed = QpcSeconds() - bench_.startTime;
		return {{"state", "running"}, {"label", bench_.label}, {"elapsedSeconds", elapsed}, {"requestedSeconds", bench_.seconds}, {"frames", bench_.intervals.size()}};
	}
	if (!benchResult_.is_null()) {
		return {{"state", "done"}, {"result", benchResult_}};
	}
	return {{"state", "idle"}};
}

} // namespace aether
