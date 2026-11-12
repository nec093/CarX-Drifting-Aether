// Aether's frame on the host's device. Phase 1: the passes are the DX12 integration test triangle, the Dear ImGui
// overlay and the frame capture, composited onto the host's back buffer at the end of the host's frame, bracketed by
// GPU timestamps. Everything here runs on the host's render thread unless noted.
//
// Rules this file keeps (docs/architecture/dx12-integration.md):
//  - command lists go to the host (HostGfx::execute) with the back buffer's state declared; Aether never changes the
//    state of a host resource behind the host's back, and holds no reference to a back buffer across frames;
//  - a command list and its allocator are reused only after the host's frame fence passed the value execute returned;
//  - any failed HRESULT stops Aether's rendering for the session (Fail) and leaves the game running.
#pragma once
#include <atomic>
#include "../Core/Common.h"
#include "../Core/Config.h"
#include "../Core/Json.h"
#include "../Device/Host.h"
#include "../Profiler/Profiler.h"

namespace aether {

class Renderer {
public:
	static Renderer& Get();

	// ---- render thread
	void FrameBegin(const HostGfx& host);
	void FrameEnd(const HostGfx& host);
	// The scene renderer's frame (render mode aether / compare): after the host drew its cameras, before its UI.
	void SceneFrame(const HostGfx& host);
	// Releases everything made on the device. waitMs > 0: first waits that long for the host fence to pass the last
	// submitted value; if it does not, the objects are leaked on purpose (releasing them could crash the GPU process).
	void ReleaseDeviceObjects(const HostGfx* host, uint32_t waitMs);

	// ---- any thread
	json State();
	json RenderGraph();
	bool Failed() const { return failed_.load(); }
	bool Ready() const { return ready_.load(); }
	// Asks for the next frame's back buffer as a PNG. Returns the capture id.
	uint64_t RequestCapture(const std::wstring& file, bool withOverlay);
	json CaptureStatus(uint64_t id); // {state: pending|encoding|done|failed, file, width, height, error}
	HWND Window() const { return hwnd_.load(); }

private:
	struct Context {
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		UINT64 fence = 0; // Aether's submission serial of the list recorded in it
		uint32_t index = 0;
		bool submitted = false;
		UINT64 hostFence = 0;     // what the host's ExecuteCommandList returned for it
		uint64_t submitTick = 0;  // acquireTick_ when it was submitted
	};
	uint64_t acquireTick_ = 0;
	struct SubmissionStats { uint64_t submissions = 0, reuses = 0, recordedOnly = 0; UINT64 lastHostFence = 0; double nextLog = 0; } stats_;
	void LogSubmissionStats(const HostGfx& host, double now);
	// Aether's own completion clock (docs/architecture/dx12-integration.md, "When is a list done"): every list ends
	// with a copy of its serial number into a readback buffer, so "the GPU has executed list N" is read from memory
	// the GPU itself wrote. The host's frame fence is not used for lifetimes.
	UINT64 Completed();
	bool EnsureMarkers();
	ComPtr<ID3D12Resource> markerUpload_, markerReadback_;
	uint64_t* markerSource_ = nullptr;
	const volatile uint64_t* markerDone_ = nullptr;
	uint64_t markerIds_[2] = {0, 0};
	UINT64 nextSerial_ = 1;
	UINT64 completedSerial_ = 0;
	UINT64 markedSerial_ = 0; // newest serial whose marker was seen (and signalled on completionFence_)
	ComPtr<ID3D12Fence> completionFence_;
	HostGfx clock_; // the host with Aether's clock in place of its fence: what the scene renderer is given
	struct TimestampFrame {
		bool pending = false;
		uint64_t frame = 0;
		UINT64 fence = 0;
		bool hasBegin = false;
		uint32_t passMask = 0; // which passes ran. Slots: 0 frame begin, 1 end-list start, then one after each pass that ran
	};
	enum Pass { kPassTriangle = 0, kPassOverlay, kPassCapture, kPassCount };
	struct Capture {
		uint64_t id = 0;
		std::wstring file;
		bool withOverlay = true;
		// after submission
		bool submitted = false;
		UINT64 fence = 0;
		ComPtr<ID3D12Resource> readback;
		uint64_t readbackId = 0;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	};

	bool EnsureDevice(const HostGfx& host);
	bool BuildPipelines();
	Context* Acquire(const HostGfx& host);
	UINT64 Submit(const HostGfx& host, Context* ctx, int stateCount, HostResourceState* states);
	int AcquireTimestampFrame();
	void CollectGpuResults(const HostGfx& host);
	void PollCaptures(const HostGfx& host);
	bool RecordCapture(Context* ctx, ID3D12Resource* backBuffer, Capture& capture);
	void Fail(const char* code, const std::string& what, HRESULT hr);
	void SetCaptureStatus(uint64_t id, const char* state, const std::string& file, uint32_t w, uint32_t h, const std::string& error);

	const SettingsData& CurrentSettings();
	SettingsData settings_;
	uint64_t settingsRevision_ = 0;
	UINT64* tsMapped_ = nullptr;

	std::atomic<bool> failed_{false}, ready_{false};
	std::atomic<HWND> hwnd_{nullptr};
	ID3D12Device* device_ = nullptr; // the host's (not owned; the host outlives the renderer's device objects)
	std::string hostName_;
	uint64_t frame_ = 0;
	uint64_t sceneFrame_ = 0;
	DWORD debugCookie_ = 0;
	uint64_t skippedFrames_ = 0;
	// what the last look for a free command list found (any thread reads these: State())
	std::atomic<uint64_t> statSkipped_{0}, statAcquires_{0};
	std::atomic<uint32_t> statLists_{0}, statBusyGpu_{0}, statBusyHost_{0}, statBusyDelay_{0};
	double lastFrameEnd_ = 0;
	UINT64 lastSubmittedFence_ = 0;

	std::vector<std::unique_ptr<Context>> contexts_;
	ComPtr<ID3D12DescriptorHeap> rtvHeap_;

	// test triangle
	ComPtr<ID3D12RootSignature> triRoot_;
	ComPtr<ID3D12PipelineState> triPso_;
	ComPtr<ID3D12Resource> triVb_;
	uint64_t triVbId_ = 0;
	D3D12_VERTEX_BUFFER_VIEW triVbView_ = {};
	DXGI_FORMAT pipelineFormat_ = DXGI_FORMAT_UNKNOWN;
	uint64_t shaderGeneration_ = 0;
	std::string triNote_;

	// GPU timestamps
	static const uint32_t kTsFrames = 32, kTsPerFrame = 8;
	ComPtr<ID3D12QueryHeap> tsHeap_;
	ComPtr<ID3D12Resource> tsReadback_;
	uint64_t tsReadbackId_ = 0;
	UINT64 tsFrequency_ = 0;
	TimestampFrame tsFrames_[kTsFrames];
	int tsCurrent_ = -1;       // slot the current frame uses (picked at FrameBegin, or at FrameEnd without a begin)
	bool tsCurrentHasBegin_ = false;

	// captures
	std::mutex captureMutex_;
	std::vector<Capture> captureRequests_;  // guarded by captureMutex_
	std::vector<Capture> capturePending_;   // render thread only
	json captureStatus_ = json::object();   // guarded by captureMutex_
	uint64_t nextCaptureId_ = 1;

	// state for State() (guarded by stateMutex_)
	std::mutex stateMutex_;
	json backBufferInfo_;
	std::string lastError_;
};

} // namespace aether
