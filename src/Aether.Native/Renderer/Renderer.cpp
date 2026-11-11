#include "Renderer.h"
#include "../Core/DevOptions.h"
#include "Png.h"
#include "SceneRenderer.h"
#include "../Scene/SceneStore.h"
#include "../Core/Log.h"
#include "../Device/GpuInfo.h"
#include "../ImGui/Overlay.h"
#include "../Resources/ResourceRegistry.h"
#include "../Runtime.h"
#include "../Shaders/ShaderManager.h"

#include <algorithm>
#include <d3d12sdklayers.h>

namespace aether {

static const size_t kMaxContexts = 48;
static const uint64_t kReuseDelay = 24; // submissions (about 8 frames at 3 lists a frame) before a list object is handed to the host again
static const char* kPassNames[] = {"TestTriangle", "Overlay", "FrameCapture"};

Renderer& Renderer::Get() {
	static Renderer* r = new Renderer();
	return *r;
}

// The format Aether's render target view uses for a back buffer: values are written as they are (no sRGB encode on
// write: the overlay's colours are already display-referred).
static DXGI_FORMAT ViewFormat(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default: return f;
	}
}

// With the D3D12 debug layer on (the game started with -force-d3d12-debug, or the test host), its messages go into
// Aether's log: Unity's Player.log only says that the device was removed, not why.
static std::atomic<int> g_debugMessages{0};
static void CALLBACK OnDebugMessage(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR description, void*) {
	if (severity > D3D12_MESSAGE_SEVERITY_ERROR || g_debugMessages.fetch_add(1) >= 60) {
		return;
	}
	Log::Get().Write(Severity::Error, "D3D12", severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "E_D3D12_CORRUPTION" : "E_D3D12_VALIDATION", description ? description : "", {},
		{{"messageId", int(id)}, {"category", int(category)}});
}

const SettingsData& Renderer::CurrentSettings() {
	uint64_t revision = Settings::Get().Revision();
	if (revision != settingsRevision_) {
		settings_ = Settings::Get().Snapshot();
		settingsRevision_ = revision;
	}
	return settings_;
}

void Renderer::Fail(const char* code, const std::string& what, HRESULT hr) {
	if (failed_.exchange(true)) {
		return;
	}
	json extra = {{"hresult", HrToString(hr)}, {"frame", frame_}, {"host", hostName_}};
	if (device_) {
		HRESULT removed = device_->GetDeviceRemovedReason();
		extra["deviceRemovedReason"] = HrToString(removed);
		// DRED (only when the device was created with it enabled): where the GPU was when it stopped
		ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
		if (removed != S_OK && SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&dred)))) {
			D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {};
			if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault))) {
				extra["dredPageFaultVA"] = Format("0x%llX", static_cast<unsigned long long>(fault.PageFaultVA));
			}
			D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs = {};
			if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) {
				json nodes = json::array();
				int count = 0;
				for (const D3D12_AUTO_BREADCRUMB_NODE1* n = crumbs.pHeadAutoBreadcrumbNode; n && count < 16; n = n->pNext, ++count) {
					UINT last = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
					if (last == n->BreadcrumbCount) {
						continue; // this list ran to its end
					}
					nodes.push_back({{"commandList", n->pCommandListDebugNameW ? WideToUtf8(n->pCommandListDebugNameW) : ""}, {"completedOps", last}, {"totalOps", n->BreadcrumbCount},
						{"nextOp", last < n->BreadcrumbCount && n->pCommandHistory ? int(n->pCommandHistory[last]) : -1}});
				}
				extra["dredIncompleteLists"] = nodes;
			}
		}
	}
	extra["gpu"] = GpuInfo::Get().Brief();
	{
		std::lock_guard lock(stateMutex_);
		lastError_ = std::string(code) + ": " + what + " (" + HrToString(hr) + ")";
	}
	Log::Get().Write(Severity::Fatal, "Renderer", code, what + ". Aether's rendering is off for this session; the game keeps running.", {}, extra);
	json report = extra;
	report["code"] = code;
	report["description"] = what;
	report["logTail"] = json::array();
	for (const LogEntry& e : Log::Get().Snapshot(40)) {
		report["logTail"].push_back(ToJson(e));
	}
	Runtime::Get().WriteCrashReport(std::move(report));
}

bool Renderer::EnsureDevice(const HostGfx& host) {
	if (!host.device || !host.queue || !host.execute || !host.completedFenceValue) {
		return false;
	}
	if (ready_.load()) {
		if (host.device == device_) {
			return true;
		}
		AE_LOG(Warning, "Renderer", "The host's device changed: rebuilding Aether's device objects");
		ReleaseDeviceObjects(nullptr, 0);
	}
	device_ = host.device;
	hostName_ = host.name;
	GpuInfo::Get().Capture(device_);
	{
		ComPtr<ID3D12InfoQueue1> infoQueue;
		if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
			debugCookie_ = 0;
			if (SUCCEEDED(infoQueue->RegisterMessageCallback(OnDebugMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &debugCookie_))) {
				AE_LOG(Info, "Renderer", "The D3D12 debug layer is on: its errors are copied into this log");
			}
		}
	}

	D3D12_DESCRIPTOR_HEAP_DESC rtv = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
	HRESULT hr = device_->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&rtvHeap_));
	if (FAILED(hr)) {
		Fail("E_DEVICE_OBJECTS", "CreateDescriptorHeap (RTV) failed", hr);
		return false;
	}

	// GPU timestamps: a query heap and a readback buffer that stays mapped
	D3D12_QUERY_HEAP_DESC qh = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kTsFrames * kTsPerFrame, 0};
	hr = device_->CreateQueryHeap(&qh, IID_PPV_ARGS(&tsHeap_));
	if (SUCCEEDED(hr)) {
		D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = kTsFrames * kTsPerFrame * sizeof(UINT64);
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tsReadback_));
	}
	if (SUCCEEDED(hr)) {
		void* p = nullptr;
		hr = tsReadback_->Map(0, nullptr, &p);
		tsMapped_ = static_cast<UINT64*>(p);
	}
	if (SUCCEEDED(hr)) {
		hr = host.queue->GetTimestampFrequency(&tsFrequency_);
	}
	if (FAILED(hr) || tsFrequency_ == 0) {
		// not fatal: the frame works without GPU timing
		AE_LOG_CODE(Warning, "Renderer", "W_GPU_TIMING", "GPU timestamps are not available: %s", HrToString(hr).c_str());
		tsHeap_.Reset();
		tsReadback_.Reset();
		tsMapped_ = nullptr;
	} else {
		tsReadbackId_ = ResourceRegistry::Get().Register(device_, tsReadback_.Get(), "Profiler.TimestampReadback", ResourceCategory::Readback, D3D12_HEAP_TYPE_READBACK, frame_);
	}
	for (TimestampFrame& t : tsFrames_) {
		t = TimestampFrame();
	}
	tsCurrent_ = -1;
	clock_ = host;
	clock_.completedFenceValue = [this]() -> UINT64 { return Completed(); };
	clock_.nextFenceValue = [this]() -> UINT64 { return nextSerial_; };
	ready_.store(true);
	AE_LOG(Info, "Renderer", "Device objects ready on the %s device (timestamp frequency %llu Hz)", host.name, static_cast<unsigned long long>(tsFrequency_));
	return true;
}

bool Renderer::BuildPipelines() {
	triPso_.Reset();
	triNote_.clear();
	shaderGeneration_ = ShaderManager::Get().Generation();
	if (pipelineFormat_ == DXGI_FORMAT_UNKNOWN) {
		return true;
	}
	HRESULT hr = S_OK;
	if (!triRoot_) {
		D3D12_ROOT_SIGNATURE_DESC rs = {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
		ComPtr<ID3DBlob> blob, error;
		hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
		if (SUCCEEDED(hr)) {
			hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&triRoot_));
		}
		if (FAILED(hr)) {
			Fail("E_DEVICE_OBJECTS", "Root signature creation failed", hr);
			return false;
		}
	}
	if (!triVb_) {
		struct Vertex { float pos[2]; float color[3]; };
		// a small triangle in the top right corner (clip space)
		const Vertex vertices[3] = {{{0.88f, 0.95f}, {1.0f, 0.25f, 0.2f}}, {{0.96f, 0.72f}, {0.2f, 1.0f, 0.3f}}, {{0.80f, 0.72f}, {0.25f, 0.4f, 1.0f}}};
		D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = sizeof(vertices);
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&triVb_));
		void* p = nullptr;
		if (SUCCEEDED(hr)) {
			hr = triVb_->Map(0, nullptr, &p);
		}
		if (FAILED(hr)) {
			Fail("E_DEVICE_OBJECTS", "Test triangle vertex buffer creation failed", hr);
			return false;
		}
		memcpy(p, vertices, sizeof(vertices));
		triVb_->Unmap(0, nullptr);
		triVbView_ = {triVb_->GetGPUVirtualAddress(), UINT(sizeof(vertices)), UINT(sizeof(Vertex))};
		triVbId_ = ResourceRegistry::Get().Register(device_, triVb_.Get(), "TestTriangle.VertexBuffer", ResourceCategory::Buffer, D3D12_HEAP_TYPE_UPLOAD, frame_);
	}

	ComPtr<ID3DBlob> vs = ShaderManager::Get().Compile("test_triangle.hlsl", "VSMain", "vs_5_1");
	ComPtr<ID3DBlob> ps = ShaderManager::Get().Compile("test_triangle.hlsl", "PSMain", "ps_5_1");
	if (!vs || !ps) {
		triNote_ = "shader compile failed (see the Shaders tab / get_shader_errors)";
		return true; // a shader error is not a device failure
	}
	static const D3D12_INPUT_ELEMENT_DESC layout[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
	d.pRootSignature = triRoot_.Get();
	d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
	d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
	d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	d.SampleMask = UINT_MAX;
	d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	d.RasterizerState.DepthClipEnable = TRUE;
	d.InputLayout = {layout, UINT(_countof(layout))};
	d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	d.NumRenderTargets = 1;
	d.RTVFormats[0] = pipelineFormat_;
	d.SampleDesc.Count = 1;
	hr = device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&triPso_));
	if (FAILED(hr)) {
		// a bad shader / format combination: keep running without the pass
		triNote_ = "pipeline state creation failed: " + HrToString(hr);
		Log::Get().Write(Severity::Error, "Renderer", "E_PIPELINE_STATE", "Test triangle pipeline state creation failed", "TestTriangle", {{"hresult", HrToString(hr)}, {"rtvFormat", int(pipelineFormat_)}});
		triPso_.Reset();
	}
	return true;
}

bool Renderer::EnsureMarkers() {
	if (markerDone_) {
		return true;
	}
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = kMaxContexts * sizeof(uint64_t);
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	D3D12_HEAP_PROPERTIES upload = {D3D12_HEAP_TYPE_UPLOAD}, readback = {D3D12_HEAP_TYPE_READBACK};
	HRESULT hr = device_->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&markerUpload_));
	if (SUCCEEDED(hr)) {
		hr = device_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&markerReadback_));
	}
	void* source = nullptr;
	void* done = nullptr;
	if (SUCCEEDED(hr)) hr = markerUpload_->Map(0, nullptr, &source);
	if (SUCCEEDED(hr)) hr = markerReadback_->Map(0, nullptr, &done);
	if (FAILED(hr)) {
		Fail("E_DEVICE_OBJECTS", "The completion marker buffers could not be created", hr);
		return false;
	}
	memset(source, 0, size_t(desc.Width));
	hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completionFence_));
	if (FAILED(hr)) {
		Fail("E_DEVICE_OBJECTS", "The completion fence could not be created", hr);
		return false;
	}
	markerSource_ = static_cast<uint64_t*>(source);
	markerDone_ = static_cast<const volatile uint64_t*>(done);
	markerIds_[0] = ResourceRegistry::Get().Register(device_, markerUpload_.Get(), "Renderer.MarkerUpload", ResourceCategory::Upload, D3D12_HEAP_TYPE_UPLOAD, frame_);
	markerIds_[1] = ResourceRegistry::Get().Register(device_, markerReadback_.Get(), "Renderer.MarkerReadback", ResourceCategory::Readback, D3D12_HEAP_TYPE_READBACK, frame_);
	return true;
}

// The serial of the newest list the GPU has finished. Lists run in the order they were submitted, so everything
// with a smaller serial is finished too.
//
// Two steps, because neither alone is enough on a queue Aether does not own:
//  1. the marker shows that the host really put the list on the queue and the GPU ran through it (the host's own
//     fence value does not promise that);
//  2. only then Aether signals its own fence on the queue. That signal is queued behind the list, so once the fence
//     shows the serial, the list's execution is complete for the runtime as well (allocators may be reset).
UINT64 Renderer::Completed() {
	if (markerDone_ && completionFence_ && clock_.queue) {
		UINT64 seen = markedSerial_;
		for (auto& c : contexts_) {
			if (c->submitted && markerDone_[c->index] == c->fence && c->fence > seen) {
				seen = c->fence;
			}
		}
		if (seen > markedSerial_ && SUCCEEDED(clock_.queue->Signal(completionFence_.Get(), seen))) {
			markedSerial_ = seen;
		}
		const UINT64 done = completionFence_->GetCompletedValue();
		if (done != UINT64_MAX && done > completedSerial_) { // UINT64_MAX: device removed
			completedSerial_ = done;
		}
	}
	return completedSerial_;
}

Renderer::Context* Renderer::Acquire(const HostGfx& host) {
	if (!EnsureMarkers()) {
		return nullptr;
	}
	const UINT64 completed = Completed();
	const UINT64 hostCompleted = host.completedFenceValue ? host.completedFenceValue() : UINT64_MAX;
	++acquireTick_;
	for (auto& c : contexts_) {
		// A list is used again only when (1) the GPU finished it (Aether's own clock), (2) the host's frame fence passed
		// the value the host returned for it and (3) a few submissions went by: the host keeps its own record of a list
		// it was handed, and handing it the same list object again while that record is alive corrupts its bookkeeping.
		if (c->submitted && (c->fence > completed || c->hostFence > hostCompleted || acquireTick_ < c->submitTick + kReuseDelay)) {
			continue;
		}
		c->fence = nextSerial_++;
		++stats_.reuses;
		HRESULT hr = c->allocator->Reset();
		if (SUCCEEDED(hr)) {
			hr = c->list->Reset(c->allocator.Get(), nullptr);
		}
		if (FAILED(hr)) {
			Fail("E_COMMAND_LIST", "Command list reset failed", hr);
			return nullptr;
		}
		c->submitted = false;
		return c.get();
	}
	if (contexts_.size() >= kMaxContexts) {
		return nullptr; // the host fence is not advancing: skip this frame rather than pile up lists
	}
	auto c = std::make_unique<Context>();
	HRESULT hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c->allocator));
	if (SUCCEEDED(hr)) {
		hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator.Get(), nullptr, IID_PPV_ARGS(&c->list));
	}
	if (FAILED(hr)) {
		Fail("E_COMMAND_LIST", "Command list creation failed", hr);
		return nullptr;
	}
	c->index = uint32_t(contexts_.size());
	c->fence = nextSerial_++;
	c->list->SetName(Utf8ToWide(Format("Aether.CommandList%zu", contexts_.size())).c_str());
	c->allocator->SetName(Utf8ToWide(Format("Aether.CommandAllocator%zu", contexts_.size())).c_str());
	contexts_.push_back(std::move(c));
	return contexts_.back().get();
}

UINT64 Renderer::Submit(const HostGfx& host, Context* ctx, int stateCount, HostResourceState* states) {
	// the last command of every list: its serial, written where Completed() reads it once the GPU got this far
	markerSource_[ctx->index] = ctx->fence;
	ctx->list->CopyBufferRegion(markerReadback_.Get(), UINT64(ctx->index) * sizeof(uint64_t), markerUpload_.Get(), UINT64(ctx->index) * sizeof(uint64_t), sizeof(uint64_t));
	HRESULT hr = ctx->list->Close();
	if (FAILED(hr)) {
		Fail("E_COMMAND_LIST", "Command list close failed (a recorded command was invalid)", hr);
		return 0;
	}
	const DevOptions& dev = DevOptions::Get();
	if (dev.recordOnly) {
		// experiment: recorded and closed, never handed to the GPU; the list is free again at once
		ctx->submitted = false;
		++stats_.recordedOnly;
		return ctx->fence;
	}
	if (dev.directSubmit && host.queue) {
		// straight onto the host's queue (the event grants queue access): the host keeps no record of this list
		ID3D12CommandList* lists[] = {ctx->list.Get()};
		host.queue->ExecuteCommandLists(1, lists);
		ctx->hostFence = 0;
	} else {
		ctx->hostFence = host.execute(ctx->list.Get(), stateCount, states);
	}
	++stats_.submissions;
	stats_.lastHostFence = ctx->hostFence;
	ctx->submitTick = acquireTick_;
	ctx->submitted = true;
	lastSubmittedFence_ = std::max(lastSubmittedFence_, ctx->fence);
	return ctx->fence;
}

// One line every DevOptions::statsSeconds: enough to see, after a crash, how far the submissions had come.
void Renderer::LogSubmissionStats(const HostGfx& host, double now) {
	const int every = DevOptions::Get().statsSeconds;
	if (every <= 0 || now < stats_.nextLog) {
		return;
	}
	stats_.nextLog = now + every;
	uint32_t busy = 0;
	for (auto& c : contexts_) {
		busy += c->submitted ? 1u : 0u;
	}
	json registry = ResourceRegistry::Get().Summary();
	Log::Get().Write(Severity::Info, "Renderer", "", "Submission statistics", {},
		{{"frame", frame_}, {"submissions", stats_.submissions}, {"recordedOnly", stats_.recordedOnly}, {"listReuses", stats_.reuses}, {"lists", contexts_.size()}, {"listsBusy", busy},
			{"nextSerial", nextSerial_}, {"markerSeenSerial", markedSerial_}, {"completedSerial", completedSerial_}, {"lastHostFence", stats_.lastHostFence},
			{"hostCompletedFence", host.completedFenceValue ? host.completedFenceValue() : 0}, {"skippedFrames", skippedFrames_},
			{"resourceAllocations", registry["allocations"]}, {"resourceReleases", registry["releases"]}, {"pendingRetirements", GpuResources::Get().PendingRetirements()},
			{"renderMode", CurrentSettings().renderMode}});
}

int Renderer::AcquireTimestampFrame() {
	if (!tsHeap_) {
		return -1;
	}
	for (uint32_t i = 0; i < kTsFrames; ++i) {
		if (!tsFrames_[i].pending) {
			return int(i);
		}
	}
	return -1;
}

void Renderer::CollectGpuResults(const HostGfx& host) {
	(void)host;
	if (!tsHeap_ || !tsMapped_) {
		return;
	}
	UINT64 completed = Completed();
	const double toMs = 1000.0 / double(tsFrequency_);
	for (uint32_t i = 0; i < kTsFrames; ++i) {
		TimestampFrame& t = tsFrames_[i];
		if (!t.pending || t.fence > completed) {
			continue;
		}
		t.pending = false;
		const UINT64* v = tsMapped_ + i * kTsPerFrame;
		std::vector<double> passMs(kPassCount, -1.0);
		uint32_t slot = 2;
		for (int p = 0; p < kPassCount; ++p) {
			if ((t.passMask & (1u << p)) && slot < kTsPerFrame) {
				passMs[size_t(p)] = v[slot] >= v[slot - 1] ? double(v[slot] - v[slot - 1]) * toMs : -1.0;
				++slot;
			}
		}
		UINT64 last = v[slot - 1];
		double aetherMs = last >= v[1] ? double(last - v[1]) * toMs : -1.0;
		double frameMs = (t.hasBegin && last >= v[0]) ? double(last - v[0]) * toMs : -1.0;
		Profiler::Get().GpuResult(t.frame, frameMs, aetherMs, passMs);
	}
}

void Renderer::FrameBegin(const HostGfx& host) {
	if (failed_.load()) {
		return;
	}
	if (!CurrentSettings().gpuTiming) {
		tsCurrent_ = -1;
		tsCurrentHasBegin_ = false;
		return;
	}
	if (!EnsureDevice(host) || !tsHeap_) {
		return;
	}
	if (tsCurrent_ >= 0 && tsCurrentHasBegin_) {
		return; // a second begin without an end: the first marker stands
	}
	int slot = AcquireTimestampFrame();
	if (slot < 0) {
		return;
	}
	Context* ctx = Acquire(host);
	if (!ctx) {
		return;
	}
	ctx->list->EndQuery(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(slot) * kTsPerFrame);
	Submit(host, ctx, 0, nullptr);
	tsCurrent_ = slot;
	tsCurrentHasBegin_ = true;
}

bool Renderer::RecordCapture(Context* ctx, ID3D12Resource* backBuffer, Capture& capture) {
	D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
	UINT64 total = 0;
	device_->GetCopyableFootprints(&desc, 0, 1, 0, &capture.footprint, nullptr, nullptr, &total);
	D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
	D3D12_RESOURCE_DESC buffer = {};
	buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer.Width = total;
	buffer.Height = 1;
	buffer.DepthOrArraySize = 1;
	buffer.MipLevels = 1;
	buffer.SampleDesc.Count = 1;
	buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&capture.readback));
	if (FAILED(hr)) {
		// out of memory for one capture is not a reason to stop rendering
		SetCaptureStatus(capture.id, "failed", WideToUtf8(capture.file), 0, 0, "readback buffer allocation failed: " + HrToString(hr));
		Log::Get().Write(Severity::Error, "Renderer", "E_CAPTURE_ALLOC", "Frame capture readback allocation failed", "FrameCapture.Readback", {{"hresult", HrToString(hr)}, {"bytes", total}});
		return false;
	}
	capture.readbackId = ResourceRegistry::Get().Register(device_, capture.readback.Get(), "FrameCapture.Readback", ResourceCategory::Readback, D3D12_HEAP_TYPE_READBACK, frame_);
	capture.format = desc.Format;

	ID3D12GraphicsCommandList* list = ctx->list.Get();
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = backBuffer;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	list->ResourceBarrier(1, &barrier);
	D3D12_TEXTURE_COPY_LOCATION dst = {capture.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
	dst.PlacedFootprint = capture.footprint;
	D3D12_TEXTURE_COPY_LOCATION src = {backBuffer, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
	src.SubresourceIndex = 0;
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
	list->ResourceBarrier(1, &barrier);
	return true;
}

void Renderer::SetCaptureStatus(uint64_t id, const char* state, const std::string& file, uint32_t w, uint32_t h, const std::string& error) {
	std::lock_guard lock(captureMutex_);
	json j = {{"id", id}, {"state", state}, {"file", file}};
	if (w) {
		j["width"] = w;
		j["height"] = h;
	}
	if (!error.empty()) {
		j["error"] = error;
	}
	captureStatus_[std::to_string(id)] = j;
	// keep the map small
	while (captureStatus_.size() > 32) {
		captureStatus_.erase(captureStatus_.begin());
	}
}

void Renderer::PollCaptures(const HostGfx& host) {
	(void)host;
	if (capturePending_.empty()) {
		return;
	}
	UINT64 completed = Completed();
	for (size_t i = 0; i < capturePending_.size();) {
		Capture& c = capturePending_[i];
		if (c.fence > completed) {
			++i;
			continue;
		}
		const uint32_t w = c.footprint.Footprint.Width, h = c.footprint.Footprint.Height;
		std::string file = WideToUtf8(c.file);
		auto pixels = std::make_shared<std::vector<uint8_t>>();
		std::string error;
		void* p = nullptr;
		D3D12_RANGE range = {0, SIZE_T(c.footprint.Offset + UINT64(c.footprint.Footprint.RowPitch) * h)};
		HRESULT hr = c.readback->Map(0, &range, &p);
		if (SUCCEEDED(hr)) {
			if (!ConvertToRgba8(c.format, static_cast<const uint8_t*>(p) + c.footprint.Offset, c.footprint.Footprint.RowPitch, w, h, *pixels)) {
				error = Format("unsupported back buffer format (DXGI_FORMAT %d)", int(c.format));
			}
			D3D12_RANGE none = {0, 0};
			c.readback->Unmap(0, &none);
		} else {
			error = "readback map failed: " + HrToString(hr);
		}
		ResourceRegistry::Get().Unregister(c.readbackId, frame_);
		uint64_t id = c.id;
		std::wstring path = c.file;
		capturePending_.erase(capturePending_.begin() + ptrdiff_t(i));
		if (!error.empty()) {
			SetCaptureStatus(id, "failed", file, w, h, error);
			continue;
		}
		SetCaptureStatus(id, "encoding", file, w, h, {});
		Runtime::Get().Post([this, id, path, file, w, h, pixels] {
			std::string err;
			if (SavePng(path, w, h, *pixels, err)) {
				SetCaptureStatus(id, "done", file, w, h, {});
				AE_LOG(Info, "Renderer", "Frame captured: %s (%ux%u)", file.c_str(), w, h);
			} else {
				SetCaptureStatus(id, "failed", file, w, h, err);
				AE_LOG_CODE(Error, "Renderer", "E_CAPTURE_WRITE", "Frame capture could not be written: %s", err.c_str());
			}
		});
	}
}

void Renderer::FrameEnd(const HostGfx& host) {
	if (failed_.load()) {
		return;
	}
	const double start = QpcSeconds();
	FrameSample sample;
	sample.frame = ++frame_;
	sample.intervalMs = lastFrameEnd_ > 0 ? (start - lastFrameEnd_) * 1000.0 : 0;
	lastFrameEnd_ = start;
	std::vector<PassTiming> passes(kPassCount);
	for (int p = 0; p < kPassCount; ++p) {
		passes[size_t(p)].name = kPassNames[p];
		passes[size_t(p)].executed = false;
	}
	auto finish = [&] {
		sample.aetherCpuMs = (QpcSeconds() - start) * 1000.0;
		Profiler::Get().EndFrame(sample, std::move(passes));
	};

	if (!EnsureDevice(host)) {
		finish();
		return;
	}
	LogSubmissionStats(host, start);
	CollectGpuResults(host);
	PollCaptures(host);
	const SettingsData& s = CurrentSettings();
	Overlay& overlay = Overlay::Get();
	if (!hwnd_.load() && host.swapChain) {
		// the window is needed for the overlay's hotkey even while nothing is drawn
		DXGI_SWAP_CHAIN_DESC desc = {};
		IDXGISwapChain* sc = host.swapChain();
		if (sc && SUCCEEDED(sc->GetDesc(&desc))) {
			hwnd_.store(desc.OutputWindow);
		}
	}

	Capture capture;
	bool wantCapture = false;
	{
		std::lock_guard lock(captureMutex_);
		if (!captureRequests_.empty()) {
			capture = std::move(captureRequests_.front());
			captureRequests_.erase(captureRequests_.begin());
			wantCapture = true;
		}
	}
	// the host's very first frames may not have the back buffer bound as a render target yet (seen as a debug layer
	// error on frame 1): nothing is drawn onto it until the host has presented a few frames
	const bool settled = frame_ > 8;
	const bool wantOverlay = settled && overlay.WantsFrame();
	const bool wantTriangle = settled && s.testTriangle;
	bool wantDraw = wantTriangle || wantOverlay || wantCapture;
	int ts = tsCurrent_;
	const bool hasBegin = tsCurrentHasBegin_ && ts >= 0;
	tsCurrent_ = -1;
	tsCurrentHasBegin_ = false;
	if (!s.gpuTiming) {
		ts = -1;
	} else if (ts < 0) {
		ts = AcquireTimestampFrame();
	}
	if (!wantDraw && ts < 0) {
		finish();
		return;
	}

	// ---- the back buffer the host is about to present
	ComPtr<ID3D12Resource> backBuffer;
	D3D12_RESOURCE_DESC bbDesc = {};
	DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
	std::string skipNote;
	if (wantDraw) {
		IDXGISwapChain* sc = host.swapChain ? host.swapChain() : nullptr;
		ComPtr<IDXGISwapChain3> sc3;
		DXGI_SWAP_CHAIN_DESC scDesc = {};
		if (sc && SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && SUCCEEDED(sc3->GetDesc(&scDesc))) {
			sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));
			hwnd_.store(scDesc.OutputWindow);
		}
		if (!backBuffer) {
			skipNote = "the host has no swap chain back buffer this frame";
		} else {
			bbDesc = backBuffer->GetDesc();
			viewFormat = ViewFormat(bbDesc.Format);
			if (bbDesc.SampleDesc.Count != 1 || bbDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
				skipNote = "unsupported back buffer (multisampled)";
				backBuffer.Reset();
			}
		}
		if (backBuffer) {
			std::lock_guard lock(stateMutex_);
			if (backBufferInfo_.is_null() || backBufferInfo_.value("width", 0u) != UINT(bbDesc.Width) || backBufferInfo_.value("height", 0u) != bbDesc.Height || backBufferInfo_.value("dxgiFormat", 0) != int(bbDesc.Format)) {
				backBufferInfo_ = {{"width", UINT(bbDesc.Width)}, {"height", bbDesc.Height}, {"dxgiFormat", int(bbDesc.Format)}, {"viewDxgiFormat", int(viewFormat)}, {"bufferCount", scDesc.BufferCount}, {"swapEffect", int(scDesc.SwapEffect)}};
				AE_LOG(Info, "Renderer", "Back buffer %ux%u, DXGI format %d (view %d), %u buffers", UINT(bbDesc.Width), bbDesc.Height, int(bbDesc.Format), int(viewFormat), scDesc.BufferCount);
			}
		}
	}
	if (backBuffer && (viewFormat != pipelineFormat_ || shaderGeneration_ != ShaderManager::Get().Generation())) {
		pipelineFormat_ = viewFormat;
		if (!BuildPipelines()) {
			finish();
			return;
		}
	}
	if (!backBuffer) {
		wantDraw = false;
		if (wantCapture) {
			SetCaptureStatus(capture.id, "failed", WideToUtf8(capture.file), 0, 0, skipNote);
			wantCapture = false;
		}
		if (ts < 0) {
			passes[kPassTriangle].note = passes[kPassOverlay].note = skipNote;
			finish();
			return;
		}
	}

	Context* ctx = Acquire(host);
	if (!ctx) {
		++skippedFrames_;
		if (wantCapture) {
			SetCaptureStatus(capture.id, "failed", WideToUtf8(capture.file), 0, 0, "no free command list (the host fence is not advancing)");
		}
		finish();
		return;
	}
	ID3D12GraphicsCommandList* list = ctx->list.Get();
	uint32_t tsSlot = 1, passMask = 0;
	auto stamp = [&] {
		if (ts >= 0 && tsSlot < kTsPerFrame) {
			list->EndQuery(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(ts) * kTsPerFrame + tsSlot++);
		}
	};
	stamp(); // slot 1: start of Aether's passes

	bool overlayDrawn = false, captureRecorded = false;
	// At the frame end the host's back buffer is still its bound render target (RENDER_TARGET; Unity moves it to
	// PRESENT afterwards, checked with the D3D12 debug layer). Aether draws into it as it is, changes no state and
	// declares none: passing the back buffer in ExecuteCommandList's state array every frame corrupted Unity's own
	// resource bookkeeping after a few thousand frames under the debug layer (docs/architecture/dx12-integration.md).
	if (backBuffer) {
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
		D3D12_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = viewFormat;
		rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		device_->CreateRenderTargetView(backBuffer.Get(), &rd, rtv);
		const D3D12_VIEWPORT viewport = {0, 0, float(bbDesc.Width), float(bbDesc.Height), 0, 1};
		const D3D12_RECT scissor = {0, 0, LONG(bbDesc.Width), LONG(bbDesc.Height)};
		auto bindTarget = [&] {
			list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			list->RSSetViewports(1, &viewport);
			list->RSSetScissorRects(1, &scissor);
		};

		// ---- pass: the integration test triangle
		{
			PassTiming& p = passes[kPassTriangle];
			double t0 = QpcSeconds();
			if (!wantTriangle) {
				p.note = "off (setting testTriangle)";
			} else if (!triPso_) {
				p.note = triNote_.empty() ? "no pipeline" : triNote_;
			} else {
				bindTarget();
				list->SetGraphicsRootSignature(triRoot_.Get());
				list->SetPipelineState(triPso_.Get());
				list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				list->IASetVertexBuffers(0, 1, &triVbView_);
				list->DrawInstanced(3, 1, 0, 0);
				p.executed = true;
				p.drawCalls = 1;
				passMask |= 1u << kPassTriangle;
				stamp();
			}
			p.cpuMs = (QpcSeconds() - t0) * 1000.0;
		}

		auto capturePass = [&] {
			PassTiming& p = passes[kPassCapture];
			double t0 = QpcSeconds();
			if (RecordCapture(ctx, backBuffer.Get(), capture)) {
				captureRecorded = true;
				p.executed = true;
				passMask |= 1u << kPassCapture;
				stamp();
			}
			p.cpuMs = (QpcSeconds() - t0) * 1000.0;
		};
		// NOTE on timestamp order: slots follow the order the passes ran in; a capture taken before the overlay swaps
		// the two, which CollectGpuResults does not know. Such a frame's two pass times are swapped (one frame only).
		if (wantCapture && !capture.withOverlay) {
			capturePass();
		}

		// ---- pass: the overlay
		{
			PassTiming& p = passes[kPassOverlay];
			double t0 = QpcSeconds();
			if (!wantOverlay) {
				p.note = "hidden";
			} else {
				bindTarget();
				uint32_t draws = 0;
				std::string note;
				overlayDrawn = overlay.Draw(clock_, hwnd_.load(), viewFormat, UINT(bbDesc.Width), bbDesc.Height, list, Completed(), draws, note);
				p.note = note;
				if (overlayDrawn) {
					p.executed = true;
					p.drawCalls = draws;
					passMask |= 1u << kPassOverlay;
					stamp();
				}
			}
			p.cpuMs = (QpcSeconds() - t0) * 1000.0;
		}

		if (wantCapture && capture.withOverlay) {
			capturePass();
		}
		if (!wantCapture) {
			passes[kPassCapture].note = "no capture requested";
		}
	}

	if (ts >= 0) {
		const uint32_t first = hasBegin ? 0 : 1;
		list->ResolveQueryData(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(ts) * kTsPerFrame + first, tsSlot - first, tsReadback_.Get(), (UINT64(ts) * kTsPerFrame + first) * sizeof(UINT64));
	}

	UINT64 fence = Submit(host, ctx, 0, nullptr);
	backBuffer.Reset(); // no reference to a back buffer outlives the frame (the swap chain can be resized)
	if (failed_.load()) {
		finish();
		return;
	}
	if (ts >= 0) {
		tsFrames_[ts].pending = true;
		tsFrames_[ts].frame = sample.frame;
		tsFrames_[ts].fence = fence;
		tsFrames_[ts].hasBegin = hasBegin;
		tsFrames_[ts].passMask = passMask;
	}
	if (overlayDrawn) {
		overlay.NoteSubmitted(fence);
	}
	if (captureRecorded) {
		capture.submitted = true;
		capture.fence = fence;
		capturePending_.push_back(std::move(capture));
	}
	for (const PassTiming& p : passes) {
		sample.drawCalls += p.drawCalls;
		sample.dispatches += p.dispatches;
	}
	finish();
}

void Renderer::SceneFrame(const HostGfx& host) {
	if (failed_.load() || !EnsureDevice(host)) {
		return;
	}
	if (HRESULT removed = device_->GetDeviceRemovedReason(); removed != S_OK) {
		// nothing more is recorded on a lost device; the report says what Aether was doing
		json report = SceneRenderer::Get().Stats();
		Log::Get().Write(Severity::Error, "Renderer", "E_SCENE_AT_DEVICE_LOSS", "The scene renderer's last frame before the device was lost", {}, report);
		Fail("E_DEVICE_REMOVED", "The D3D12 device was removed", removed);
		return;
	}
	const SettingsData& s = CurrentSettings();
	const bool draw = s.renderMode != "hdrp";
	if (!draw && !SceneStore::Get().HasWork()) {
		Profiler::Get().SceneIdle();
		return;
	}
	ComPtr<ID3D12Resource> backBuffer;
	D3D12_RESOURCE_DESC bbDesc = {};
	DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
	if (draw) {
		IDXGISwapChain* sc = host.swapChain ? host.swapChain() : nullptr;
		ComPtr<IDXGISwapChain3> sc3;
		if (sc && SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) {
			sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));
		}
		if (backBuffer) {
			bbDesc = backBuffer->GetDesc();
			viewFormat = ViewFormat(bbDesc.Format);
			if (bbDesc.SampleDesc.Count != 1) {
				backBuffer.Reset();
			}
		}
	}
	Context* ctx = Acquire(host);
	if (!ctx) {
		++skippedFrames_;
		return;
	}
	SceneRenderer::Target target;
	if (backBuffer) {
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
		D3D12_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = viewFormat;
		rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		device_->CreateRenderTargetView(backBuffer.Get(), &rd, rtv);
		target.backBuffer = backBuffer.Get();
		target.backBufferRtv = rtv;
		target.backBufferFormat = viewFormat;
		target.width = UINT(bbDesc.Width);
		target.height = bbDesc.Height;
	}
	// what this frame retires must outlive the list recorded now: this list's own serial
	const UINT64 retireFence = ctx->fence;
	const bool wrote = SceneRenderer::Get().Render(clock_, ctx->list.Get(), target, draw && backBuffer, retireFence, ++sceneFrame_, s);
	// Between the game's cameras and its overlay UI the back buffer is the bound render target (RENDER_TARGET): the
	// scene list uses it as it is and leaves it as it is. No states are declared to the host (see FrameEnd).
	const UINT64 fence = Submit(host, ctx, 0, nullptr);
	backBuffer.Reset();
	if (failed_.load()) {
		return;
	}
	SceneRenderer::Get().EndFrame(clock_, fence);
	if (!wrote) {
		Profiler::Get().SceneIdle();
	}
}

void Renderer::ReleaseDeviceObjects(const HostGfx* host, uint32_t waitMs) {
	bool leak = false;
	if (ready_.load() && lastSubmittedFence_ != 0) {
		// Releasing is only safe once the GPU is done with Aether's lists. Without a way to know, nothing is released.
		if (!host) {
			leak = true;
		} else if (Completed() >= lastSubmittedFence_) {
			leak = false;
		} else {
			// the markers tell when the GPU got through Aether's last list; the host's idle wait only helps it along
			if (host->waitIdle && waitMs > 0) {
				host->waitIdle(waitMs);
			}
			const double deadline = QpcSeconds() + waitMs / 1000.0;
			while (Completed() < lastSubmittedFence_ && QpcSeconds() < deadline) {
				Sleep(1);
			}
			leak = Completed() < lastSubmittedFence_;
		}
		if (leak) {
			Log::Get().Write(Severity::Warning, "Renderer", "W_SHUTDOWN_LEAK", "The GPU could not be confirmed idle on Aether's last submission: device objects are left unreleased instead of being freed under the GPU", {},
				{{"lastSubmittedFence", lastSubmittedFence_}, {"waitMs", waitMs}, {"hostAvailable", host != nullptr}});
		}
	}
	if (debugCookie_ && device_ && !leak) {
		ComPtr<ID3D12InfoQueue1> infoQueue;
		if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
			infoQueue->UnregisterMessageCallback(debugCookie_);
		}
	}
	debugCookie_ = 0;
	Overlay::Get().ReleaseDeviceObjects(leak);
	SceneRenderer::Get().Release(leak);
	if (tsReadback_ && tsMapped_ && !leak) {
		tsReadback_->Unmap(0, nullptr);
	}
	tsMapped_ = nullptr;
	if (leak) {
		for (auto& c : contexts_) {
			c->list.Detach();
			c->allocator.Detach();
		}
		rtvHeap_.Detach();
		triRoot_.Detach();
		triPso_.Detach();
		triVb_.Detach();
		tsHeap_.Detach();
		tsReadback_.Detach();
		for (Capture& c : capturePending_) {
			c.readback.Detach();
		}
	}
	if (leak) {
		markerUpload_.Detach();
		markerReadback_.Detach();
	} else {
		if (markerUpload_ && markerSource_) markerUpload_->Unmap(0, nullptr);
		if (markerReadback_ && markerDone_) markerReadback_->Unmap(0, nullptr);
	}
	markerUpload_.Reset();
	markerReadback_.Reset();
	markerSource_ = nullptr;
	markerDone_ = nullptr;
	completedSerial_ = 0;
	markedSerial_ = 0;
	completionFence_.Reset();
	nextSerial_ = 1;
	contexts_.clear();
	rtvHeap_.Reset();
	triRoot_.Reset();
	triPso_.Reset();
	triVb_.Reset();
	tsHeap_.Reset();
	tsReadback_.Reset();
	for (Capture& c : capturePending_) {
		SetCaptureStatus(c.id, "failed", WideToUtf8(c.file), 0, 0, "the renderer shut down before the capture finished");
	}
	capturePending_.clear();
	ResourceRegistry::Get().Clear();
	triVbId_ = tsReadbackId_ = 0;
	pipelineFormat_ = DXGI_FORMAT_UNKNOWN;
	tsCurrent_ = -1;
	tsCurrentHasBegin_ = false;
	lastSubmittedFence_ = 0;
	device_ = nullptr;
	GpuInfo::Get().Reset();
	if (ready_.exchange(false)) {
		AE_LOG(Info, "Renderer", "Device objects released%s", leak ? " (leaked on purpose)" : "");
	}
}

uint64_t Renderer::RequestCapture(const std::wstring& file, bool withOverlay) {
	std::lock_guard lock(captureMutex_);
	Capture c;
	c.id = nextCaptureId_++;
	c.file = file;
	c.withOverlay = withOverlay;
	captureStatus_[std::to_string(c.id)] = {{"id", c.id}, {"state", "pending"}, {"file", WideToUtf8(file)}};
	captureRequests_.push_back(std::move(c));
	return captureRequests_.back().id;
}

json Renderer::CaptureStatus(uint64_t id) {
	std::lock_guard lock(captureMutex_);
	auto it = captureStatus_.find(std::to_string(id));
	if (it == captureStatus_.end()) {
		return {{"id", id}, {"state", "unknown"}};
	}
	return *it;
}

json Renderer::State() {
	json j;
	j["state"] = failed_.load() ? "failed" : ready_.load() ? "ready" : "waiting for the first frame";
	j["mainRenderer"] = "HDRP (Unity). Aether runs alongside it and composites its passes onto the back buffer.";
	j["graphicsApi"] = "Direct3D 12";
	j["frame"] = Profiler::Get().FrameCount();
	std::lock_guard lock(stateMutex_);
	j["host"] = hostName_;
	j["backBuffer"] = backBufferInfo_;
	j["lastError"] = lastError_.empty() ? json(nullptr) : json(lastError_);
	return j;
}

json Renderer::RenderGraph() {
	json state = State();
	json timings = Profiler::Get().Passes();
	json passes = json::array();
	passes.push_back({{"name", "FrameBeginMarker"}, {"type", "marker"}, {"description", "GPU timestamp at the start of the host's frame"}, {"reads", json::array()}, {"writes", json::array({"Profiler.TimestampHeap"})}});
	passes.push_back({{"name", "HostFrame"}, {"type", "external"}, {"owner", "Unity HDRP"}, {"description", "The game's whole frame (HDRP cameras, UI). Not controlled by Aether in Phase 1."}, {"reads", json::array()}, {"writes", json::array({"BackBuffer"})}});
	static const char* descriptions[] = {"DX12 integration test triangle", "Dear ImGui overlay", "Copies the back buffer to a readback buffer (PNG)"};
	static const char* reads[] = {"TestTriangle.VertexBuffer", "ImGui buffers and font atlas", "BackBuffer"};
	static const char* writes[] = {"BackBuffer", "BackBuffer", "FrameCapture.Readback"};
	for (int p = 0; p < kPassCount; ++p) {
		json pass = {{"name", kPassNames[p]}, {"type", "raster"}, {"owner", "Aether"}, {"description", descriptions[p]}, {"reads", json::array({reads[p]})}, {"writes", json::array({writes[p]})}};
		if (timings["passes"].is_array() && timings["passes"].size() > size_t(p)) {
			pass["lastFrame"] = timings["passes"][size_t(p)];
		}
		passes.push_back(pass);
	}
	passes.push_back({{"name", "HostPresent"}, {"type", "external"}, {"owner", "Unity"}, {"description", "Unity transitions the back buffer to PRESENT and presents"}, {"reads", json::array({"BackBuffer"})}, {"writes", json::array()}});
	json resources = json::array();
	resources.push_back({{"name", "BackBuffer"}, {"owner", "host"}, {"lifetime", "external"}, {"stateOnEntry", "RENDER_TARGET"}, {"stateOnExit", "RENDER_TARGET"}, {"info", state["backBuffer"]}});
	for (const ResourceRecord& r : ResourceRegistry::Get().Snapshot()) {
		resources.push_back({{"name", r.name}, {"owner", "aether"}, {"lifetime", "persistent"}, {"category", CategoryName(r.category)}, {"bytes", r.bytes}});
	}
	json scene = SceneRenderer::Get().GraphDescription();
	if (scene.contains("passes")) {
		// last frame's timings next to the scene graph's passes
		for (auto& pass : scene["passes"]) {
			for (const auto& t : timings["scenePasses"]) {
				if (t.value("name", "") == pass.value("name", "")) {
					pass["lastFrame"] = t;
				}
			}
		}
	}
	return {{"schema", "aether.rendergraph/2"}, {"phase", 2}, {"frame", timings["frame"]}, {"renderer", state}, {"renderMode", Settings::Get().Snapshot().renderMode},
		{"passes", passes}, {"resources", resources}, {"sceneGraph", scene}, {"sceneStats", SceneRenderer::Get().Stats()},
		{"note", "passes / resources: the frame-end list on the host's back buffer (test triangle, overlay, capture). sceneGraph: Aether's scene renderer (render mode aether / compare), a render graph with transient resources and explicit states; it runs after the host's cameras and before the host's UI."}};
}

} // namespace aether
