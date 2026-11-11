// AetherHost: Aether's runtime on a plain window with its own D3D12 device, standing in for Unity. It drives Aether the
// way the game does (frame begin, the "game's" own rendering, frame end with the back buffer in RENDER_TARGET, present)
// with the D3D12 debug layer on, so the integration rules are checked without starting the game:
//   AetherHost --data <dir> [--seconds 10] [--frames N] [--overlay] [--resize] [--capture] [--warp] [--no-debug] [--hidden]
// <dir>\shaders must hold the shaders. Exit code 0 = no D3D12 errors, no leaked objects, no Aether failure.
#include "Core/Common.h"
#include "Core/Config.h"
#include "Core/Log.h"
#include "ImGui/Overlay.h"
#include "Renderer/Renderer.h"
#include "Resources/GpuResources.h"
#include "Runtime.h"
#include "Scene/SceneStore.h"
#include "Session.h"
#include "ValidationScene.h"

#include <d3d12sdklayers.h>

using namespace aether;

namespace {

const UINT kBuffers = 2;

struct Host {
	HWND hwnd = nullptr;
	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandQueue> queue;
	ComPtr<IDXGISwapChain3> swapChain;
	ComPtr<ID3D12Fence> frameFence;
	UINT64 frameFenceValue = 0; // last signalled
	ComPtr<ID3D12DescriptorHeap> rtvHeap;
	ComPtr<ID3D12CommandAllocator> allocators[kBuffers];
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12InfoQueue> infoQueue;
	D3D12_RESOURCE_STATES backBufferState[kBuffers] = {D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT};
	HANDLE fenceEvent = nullptr;
	UINT width = 1280, height = 720;
	bool resized = false;
	uint64_t errors = 0;
} g;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_SIZE:
		if (wParam != SIZE_MINIMIZED) {
			g.resized = true;
		}
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void Die(const char* what, HRESULT hr) {
	printf("HOST FAILED: %s: %s\n", what, HrToString(hr).c_str());
	ExitProcess(2);
}

void WaitForGpu() {
	const UINT64 value = ++g.frameFenceValue;
	g.queue->Signal(g.frameFence.Get(), value);
	if (g.frameFence->GetCompletedValue() < value) {
		g.frameFence->SetEventOnCompletion(value, g.fenceEvent);
		WaitForSingleObject(g.fenceEvent, 5000);
	}
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = resource;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	list->ResourceBarrier(1, &b);
}

// Prints what the debug layer said since the last call; counts errors and corruptions.
void DrainInfoQueue() {
	if (!g.infoQueue) {
		return;
	}
	UINT64 count = g.infoQueue->GetNumStoredMessages();
	for (UINT64 i = 0; i < count; ++i) {
		SIZE_T size = 0;
		g.infoQueue->GetMessage(i, nullptr, &size);
		std::vector<char> buffer(size);
		D3D12_MESSAGE* m = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
		if (FAILED(g.infoQueue->GetMessage(i, m, &size))) {
			continue;
		}
		if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
			bool error = m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
			g.errors += error ? 1 : 0;
			printf("D3D12 %s: %s\n", error ? "ERROR" : "warning", m->pDescription);
		}
	}
	g.infoQueue->ClearStoredMessages();
}

void Resize() {
	RECT rc;
	GetClientRect(g.hwnd, &rc);
	UINT w = UINT(rc.right - rc.left), h = UINT(rc.bottom - rc.top);
	if (w == 0 || h == 0 || (w == g.width && h == g.height)) {
		return;
	}
	WaitForGpu();
	HRESULT hr = g.swapChain->ResizeBuffers(kBuffers, w, h, DXGI_FORMAT_UNKNOWN, 0);
	if (FAILED(hr)) {
		// this is what happens when someone still holds a back buffer
		Die("ResizeBuffers (a back buffer reference is still held?)", hr);
	}
	g.width = w;
	g.height = h;
	for (auto& s : g.backBufferState) {
		s = D3D12_RESOURCE_STATE_PRESENT;
	}
	printf("resized to %ux%u\n", w, h);
}

bool Arg(int argc, wchar_t** argv, const wchar_t* name, const wchar_t** value = nullptr) {
	for (int i = 1; i < argc; ++i) {
		if (wcscmp(argv[i], name) == 0) {
			if (value) {
				if (i + 1 >= argc) {
					return false;
				}
				*value = argv[i + 1];
			}
			return true;
		}
	}
	return false;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
	const wchar_t* dataDir = nullptr;
	const wchar_t* text = nullptr;
	if (!Arg(argc, argv, L"--data", &dataDir)) {
		printf("usage: AetherHost --data <dir> [--seconds 10] [--frames N] [--overlay] [--resize] [--capture] [--warp] [--no-debug] [--hidden]\n");
		return 2;
	}
	double seconds = Arg(argc, argv, L"--seconds", &text) ? _wtof(text) : 10.0;
	long long maxFrames = Arg(argc, argv, L"--frames", &text) ? _wtoi64(text) : -1;
	const bool overlay = Arg(argc, argv, L"--overlay"), resizeTest = Arg(argc, argv, L"--resize"), capture = Arg(argc, argv, L"--capture");
	const bool warp = Arg(argc, argv, L"--warp"), debugLayer = !Arg(argc, argv, L"--no-debug"), hidden = Arg(argc, argv, L"--hidden");
	// --scene: the validation scene through the scene data contract, drawn by Aether's scene renderer
	const bool sceneTest = Arg(argc, argv, L"--scene");
	const wchar_t* modeText = nullptr;
	const std::string sceneMode = Arg(argc, argv, L"--mode", &modeText) ? WideToUtf8(modeText) : "aether";

	// ---- window
	WNDCLASSW wc = {};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"AetherHost";
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	RegisterClassW(&wc);
	RECT rc = {0, 0, LONG(g.width), LONG(g.height)};
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
	g.hwnd = CreateWindowW(L"AetherHost", L"AetherHost (test host for the Aether runtime)", WS_OVERLAPPEDWINDOW, 60, 60, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
	ShowWindow(g.hwnd, hidden ? SW_SHOWNOACTIVATE : SW_SHOW);

	// ---- device
	if (debugLayer) {
		ComPtr<ID3D12Debug> debug;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
			debug->EnableDebugLayer();
		} else {
			printf("the D3D12 debug layer is not installed (Graphics Tools): running without validation\n");
		}
	}
	ComPtr<IDXGIFactory4> factory;
	HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
	if (FAILED(hr)) Die("CreateDXGIFactory2", hr);
	ComPtr<IDXGIAdapter1> adapter;
	if (warp) {
		factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
	}
	hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device));
	if (FAILED(hr)) Die("D3D12CreateDevice", hr);
	if (SUCCEEDED(g.device.As(&g.infoQueue))) {
		g.infoQueue->SetMessageCountLimit(4096);
	}
	D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
	hr = g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue));
	if (FAILED(hr)) Die("CreateCommandQueue", hr);
	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = g.width;
	sd.Height = g.height;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = kBuffers;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	ComPtr<IDXGISwapChain1> sc1;
	hr = factory->CreateSwapChainForHwnd(g.queue.Get(), g.hwnd, &sd, nullptr, nullptr, &sc1);
	if (FAILED(hr)) Die("CreateSwapChainForHwnd", hr);
	sc1.As(&g.swapChain);
	sc1.Reset();
	factory->MakeWindowAssociation(g.hwnd, DXGI_MWA_NO_ALT_ENTER);
	factory.Reset();
	adapter.Reset();
	g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.frameFence));
	g.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
	g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtvHeap));
	for (auto& a : g.allocators) {
		g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
	}
	g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocators[0].Get(), nullptr, IID_PPV_ARGS(&g.list));
	g.list->Close();

	// ---- Aether
	std::wstring data = dataDir;
	session::Start(data, data + L"\\config");
	if (overlay) {
		Overlay::Get().SetVisible(true);
	}
	HostGfx host;
	host.name = "AetherHost";
	host.device = g.device.Get();
	host.queue = g.queue.Get();
	host.swapChain = [] { return static_cast<IDXGISwapChain*>(g.swapChain.Get()); };
	host.completedFenceValue = [] { return g.frameFence->GetCompletedValue(); };
	host.nextFenceValue = [] { return g.frameFenceValue + 1; };
	host.execute = [](ID3D12GraphicsCommandList* list, int count, HostResourceState* states) -> UINT64 {
		// like Unity: bring the declared resources to `expected`, run the list, remember `current`
		const UINT index = g.swapChain->GetCurrentBackBufferIndex();
		ComPtr<ID3D12Resource> backBuffer;
		g.swapChain->GetBuffer(index, IID_PPV_ARGS(&backBuffer));
		for (int i = 0; i < count; ++i) {
			if (states[i].resource != backBuffer.Get()) {
				printf("HOST: a state was declared for a resource the host does not track\n");
				g.errors++;
				continue;
			}
			if (g.backBufferState[index] != states[i].expected) {
				// a transition list of the host's own (kept simple: a throwaway allocator)
				ComPtr<ID3D12CommandAllocator> alloc;
				ComPtr<ID3D12GraphicsCommandList> fix;
				g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
				g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&fix));
				Transition(fix.Get(), backBuffer.Get(), g.backBufferState[index], states[i].expected);
				fix->Close();
				ID3D12CommandList* lists[] = {fix.Get()};
				g.queue->ExecuteCommandLists(1, lists);
				WaitForGpu();
			}
			g.backBufferState[index] = states[i].current;
		}
		ID3D12CommandList* lists[] = {list};
		g.queue->ExecuteCommandLists(1, lists);
		return g.frameFenceValue + 1;
	};
	host.waitIdle = [](uint32_t) {
		WaitForGpu();
		return true;
	};

	host::ValidationScene scene;
	uint64_t sceneCaptures[3] = {0, 0, 0};
	if (sceneTest) {
		if (!scene.Create(g.device.Get(), g.queue.Get())) {
			Die("validation scene", E_FAIL);
		}
		Settings::Get().Set("renderMode", sceneMode, "host");
		Settings::Get().Set("debugView", "none", "host");
		scene.Register();
		scene.PrintProbes(g.width, g.height);
		CreateDirectoryW((data + L"\\captures").c_str(), nullptr);
	}

	// ---- frames
	const double start = QpcSeconds();
	long long frame = 0;
	bool running = true, captured = false, resizedOnce = false;
	uint64_t captureId = 0;
	while (running) {
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				running = false;
			}
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		const double elapsed = QpcSeconds() - start;
		if (elapsed >= seconds || (maxFrames >= 0 && frame >= maxFrames)) {
			break;
		}
		if (resizeTest && !resizedOnce && elapsed > seconds * 0.5) {
			resizedOnce = true;
			SetWindowPos(g.hwnd, nullptr, 0, 0, 1000, 620, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
		if (g.resized) {
			g.resized = false;
			Resize();
		}
		session::MainThreadUpdate();
		session::FrameBegin(host);

		// the "game": clear the back buffer to a slowly changing colour
		const UINT index = g.swapChain->GetCurrentBackBufferIndex();
		{
			ComPtr<ID3D12Resource> backBuffer;
			g.swapChain->GetBuffer(index, IID_PPV_ARGS(&backBuffer));
			g.allocators[index]->Reset();
			g.list->Reset(g.allocators[index].Get(), nullptr);
			Transition(g.list.Get(), backBuffer.Get(), g.backBufferState[index], D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.backBufferState[index] = D3D12_RESOURCE_STATE_RENDER_TARGET;
			D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
			g.device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtv);
			const float t = float(elapsed);
			const float color[4] = {0.10f + 0.05f * sinf(t), 0.12f, 0.18f + 0.05f * cosf(t * 0.7f), 1.0f};
			g.list->ClearRenderTargetView(rtv, color, 0, nullptr);
			g.list->Close();
			ID3D12CommandList* lists[] = {g.list.Get()};
			g.queue->ExecuteCommandLists(1, lists);
		}

		if (sceneTest) {
			// what the managed side does every frame: describe the frame, then let the scene renderer run after the
			// "game" drew and before its "UI"
			scene.Churn(uint32_t(frame));
			scene.SubmitFrame(g.width, g.height, uint32_t(frame), float(elapsed));
			session::SceneFrame(host);
			if (frame == 40) {
				sceneCaptures[0] = Renderer::Get().RequestCapture(data + L"\\captures\\scene-lit.png", false);
			} else if (frame == 60) {
				Settings::Get().Set("debugView", "normal", "host");
			} else if (frame == 80) {
				sceneCaptures[1] = Renderer::Get().RequestCapture(data + L"\\captures\\scene-normal.png", false);
			} else if (frame == 90) {
				Settings::Get().Set("debugView", "none", "host");
			} else if (frame == 140) {
				sceneCaptures[2] = Renderer::Get().RequestCapture(data + L"\\captures\\scene-after.png", false);
			}
		}
		if (capture && !captured && frame == 30) {
			captured = true;
			captureId = Renderer::Get().RequestCapture(data + L"\\captures\\host-capture.png", true);
			CreateDirectoryW((data + L"\\captures").c_str(), nullptr);
		}
		session::FrameEnd(host);

		// present
		{
			ComPtr<ID3D12Resource> backBuffer;
			g.swapChain->GetBuffer(index, IID_PPV_ARGS(&backBuffer));
			WaitForGpu(); // the clear list is done before its allocator is reused (a test host, not a fast one)
			g.allocators[index]->Reset();
			g.list->Reset(g.allocators[index].Get(), nullptr);
			if (g.backBufferState[index] != D3D12_RESOURCE_STATE_PRESENT) {
				Transition(g.list.Get(), backBuffer.Get(), g.backBufferState[index], D3D12_RESOURCE_STATE_PRESENT);
			}
			g.backBufferState[index] = D3D12_RESOURCE_STATE_PRESENT;
			g.list->Close();
			ID3D12CommandList* lists[] = {g.list.Get()};
			g.queue->ExecuteCommandLists(1, lists);
		}
		hr = g.swapChain->Present(1, 0);
		if (FAILED(hr)) {
			printf("Present failed: %s\n", HrToString(hr).c_str());
			g.errors++;
			break;
		}
		WaitForGpu();
		DrainInfoQueue();
		++frame;
	}

	// ---- shutdown, the way the game does it
	std::string captureState = captureId ? Renderer::Get().CaptureStatus(captureId).value("state", "") : "";
	for (int i = 0; captureId && i < 200 && captureState != "done" && captureState != "failed"; ++i) {
		Sleep(10);
		captureState = Renderer::Get().CaptureStatus(captureId).value("state", "");
	}
	bool sceneCapturesDone = true;
	for (uint64_t id : sceneCaptures) {
		for (int i = 0; sceneTest && id && i < 300 && Renderer::Get().CaptureStatus(id).value("state", "") != "done" && Renderer::Get().CaptureStatus(id).value("state", "") != "failed"; ++i) {
			Sleep(10);
		}
		sceneCapturesDone = sceneCapturesDone && (!sceneTest || (id && Renderer::Get().CaptureStatus(id).value("state", "") == "done"));
	}
	if (sceneTest) {
		printf("scene stats: %s\n", Dump(SceneStore::Get().Summary()).c_str());
		printf("gpu resources: %s\n", Dump(GpuResources::Get().Report()).c_str());
	}
	const bool failed = Renderer::Get().Failed() || session::Dead();
	const uint32_t logErrors = Log::Get().ErrorCount();
	session::ReleaseOnRenderThread(host, 2000);
	session::Stop();
	scene.Release();
	WaitForGpu();
	DrainInfoQueue();

	g.list.Reset();
	for (auto& a : g.allocators) {
		a.Reset();
	}
	g.rtvHeap.Reset();
	g.frameFence.Reset();
	g.swapChain.Reset();
	g.queue.Reset();
	g.infoQueue.Reset();
	host = HostGfx();
	// every object created on the device holds a reference to it: 0 left = nothing leaked
	ULONG refs = g.device.Reset();
	DestroyWindow(g.hwnd);

	printf("frames: %lld\n", frame);
	printf("d3d12 errors: %llu\n", static_cast<unsigned long long>(g.errors));
	printf("aether log errors: %u\n", logErrors);
	printf("aether failed: %s\n", failed ? "yes" : "no");
	printf("device references left: %lu\n", refs);
	if (capture) {
		printf("capture: %s\n", captureState.c_str());
	}
	if (sceneTest) {
		printf("scene captures: %s\n", sceneCapturesDone ? "done" : "MISSING");
	}
	const bool ok = g.errors == 0 && !failed && refs == 0 && logErrors == 0 && (!capture || captureState == "done") && sceneCapturesDone;
	printf(ok ? "HOST OK\n" : "HOST FAILED\n");
	return ok ? 0 : 1;
}
