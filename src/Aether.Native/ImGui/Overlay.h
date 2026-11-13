// The in-game overlay (Dear ImGui, its DX12 and Win32 backends).
//
// Threads: the ImGui frame is built and recorded on the host's render thread. The game window's procedure (main
// thread) never touches ImGui: it queues the input messages, and the render thread replays them before the next ImGui
// frame. The main thread therefore never waits for the render thread (and cannot deadlock with it).
// Hidden = no ImGui frame, no queueing, no input taken from the game.
#pragma once
#include "../Core/Common.h"
#include "../Device/Host.h"

struct ImGui_ImplDX12_InitInfo;

namespace aether {

class Overlay {
public:
	static Overlay& Get();

	// ---- any thread
	bool Visible() const { return visible_.load(); }
	void SetVisible(bool visible);
	void Toggle() { SetVisible(!visible_.load()); }
	bool WantsFrame() const;  // visible, or an approval prompt is waiting
	bool WantsInput() const { return WantsFrame() && (wantsMouse_.load() || wantsKeyboard_.load()); }
	// bit 0: the overlay uses the mouse, bit 1: the keyboard (the free camera takes both while its input is live)
	int InputFlags() const;
	// ---- free camera (the game window's procedure switches it with its hotkey and gathers its input)
	bool FreeCamera() const { return freeCamera_.load(); }
	// the free camera has the mouse and keyboard: on, and the overlay closed
	bool FreeCameraLive() const { return freeCamera_.load() && !visible_.load(); }
	// Main thread (managed, once a frame): the input gathered since the last call. Returns bit 0 on, bit 1 live, bits
	// 8.. the keys held (FreeCameraKey). out: mouse x, mouse y (counts), wheel (notches), speed, sensitivity.
	// `fovDegrees`: the camera's field of view, for the display.
	int TakeFreeCameraInput(float fovDegrees, float out[5]);
	void FreeCameraStatus(); // (render thread, inside the overlay's UI: its state and the path tracing's samples)
	enum FreeCameraKey { kKeyForward = 1 << 8, kKeyLeft = 1 << 9, kKeyBack = 1 << 10, kKeyRight = 1 << 11, kKeyDown = 1 << 12, kKeyUp = 1 << 13, kKeyFast = 1 << 14, kKeySlow = 1 << 15, kKeyResetFov = 1 << 16 };

	// ---- main thread: subclass the game window for input and the hotkey (restored by DetachWindow)
	void AttachWindow(HWND hwnd);
	void DetachWindow();
	bool Attached() const { return hooked_ != nullptr; }

	// ---- render thread
	// Records the overlay into `list` (render target already bound). `completedFence` is the host fence's completed
	// value: the overlay skips a frame rather than reuse buffers of a list that has not run yet. False = nothing drawn.
	bool Draw(const HostGfx& host, HWND hwnd, DXGI_FORMAT rtvFormat, UINT width, UINT height, ID3D12GraphicsCommandList* list, UINT64 completedFence, uint32_t& drawCalls, std::string& note);
	void NoteSubmitted(UINT64 fence); // the fence value of the list the last Draw went into
	// leak = true: the GPU may still use the overlay's buffers (the host fence did not advance): drop them unreleased
	void ReleaseDeviceObjects(bool leak);

private:
	struct Message { HWND hwnd; UINT msg; WPARAM wParam; LPARAM lParam; };
	static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	bool EnsureContext(const HostGfx& host, HWND hwnd, DXGI_FORMAT rtvFormat);
	void ShutdownBackends();
	void BuildUi(UINT width, UINT height);
	bool HandleFreeCamera(UINT msg, WPARAM wParam, LPARAM lParam); // true = taken from the game
	static void SrvAlloc(::ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
	static void SrvFree(::ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);

	static const int kFramesInFlight = 8;
	static const UINT kSrvCount = 64;
	std::atomic<bool> visible_{false};
	std::atomic<bool> wantsMouse_{false}, wantsKeyboard_{false};
	std::atomic<int> hotkey_{0x79};
	std::atomic<int> freeCameraHotkey_{0x7A};
	std::atomic<bool> freeCamera_{false};
	std::atomic<uint32_t> freeCameraKeys_{0};
	std::atomic<int> mouseX_{0}, mouseY_{0}, wheel_{0};
	std::atomic<bool> rawMouse_{false}; // raw mouse input arrives (WM_INPUT): its relative motion is used, not the cursor's
	std::atomic<float> freeCameraFov_{0};
	int lastCursorX_ = INT_MIN, lastCursorY_ = INT_MIN; // (main thread)
	bool cursorCentred_ = false; // (main thread) the cursor was put in the window's middle last frame
	bool loggedInput_ = false;

	std::mutex queueMutex_;
	std::vector<Message> queue_;

	std::mutex mutex_; // Draw (render thread) against ReleaseDeviceObjects (shutdown may come from another thread)
	bool context_ = false;
	bool backends_ = false;
	HWND backendWindow_ = nullptr;
	DXGI_FORMAT backendFormat_ = DXGI_FORMAT_UNKNOWN;
	ID3D12Device* device_ = nullptr;
	ComPtr<ID3D12DescriptorHeap> srvHeap_;
	std::vector<UINT> srvFree_;
	UINT srvSize_ = 0;
	UINT64 slotFence_[kFramesInFlight] = {};
	uint64_t draws_ = 0; // overlay draws since the backends started (the DX12 backend's frame index)
	bool drawPending_ = false;
	float appliedScale_ = 0;

	HWND hooked_ = nullptr;
	WNDPROC previousProc_ = nullptr;
};

} // namespace aether
