#include "Overlay.h"
#include "../Core/Config.h"
#include "../Core/Log.h"
#include "../Device/GpuInfo.h"
#include "../Diagnostics.h"
#include "../Profiler/Profiler.h"
#include "../Renderer/Renderer.h"
#include "../Renderer/SceneRenderer.h"
#include "../Resources/GpuResources.h"
#include "../Resources/ResourceRegistry.h"
#include "../Scene/SceneStore.h"
#include "../Runtime.h"
#include "../Shaders/ShaderManager.h"

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <windowsx.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace aether {

Overlay& Overlay::Get() {
	static Overlay* o = new Overlay();
	return *o;
}

void Overlay::SetVisible(bool visible) {
	visible_.store(visible);
	if (!visible) {
		wantsMouse_.store(false);
		wantsKeyboard_.store(false);
	}
}

static bool IsMouseMessage(UINT msg);

bool Overlay::WantsFrame() const {
	return visible_.load() || Runtime::Get().ApprovalPending();
}

int Overlay::InputFlags() const {
	if (FreeCameraLive()) {
		return 3; // the free camera has the mouse and the keyboard: the game must not drive or steer the menus with them
	}
	return WantsFrame() ? (wantsMouse_.load() ? 1 : 0) | (wantsKeyboard_.load() ? 2 : 0) : 0;
}

int Overlay::TakeFreeCameraInput(float fovDegrees, float out[5]) {
	freeCameraFov_.store(fovDegrees);
	const SettingsData s = Settings::Get().Snapshot();
	freeCameraHotkey_.store(s.freeCameraHotkey);
	const bool live = FreeCameraLive();
	uint32_t keys = freeCameraKeys_.load();
	if (live && hooked_ && GetForegroundWindow() == hooked_) {
		// The keys' state and the cursor read directly: the game takes the keyboard and the mouse through raw input
		// (Rewired), and the letter keys and mouse motion did not all arrive as window messages (the camera did not move).
		auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
		if (down('W') || down(VK_UP)) keys |= kKeyForward;
		if (down('A') || down(VK_LEFT)) keys |= kKeyLeft;
		if (down('S') || down(VK_DOWN)) keys |= kKeyBack;
		if (down('D') || down(VK_RIGHT)) keys |= kKeyRight;
		if (down('Q') || down('C')) keys |= kKeyDown;
		if (down('E') || down(VK_SPACE)) keys |= kKeyUp;
		if (down(VK_SHIFT)) keys |= kKeyFast;
		if (down(VK_CONTROL)) keys |= kKeySlow;
		if (down('R')) keys |= kKeyResetFov;
		if (!rawMouse_.load()) {
			// the cursor's way from the window's middle since the last frame, then back to the middle
			RECT rc;
			POINT centre = {0, 0}, cursor;
			if (GetClientRect(hooked_, &rc) && GetCursorPos(&cursor)) {
				centre = {(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
				ClientToScreen(hooked_, &centre);
				if (cursorCentred_) {
					mouseX_.fetch_add(cursor.x - centre.x);
					mouseY_.fetch_add(cursor.y - centre.y);
				}
				SetCursorPos(centre.x, centre.y);
				cursorCentred_ = true;
			}
		}
	} else {
		cursorCentred_ = false;
	}
	if (live && keys != 0 && !loggedInput_) {
		loggedInput_ = true;
		AE_LOG(Info, "Overlay", "Free camera: keys arrive (0x%X)", keys);
	}
	const int mx = mouseX_.exchange(0), my = mouseY_.exchange(0), wheel = wheel_.exchange(0);
	out[0] = live ? float(mx) : 0.0f;
	out[1] = live ? float(my) : 0.0f;
	out[2] = live ? float(wheel) / float(WHEEL_DELTA) : 0.0f;
	out[3] = s.freeCameraSpeed;
	out[4] = s.freeCameraSensitivity;
	int result = (freeCamera_.load() ? 1 : 0) | (live ? 2 : 0);
	if (live) {
		result |= int(keys);
	}
	return result;
}

// The free camera's input, while it has it (on, the overlay closed): every key and mouse message is kept from the
// game. Keys held are remembered, the mouse's motion and wheel are added up for TakeFreeCameraInput.
bool Overlay::HandleFreeCamera(UINT msg, WPARAM wParam, LPARAM lParam) {
	auto keyBit = [](WPARAM vk) -> uint32_t {
		switch (vk) {
		case 'W': case VK_UP: return kKeyForward;
		case 'A': case VK_LEFT: return kKeyLeft;
		case 'S': case VK_DOWN: return kKeyBack;
		case 'D': case VK_RIGHT: return kKeyRight;
		case 'Q': case 'C': return kKeyDown;
		case 'E': case VK_SPACE: return kKeyUp;
		case VK_SHIFT: return kKeyFast;
		case VK_CONTROL: return kKeySlow;
		case 'R': return kKeyResetFov;
		default: return 0;
		}
	};
	switch (msg) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		freeCameraKeys_.fetch_or(keyBit(wParam));
		return true;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		freeCameraKeys_.fetch_and(~keyBit(wParam));
		return true;
	case WM_CHAR:
	case WM_SYSCHAR:
	case WM_DEADCHAR:
		return true;
	case WM_KILLFOCUS:
		freeCameraKeys_.store(0); // (a key let go in another window never comes back as a key up)
		return false;
	case WM_INPUT: {
		// relative motion straight from the mouse (the game registered for raw input): not stopped by the screen's edge
		UINT size = 0;
		GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
		alignas(8) BYTE buffer[256];
		if (size > 0 && size <= sizeof(buffer) && GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == size) {
			const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buffer);
			if (raw->header.dwType == RIM_TYPEMOUSE && (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
				mouseX_.fetch_add(raw->data.mouse.lLastX);
				mouseY_.fetch_add(raw->data.mouse.lLastY);
				if (!rawMouse_.exchange(true)) {
					AE_LOG(Info, "Overlay", "Free camera: the mouse's raw input is used for looking round");
				}
			}
		}
		return true;
	}
	case WM_MOUSEMOVE: {
		// (without raw input: the cursor's motion, ignoring jumps such as the game putting it back in the middle)
		const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
		// (without raw input the cursor is read and put back in the middle once a frame: TakeFreeCameraInput)
		lastCursorX_ = x;
		lastCursorY_ = y;
		return true;
	}
	case WM_MOUSEWHEEL:
		wheel_.fetch_add(GET_WHEEL_DELTA_WPARAM(wParam));
		return true;
	default:
		return IsMouseMessage(msg) && msg != WM_MOUSELEAVE && msg != WM_NCMOUSELEAVE && msg != WM_NCMOUSEMOVE;
	}
}

// ------------------------------------------------------------------------------------------------ window procedure

void Overlay::AttachWindow(HWND hwnd) {
	if (!hwnd || hooked_ == hwnd) {
		return;
	}
	if (GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) {
		return; // only from the thread that owns the window
	}
	DetachWindow();
	SetLastError(0);
	LONG_PTR previous = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Overlay::WndProc));
	if (previous == 0) {
		AE_LOG_CODE(Error, "Overlay", "E_WNDPROC", "Could not subclass the game window (error %lu): the overlay has no input", GetLastError());
		return;
	}
	previousProc_ = reinterpret_cast<WNDPROC>(previous);
	hooked_ = hwnd;
	{
		const SettingsData s = Settings::Get().Snapshot();
		hotkey_.store(s.overlayHotkey);
		freeCameraHotkey_.store(s.freeCameraHotkey);
	}
	AE_LOG(Info, "Overlay", "Input attached to the game window (hotkey: virtual key %d)", hotkey_.load());
}

void Overlay::DetachWindow() {
	if (!hooked_) {
		return;
	}
	if (IsWindow(hooked_)) {
		LONG_PTR current = GetWindowLongPtrW(hooked_, GWLP_WNDPROC);
		if (current == reinterpret_cast<LONG_PTR>(&Overlay::WndProc)) {
			SetWindowLongPtrW(hooked_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previousProc_));
		} else {
			// someone subclassed the window after Aether: taking Aether's procedure out of the middle of the chain is
			// not possible, so it stays as a pass-through (the DLL is never unloaded while the game runs)
			AE_LOG(Warning, "Overlay", "Another window procedure was installed after Aether's: Aether's stays in the chain as a pass-through");
			SetVisible(false);
			return;
		}
	}
	hooked_ = nullptr;
	previousProc_ = nullptr;
}

LRESULT CALLBACK Overlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	return Overlay::Get().HandleMessage(hwnd, msg, wParam, lParam);
}

static bool IsMouseMessage(UINT msg) {
	return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSELEAVE || msg == WM_NCMOUSEMOVE || msg == WM_NCMOUSELEAVE;
}

static bool IsKeyboardMessage(UINT msg) {
	return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_CHAR || msg == WM_SYSCHAR || msg == WM_DEADCHAR;
}

LRESULT Overlay::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	WNDPROC next = previousProc_;
	if (!next) {
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
	if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && int(wParam) == hotkey_.load()) {
		if (!(lParam & (1 << 30))) { // not an auto-repeat
			Toggle();
		}
		return 0;
	}
	if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && int(wParam) == freeCameraHotkey_.load()) {
		if (!(lParam & (1 << 30))) {
			const bool on = !freeCamera_.load();
			freeCameraKeys_.store(0);
			mouseX_.store(0);
			mouseY_.store(0);
			wheel_.store(0);
			lastCursorX_ = lastCursorY_ = INT_MIN;
			cursorCentred_ = false;
			freeCamera_.store(on);
			AE_LOG(Info, "Overlay", "Free camera %s", on ? "on" : "off");
		}
		return 0;
	}
	if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && (int(wParam) == hotkey_.load() || int(wParam) == freeCameraHotkey_.load())) {
		return 0;
	}
	if (FreeCameraLive() && HandleFreeCamera(msg, wParam, lParam)) {
		return msg == WM_INPUT ? DefWindowProcW(hwnd, msg, wParam, lParam) : 0; // (raw input must still be cleaned up)
	}
	if (!visible_.load() && !Runtime::Get().ApprovalPending()) {
		return CallWindowProcW(next, hwnd, msg, wParam, lParam);
	}
	const bool mouse = IsMouseMessage(msg), keyboard = IsKeyboardMessage(msg);
	if (mouse || keyboard || msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_INPUTLANGCHANGE) {
		std::lock_guard lock(queueMutex_);
		if (queue_.size() < 512) {
			queue_.push_back({hwnd, msg, wParam, lParam});
		}
	}
	// what the overlay uses does not reach the game
	if ((mouse && msg != WM_MOUSELEAVE && msg != WM_NCMOUSELEAVE && wantsMouse_.load()) || (keyboard && wantsKeyboard_.load())) {
		return 0;
	}
	if (msg == WM_INPUT && (wantsMouse_.load() || wantsKeyboard_.load())) {
		return DefWindowProcW(hwnd, msg, wParam, lParam); // raw input must still be cleaned up
	}
	return CallWindowProcW(next, hwnd, msg, wParam, lParam);
}

// ------------------------------------------------------------------------------------------------ device objects

void Overlay::SrvAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
	Overlay* self = static_cast<Overlay*>(info->UserData);
	if (self->srvFree_.empty()) {
		AE_LOG_CODE(Error, "Overlay", "E_DESCRIPTOR_HEAP_FULL", "The overlay's SRV heap is full (%u descriptors)", kSrvCount);
		*cpu = self->srvHeap_->GetCPUDescriptorHandleForHeapStart();
		*gpu = self->srvHeap_->GetGPUDescriptorHandleForHeapStart();
		return;
	}
	UINT index = self->srvFree_.back();
	self->srvFree_.pop_back();
	cpu->ptr = self->srvHeap_->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(index) * self->srvSize_;
	gpu->ptr = self->srvHeap_->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(index) * self->srvSize_;
}

void Overlay::SrvFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
	Overlay* self = static_cast<Overlay*>(info->UserData);
	if (!self->srvHeap_) {
		return;
	}
	UINT index = UINT((cpu.ptr - self->srvHeap_->GetCPUDescriptorHandleForHeapStart().ptr) / self->srvSize_);
	self->srvFree_.push_back(index);
}

void Overlay::ShutdownBackends() {
	if (backends_) {
		ImGui_ImplDX12_Shutdown();
		ImGui_ImplWin32_Shutdown();
		backends_ = false;
	}
	srvHeap_.Reset();
	srvFree_.clear();
	draws_ = 0;
	drawPending_ = false;
	for (UINT64& f : slotFence_) {
		f = 0;
	}
}

bool Overlay::EnsureContext(const HostGfx& host, HWND hwnd, DXGI_FORMAT rtvFormat) {
	if (!hwnd) {
		return false;
	}
	if (!context_) {
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		static std::string iniPath; // ImGui keeps the pointer
		iniPath = WideToUtf8(Runtime::Get().DataDir() + L"\\imgui.ini");
		io.IniFilename = iniPath.c_str();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.MouseDrawCursor = true; // the game hides the system cursor while driving
		context_ = true;
		appliedScale_ = 0;
	}
	if (backends_ && (backendWindow_ != hwnd || backendFormat_ != rtvFormat || device_ != host.device)) {
		// only reached with the GPU idle on the old objects? Not guaranteed: wait for the slots first
		UINT64 completed = host.completedFenceValue();
		for (UINT64 f : slotFence_) {
			if (f > completed) {
				return false; // try again next frame
			}
		}
		ShutdownBackends();
	}
	if (!backends_) {
		device_ = host.device;
		D3D12_DESCRIPTOR_HEAP_DESC desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCount, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
		HRESULT hr = device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&srvHeap_));
		if (FAILED(hr)) {
			AE_LOG_CODE(Error, "Overlay", "E_DEVICE_OBJECTS", "The overlay's descriptor heap could not be created: %s", HrToString(hr).c_str());
			return false;
		}
		srvHeap_->SetName(L"Aether.Overlay.SrvHeap");
		srvSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		srvFree_.clear();
		for (UINT i = kSrvCount; i > 0; --i) {
			srvFree_.push_back(i - 1);
		}
		if (!ImGui_ImplWin32_Init(hwnd)) {
			srvHeap_.Reset();
			return false;
		}
		// The IME hook would call into the window's thread from the render thread: off.
		ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;
		ImGui_ImplDX12_InitInfo info;
		info.Device = device_;
		info.CommandQueue = host.queue;
		info.NumFramesInFlight = kFramesInFlight;
		info.RTVFormat = rtvFormat;
		info.DSVFormat = DXGI_FORMAT_UNKNOWN;
		info.UserData = this;
		info.SrvDescriptorHeap = srvHeap_.Get();
		info.SrvDescriptorAllocFn = &Overlay::SrvAlloc;
		info.SrvDescriptorFreeFn = &Overlay::SrvFree;
		if (!ImGui_ImplDX12_Init(&info)) {
			ImGui_ImplWin32_Shutdown();
			srvHeap_.Reset();
			AE_LOG_CODE(Error, "Overlay", "E_DEVICE_OBJECTS", "The Dear ImGui DX12 backend could not start");
			return false;
		}
		backends_ = true;
		backendWindow_ = hwnd;
		backendFormat_ = rtvFormat;
		draws_ = 0;
		AE_LOG(Info, "Overlay", "Dear ImGui %s started (DX12 + Win32 backends, render target format %d)", IMGUI_VERSION, int(rtvFormat));
	}
	return true;
}

void Overlay::NoteSubmitted(UINT64 fence) {
	std::lock_guard lock(mutex_);
	if (drawPending_) {
		slotFence_[(draws_ - 1) % kFramesInFlight] = fence;
		drawPending_ = false;
	}
}

void Overlay::ReleaseDeviceObjects(bool leak) {
	std::lock_guard lock(mutex_);
	if (leak) {
		// the backend's buffers may still be read by the GPU: forget them without releasing
		srvHeap_.Detach();
		backends_ = false;
		if (context_) {
			ImGuiIO& io = ImGui::GetIO();
			io.BackendRendererUserData = nullptr;
			io.BackendPlatformUserData = nullptr;
			context_ = false; // the context itself is leaked too (it still points at backend data)
			ImGui::SetCurrentContext(nullptr);
		}
		return;
	}
	if (backends_) {
		ShutdownBackends();
	}
	if (context_) {
		ImGui::DestroyContext();
		context_ = false;
	}
	device_ = nullptr;
	backendWindow_ = nullptr;
	backendFormat_ = DXGI_FORMAT_UNKNOWN;
}

bool Overlay::Draw(const HostGfx& host, HWND hwnd, DXGI_FORMAT rtvFormat, UINT width, UINT height, ID3D12GraphicsCommandList* list, UINT64 completedFence, uint32_t& drawCalls, std::string& note) {
	std::lock_guard lock(mutex_);
	if (!EnsureContext(host, hwnd, rtvFormat)) {
		note = "the overlay could not start (see the log)";
		return false;
	}
	// the DX12 backend reuses (and may free) the buffers of the draw kFramesInFlight frames ago
	if (slotFence_[draws_ % kFramesInFlight] > completedFence) {
		note = "waiting for the GPU to finish an older overlay frame";
		return false;
	}

	SettingsData s = Settings::Get().Snapshot();
	hotkey_.store(s.overlayHotkey);
	freeCameraHotkey_.store(s.freeCameraHotkey);
	ImGuiIO& io = ImGui::GetIO();
	io.MouseDrawCursor = visible_.load(); // (the game hides the system cursor while driving; no cursor over the free camera's view)
	if (appliedScale_ != s.overlayScale) {
		ImGuiStyle style;
		ImGui::StyleColorsDark(&style);
		style.WindowRounding = 4.0f;
		style.FrameRounding = 3.0f;
		style.ScaleAllSizes(s.overlayScale);
		style.FontScaleMain = s.overlayScale;
		ImGui::GetStyle() = style;
		appliedScale_ = s.overlayScale;
	}

	// input the window procedure queued on the main thread
	std::vector<Message> messages;
	{
		std::lock_guard qlock(queueMutex_);
		messages.swap(queue_);
	}
	for (const Message& m : messages) {
		ImGui_ImplWin32_WndProcHandler(m.hwnd, m.msg, m.wParam, m.lParam);
	}
	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	// the Win32 backend reads the modifier keys with GetKeyState, which is per thread: set them from the async state
	io.AddKeyEvent(ImGuiMod_Ctrl, (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
	io.AddKeyEvent(ImGuiMod_Shift, (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
	io.AddKeyEvent(ImGuiMod_Alt, (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
	io.DisplaySize = ImVec2(float(width), float(height));
	ImGui::NewFrame();
	BuildUi(width, height);
	ImGui::Render();
	wantsMouse_.store(io.WantCaptureMouse);
	wantsKeyboard_.store(io.WantCaptureKeyboard);

	ImDrawData* data = ImGui::GetDrawData();
	ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
	list->SetDescriptorHeaps(1, heaps);
	ImGui_ImplDX12_RenderDrawData(data, list);
	drawCalls = 0;
	for (ImDrawList* dl : data->CmdLists) {
		drawCalls += uint32_t(dl->CmdBuffer.Size);
	}
	++draws_;
	drawPending_ = true;
	return true;
}

// ------------------------------------------------------------------------------------------------ the windows

namespace {

std::string Bytes(uint64_t b) {
	if (b >= (1ull << 30)) {
		return Format("%.2f GB", double(b) / double(1ull << 30));
	}
	if (b >= (1ull << 20)) {
		return Format("%.1f MB", double(b) / double(1ull << 20));
	}
	if (b >= (1ull << 10)) {
		return Format("%.1f KB", double(b) / double(1ull << 10));
	}
	return Format("%llu B", static_cast<unsigned long long>(b));
}

// Data that is not worth fetching every frame.
struct UiCache {
	double nextSlow = 0, nextLog = 0, nextManaged = 0;
	json gpu, scene, materials, renderTextures, managed, shaders;
	json sceneRenderer, sceneGraph, sceneStore, gpuManager; // Aether's own scene path
	double sceneAge = -1, materialsAge = -1, rtAge = -1;
	std::vector<LogEntry> logs;
	VideoMemory vm;
	std::vector<ResourceRecord> resources;
	std::vector<float> intervals, gpuMs;
	uint64_t lastCapture = 0;
	int logSeverity = 1;
	bool logAutoScroll = true;
};
UiCache g_ui;

void JsonTree(const char* label, const json& j, int depth = 0) {
	if (j.is_object() || j.is_array()) {
		if (depth > 0 && !ImGui::TreeNode(label, "%s (%zu)", label, j.size())) {
			return;
		}
		int index = 0;
		for (auto it = j.begin(); it != j.end(); ++it, ++index) {
			std::string key = j.is_object() ? it.key() : Format("[%d]", index);
			if (j.is_array() && it->is_object()) {
				// name array elements after a telling member
				for (const char* k : {"name", "shader", "key", "path"}) {
					if (it->contains(k) && (*it)[k].is_string()) {
						key += " " + (*it)[k].get<std::string>();
						break;
					}
				}
			}
			ImGui::PushID(index);
			JsonTree(key.c_str(), *it, depth + 1);
			ImGui::PopID();
			if (index >= 400) {
				ImGui::TextDisabled("... %zu more", j.size() - size_t(index) - 1);
				break;
			}
		}
		if (depth > 0) {
			ImGui::TreePop();
		}
		return;
	}
	std::string value = j.is_string() ? j.get<std::string>() : Dump(j);
	ImGui::BulletText("%s: %s", label, value.c_str());
}

void KeyValue(const char* key, const std::string& value) {
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::TextUnformatted(key);
	ImGui::TableSetColumnIndex(1);
	ImGui::TextUnformatted(value.c_str());
}

void UnityRow(const char* label, int index, bool bytes = false) {
	double v = Profiler::Get().UnityValue(index);
	if (v < 0) {
		return;
	}
	KeyValue(label, bytes ? Bytes(uint64_t(v)) : (v == double(int64_t(v)) ? Format("%lld", static_cast<long long>(v)) : Format("%.2f", v)));
}

void PerformanceTab() {
	Profiler& prof = Profiler::Get();
	const auto& iv = g_ui.intervals;
	double sum = 0;
	size_t n = 0;
	float worst = 0;
	for (size_t i = iv.size() > 120 ? iv.size() - 120 : 0; i < iv.size(); ++i, ++n) {
		sum += iv[i];
		worst = std::max(worst, iv[i]);
	}
	double avg = n ? sum / double(n) : 0;
	ImGui::Text("FPS %.1f    frame %.2f ms    worst (120 frames) %.2f ms", avg > 0 ? 1000.0 / avg : 0.0, avg, worst);
	if (!iv.empty()) {
		ImGui::PlotLines("##frametime", iv.data(), int(iv.size()), 0, "frame time (ms)", 0.0f, std::max(33.3f, worst * 1.1f), ImVec2(-1, 90));
		ImGui::PlotLines("##gputime", g_ui.gpuMs.data(), int(g_ui.gpuMs.size()), 0, "GPU frame time (ms)", 0.0f, std::max(33.3f, worst * 1.1f), ImVec2(-1, 70));
	}
	if (ImGui::BeginTable("perf", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		UnityRow("Unity CPU main thread (ms)", kUnityCpuMainThreadMs);
		UnityRow("Unity CPU render thread (ms)", kUnityCpuRenderThreadMs);
		UnityRow("Unity present wait (ms)", kUnityCpuPresentWaitMs);
		UnityRow("Unity GPU frame (ms)", kUnityGpuFrameMs);
		UnityRow("Unity draw calls", kUnityDrawCalls);
		UnityRow("Unity batches", kUnityBatches);
		UnityRow("Unity SetPass calls", kUnitySetPassCalls);
		UnityRow("Unity triangles", kUnityTriangles);
		UnityRow("Unity vertices", kUnityVertices);
		UnityRow("Shadow casters", kUnityShadowCasters);
		ImGui::EndTable();
	}
	json bench = prof.BenchmarkStatus();
	std::string state = bench.value("state", "idle");
	if (state == "running") {
		ImGui::ProgressBar(float(bench.value("elapsedSeconds", 0.0) / std::max(0.1, bench.value("requestedSeconds", 1.0))), ImVec2(-1, 0), "benchmark running");
		if (ImGui::Button("Stop benchmark")) {
			prof.StopBenchmark();
		}
	} else {
		if (ImGui::Button("Run 10 s benchmark")) {
			prof.StartBenchmark("overlay", 10.0, Runtime::Get().Subdir(L"benchmarks"), {{"startedBy", "overlay"}, {"scene", Runtime::Get().ManagedState("scene")}});
		}
		if (state == "done") {
			const json& r = bench["result"];
			ImGui::SameLine();
			ImGui::Text("last: %.1f fps avg, %.1f fps 1%% low, %zu frames", r.value("averageFps", 0.0), r.value("onePercentLowFps", 0.0), r.value("frames", size_t(0)));
		}
	}
}

void GpuTab() {
	const json& g = g_ui.gpu;
	if (!g.value("available", false)) {
		ImGui::TextUnformatted("The D3D12 device has not been seen yet.");
		return;
	}
	if (g.contains("adapter") && ImGui::BeginTable("adapter", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		const json& a = g["adapter"];
		KeyValue("Vendor", a.value("vendor", ""));
		KeyValue("Name", a.value("name", ""));
		KeyValue("Device ID", a.value("deviceId", ""));
		KeyValue("Driver", a.value("driverVersion", ""));
		KeyValue("Dedicated VRAM", Bytes(a.value("dedicatedVideoMemoryBytes", uint64_t(0))));
		KeyValue("Shared system memory", Bytes(a.value("sharedSystemMemoryBytes", uint64_t(0))));
		KeyValue("Device state", g.value("deviceState", ""));
		ImGui::EndTable();
	}
	if (g.contains("features") && ImGui::CollapsingHeader("D3D12 feature support", ImGuiTreeNodeFlags_DefaultOpen)) {
		JsonTree("features", g["features"]);
	}
}

void VramTab() {
	const VideoMemory& vm = g_ui.vm;
	ResourceRegistry& reg = ResourceRegistry::Get();
	uint64_t aetherGpu = reg.GpuBytes();
	if (vm.valid) {
		float fraction = vm.localBudget ? float(double(vm.localUsage) / double(vm.localBudget)) : 0.0f;
		ImGui::Text("Dedicated VRAM (this process)");
		ImGui::ProgressBar(fraction, ImVec2(-1, 0), Format("%s / %s budget", Bytes(vm.localUsage).c_str(), Bytes(vm.localBudget).c_str()).c_str());
		ImGui::Text("Shared GPU memory: %s / %s budget", Bytes(vm.nonLocalUsage).c_str(), Bytes(vm.nonLocalBudget).c_str());
	} else {
		ImGui::TextUnformatted("Process video memory is not available yet.");
	}
	ImGui::Separator();
	if (ImGui::BeginTable("vram", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		KeyValue("Aether GPU resources", Bytes(aetherGpu));
		KeyValue("Aether upload / readback (system memory)", Bytes(reg.TotalBytes() - aetherGpu));
		if (vm.valid) {
			KeyValue("Not Aether (Unity / HDRP, assets, driver)", Bytes(vm.localUsage > aetherGpu ? vm.localUsage - aetherGpu : 0));
		}
		for (int c = 0; c < int(ResourceCategory::Count); ++c) {
			uint64_t b = reg.CategoryBytes(ResourceCategory(c));
			if (b) {
				KeyValue(Format("  Aether %s", CategoryName(ResourceCategory(c))).c_str(), Bytes(b));
			}
		}
		UnityRow("Unity render textures", kUnityRenderTexturesBytes, true);
		UnityRow("Unity render texture count", kUnityRenderTexturesCount);
		UnityRow("Unity textures in use", kUnityUsedTexturesBytes, true);
		UnityRow("Unity texture count", kUnityUsedTexturesCount);
		UnityRow("Unity buffers in use", kUnityUsedBuffersBytes, true);
		UnityRow("Texture streaming: current", kUnityTextureStreamingCurrentBytes, true);
		UnityRow("Texture streaming: all mips", kUnityTextureTotalBytes, true);
		UnityRow("Unity graphics driver memory", kUnityGfxDriverBytes, true);
		ImGui::EndTable();
	}
	if (ImGui::CollapsingHeader("Aether GPU resource manager (descriptors, frame memory, budget)")) {
		JsonTree("manager", g_ui.gpuManager);
	}
	if (ImGui::CollapsingHeader("Unity render targets (HDRP side)")) {
		if (ImGui::Button("Refresh")) {
			Runtime::Get().RequestManaged(kRequestRenderTextures);
		}
		if (g_ui.renderTextures.is_null()) {
			ImGui::TextDisabled("Not collected yet: press Refresh.");
		} else {
			ImGui::SameLine();
			ImGui::TextDisabled("%.0f s old", g_ui.rtAge);
			JsonTree("renderTextures", g_ui.renderTextures);
		}
	}
}

void ResourcesTab() {
	ImGui::Text("%zu resources created by Aether, %s", g_ui.resources.size(), Bytes(ResourceRegistry::Get().TotalBytes()).c_str());
	if (ImGui::BeginTable("resources", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("Category");
		ImGui::TableSetupColumn("Size");
		ImGui::TableSetupColumn("Dimensions");
		ImGui::TableSetupColumn("Heap");
		ImGui::TableSetupColumn("Age (s)");
		ImGui::TableHeadersRow();
		double now = QpcSeconds();
		for (const ResourceRecord& r : g_ui.resources) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.name.c_str());
			ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(CategoryName(r.category));
			ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(Bytes(r.bytes).c_str());
			ImGui::TableSetColumnIndex(3); ImGui::Text("%ux%u", r.width, r.height);
			ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(r.heap == D3D12_HEAP_TYPE_DEFAULT ? "default" : r.heap == D3D12_HEAP_TYPE_UPLOAD ? "upload" : "readback");
			ImGui::TableSetColumnIndex(5); ImGui::Text("%.0f", now - r.createdTime);
		}
		ImGui::EndTable();
	}
}

void PipelineTab() {
	json state = Renderer::Get().State();
	ImGui::Text("Renderer: %s", state.value("state", "").c_str());
	ImGui::TextWrapped("Main renderer: %s", state.value("mainRenderer", "").c_str());
	if (state.contains("backBuffer") && state["backBuffer"].is_object()) {
		const json& b = state["backBuffer"];
		ImGui::Text("Back buffer: %ux%u, DXGI format %d, %u buffers", b.value("width", 0u), b.value("height", 0u), b.value("dxgiFormat", 0), b.value("bufferCount", 0u));
	}
	if (state.contains("lastError") && state["lastError"].is_string()) {
		ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", state["lastError"].get<std::string>().c_str());
	}
	ImGui::SeparatorText("Render passes (last frame)");
	std::vector<PassTiming> passes = Profiler::Get().PassSnapshot();
	if (ImGui::BeginTable("passes", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Pass");
		ImGui::TableSetupColumn("Ran");
		ImGui::TableSetupColumn("CPU ms");
		ImGui::TableSetupColumn("GPU ms");
		ImGui::TableSetupColumn("Draw calls");
		ImGui::TableSetupColumn("Note");
		ImGui::TableHeadersRow();
		for (const PassTiming& p : passes) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(p.name.c_str());
			ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(p.executed ? "yes" : "no");
			ImGui::TableSetColumnIndex(2); ImGui::Text("%.3f", p.cpuMs);
			ImGui::TableSetColumnIndex(3);
			if (p.gpuMs >= 0) {
				ImGui::Text("%.3f", p.gpuMs);
			} else {
				ImGui::TextDisabled("-");
			}
			ImGui::TableSetColumnIndex(4); ImGui::Text("%u", p.drawCalls);
			ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(p.note.c_str());
		}
		ImGui::EndTable();
	}
	ImGui::SeparatorText("Aether scene renderer");
	Settings& settings = Settings::Get();
	SettingsData s = settings.Snapshot();
	if (ImGui::BeginCombo("Render mode", s.renderMode.c_str())) {
		for (const std::string& m : RenderModeNames()) {
			if (ImGui::Selectable(m.c_str(), m == s.renderMode)) {
				settings.Set("renderMode", m, "overlay");
			}
		}
		ImGui::EndCombo();
	}
	ImGui::TextDisabled("hdrp: the game's renderer. aether: Aether's image replaces it. compare: left HDRP, right Aether.");
	if (ImGui::BeginCombo("Debug view", s.debugView.c_str())) {
		for (const std::string& v : DebugViewNames()) {
			if (ImGui::Selectable(v.c_str(), v == s.debugView)) {
				settings.Set("debugView", v, "overlay");
			}
		}
		ImGui::EndCombo();
	}
	bool b = s.shadows;
	if (ImGui::Checkbox("Sun shadows", &b)) {
		settings.Set("shadows", b, "overlay");
	}
	ImGui::SameLine();
	b = s.sky;
	if (ImGui::Checkbox("Sky", &b)) {
		settings.Set("sky", b, "overlay");
	}
	float ev = s.exposureEV;
	ImGui::SetNextItemWidth(200);
	ImGui::SliderFloat("Exposure bias (EV)", &ev, -5.0f, 5.0f, "%.2f");
	if (ImGui::IsItemDeactivatedAfterEdit()) {
		settings.Set("exposureEV", ev, "overlay");
	}
	const json& r = g_ui.sceneRenderer;
	if (r.is_object() && r.contains("drawCalls")) {
		if (ImGui::BeginTable("scenestats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			KeyValue("Visible instances", Format("%u", r.value("visibleInstances", 0u)));
			KeyValue("Draw calls (opaque / transparent / shadow)", Format("%u (%u / %u / %u)", r.value("drawCalls", 0u), r.value("opaqueDraws", 0u), r.value("transparentDraws", 0u), r.value("shadowCasterDraws", 0u)));
			KeyValue("Triangles", Format("%u", r.value("triangles", 0u)));
			KeyValue("Materials in frame", Format("%u (%u draws with an unsupported material)", r.value("materialsInFrame", 0u), r.value("unsupportedMaterialDraws", 0u)));
			KeyValue("Pipeline states", Format("%u", r.value("pipelineStates", 0u)));
			if (r.contains("lights")) {
				const json& l = r["lights"];
				KeyValue("Lights", Format("%d directional, %u punctual; %u of %u clusters lit, at most %u lights in one", l.value("directional", 0), l.value("punctual", 0u), l.value("clustersWithLights", 0u), l.value("clusters", 0u), l.value("maxLightsInACluster", 0u)));
			}
			if (r.contains("shadows")) {
				const json& sh = r["shadows"];
				KeyValue("Shadow cascades", Format("%u x %u px (%s)", sh.value("cascades", 0u), sh.value("cascadeSize", 0u), Bytes(sh.value("atlasBytes", uint64_t(0))).c_str()));
			}
			ImGui::EndTable();
		}
	} else {
		ImGui::TextDisabled("Not rendering (render mode hdrp, or no scene camera yet).");
	}
	if (ImGui::CollapsingHeader("Render graph (scene)")) {
		JsonTree("sceneGraph", g_ui.sceneGraph);
	}
}

void ShadersTab() {
	if (ImGui::Button("Reload shaders")) {
		ShaderManager::Get().RequestReload();
	}
	const json& list = g_ui.shaders;
	ImGui::SameLine();
	ImGui::TextDisabled("%s", list.value("directory", "").c_str());
	if (!list.contains("shaders")) {
		return;
	}
	for (const json& s : list["shaders"]) {
		bool ok = s.value("ok", false);
		std::string output = s.value("compilerOutput", "");
		ImGui::TextColored(ok ? ImVec4(0.5f, 1, 0.5f, 1) : ImVec4(1, 0.4f, 0.4f, 1), "%s", ok ? "OK   " : "ERROR");
		ImGui::SameLine();
		ImGui::Text("%s  (%.1f ms, %zu bytes)", s.value("key", "").c_str(), s.value("compileMs", 0.0), s.value("bytecodeBytes", size_t(0)));
		if (!output.empty()) {
			ImGui::Indent();
			ImGui::PushStyleColor(ImGuiCol_Text, ok ? ImVec4(1, 0.9f, 0.5f, 1) : ImVec4(1, 0.6f, 0.6f, 1));
			ImGui::TextWrapped("%s", output.c_str());
			ImGui::PopStyleColor();
			ImGui::Unindent();
		}
	}
}

void SceneTab() {
	if (ImGui::Button("Refresh scene")) {
		Runtime::Get().RequestManaged(kRequestScene);
	}
	ImGui::SameLine();
	if (ImGui::Button("Collect materials")) {
		Runtime::Get().RequestManaged(kRequestMaterials);
	}
	if (!g_ui.managed.is_null() && ImGui::CollapsingHeader("Game / Unity", ImGuiTreeNodeFlags_DefaultOpen)) {
		JsonTree("managed", g_ui.managed);
	}
	if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (g_ui.scene.is_null()) {
			ImGui::TextDisabled("Not published yet.");
		} else {
			ImGui::TextDisabled("%.0f s old", g_ui.sceneAge);
			JsonTree("scene", g_ui.scene);
		}
	}
	if (ImGui::CollapsingHeader("Aether scene (what the native renderer holds)", ImGuiTreeNodeFlags_DefaultOpen)) {
		JsonTree("aetherScene", g_ui.sceneStore);
	}
	if (ImGui::CollapsingHeader("Materials")) {
		if (g_ui.materials.is_null()) {
			ImGui::TextDisabled("Not collected yet: press Collect materials (walks every renderer once).");
		} else {
			ImGui::TextDisabled("%.0f s old", g_ui.materialsAge);
			JsonTree("materials", g_ui.materials);
		}
	}
}

void SettingsTab() {
	Settings& settings = Settings::Get();
	SettingsData s = settings.Snapshot();
	bool b = s.testTriangle;
	if (ImGui::Checkbox("Test triangle (DX12 integration check)", &b)) {
		settings.Set("testTriangle", b, "overlay");
	}
	b = s.gpuTiming;
	if (ImGui::Checkbox("GPU timing (timestamp queries)", &b)) {
		settings.Set("gpuTiming", b, "overlay");
	}
	const auto& qualities = QualityNames();
	if (ImGui::BeginCombo("Render quality", s.quality.c_str())) {
		for (const std::string& q : qualities) {
			if (ImGui::Selectable(q.c_str(), q == s.quality)) {
				settings.Set("quality", q, "overlay");
			}
		}
		ImGui::EndCombo();
	}
	ImGui::TextDisabled("Quality sets the shadow cascades' resolution and distance of Aether's scene renderer.");
	ImGui::SeparatorText("Graphics");
	ImGui::TextDisabled("These are Aether's own options. The game's graphics menu is not used.");
	static const char* skyModes[] = {"physical", "scene"};
	static const char* skyText[] = {"Physical sky (time of day)", "The scene's own sky"};
	int skyMode = s.skyMode == "scene" ? 1 : 0;
	if (ImGui::Combo("Sky and sun", &skyMode, skyText, 2)) {
		settings.Set("skyMode", skyModes[skyMode], "overlay");
	}
	if (skyMode == 0) {
		float hours = s.timeOfDay;
		if (ImGui::SliderFloat("Time of day (hours)", &hours, 0.0f, 24.0f, "%.2f")) {
			settings.Set("timeOfDay", hours, "overlay");
		}
		float heading = s.sunHeading;
		if (ImGui::SliderFloat("Sun heading (degrees)", &heading, 0.0f, 360.0f, "%.0f")) {
			settings.Set("sunHeading", heading, "overlay");
		}
		float coverage = s.cloudCoverage;
		if (ImGui::SliderFloat("Clouds (coverage)", &coverage, 0.0f, 1.0f, "%.2f")) {
			settings.Set("cloudCoverage", coverage, "overlay");
		}
		float speed = s.cloudSpeed;
		if (ImGui::SliderFloat("Cloud speed", &speed, 0.0f, 4.0f, "%.1f")) {
			settings.Set("cloudSpeed", speed, "overlay");
		}
	}
	static const char* tonemaps[] = {"host", "agx", "aces", "neutral", "none"};
	static const char* tonemapText[] = {"The scene's own curve", "AgX", "ACES", "Neutral", "None"};
	int tonemap = 0;
	for (int i = 0; i < 5; ++i) {
		if (s.tonemap == tonemaps[i]) {
			tonemap = i;
		}
	}
	if (ImGui::Combo("Tone mapping", &tonemap, tonemapText, 5)) {
		settings.Set("tonemap", tonemaps[tonemap], "overlay");
	}
	struct Toggle { const char* key; const char* label; bool value; };
	Toggle toggles[] = {
		{"antialiasing", "Antialiasing (temporal)", s.antialiasing}, {"shadows", "Sun shadows", s.shadows}, {"ssao", "Ambient occlusion", s.ssao},
		{"ssr", "Screen space reflections", s.ssr}, {"bloom", "Bloom", s.bloom}, {"motionBlur", "Motion blur", s.motionBlur},
		{"particles", "Particles and tyre smoke", s.particles}, {"volumetricFog", "Volumetric fog (light shafts)", s.volumetricFog},
		{"indirectLight", "Indirect light (bounced, screen space)", s.indirectLight}, {"reflectionProbe", "Reflection probes", s.reflectionProbe}, {"rayTracedReflections", "Ray traced reflections", s.rayTracedReflections}, {"probeGi", "Global illumination (probe grid; when cascades are off)", s.probeGi}, {"lensFlare", "Lens flare", s.lensFlare}, {"rayTracedLighting", "Ray traced shadows and ambient occlusion (NVIDIA NRD)", s.rayTracedLighting}, {"rayTracedGi", "Ambient light per pixel from long rays (NVIDIA NRD)", s.rayTracedGi}, {"radianceCascades", "Global illumination (radiance cascades)", s.radianceCascades}, {"followSceneNight", "Night when the game's scene is at night", s.followSceneNight},
	};
	for (Toggle& t : toggles) {
		if (ImGui::Checkbox(t.label, &t.value)) {
			settings.Set(t.key, t.value, "overlay");
		}
	}
	bool nrd = s.reflectionDenoiser == "nrd";
	if (ImGui::Checkbox("Glossy reflections, denoised (NVIDIA NRD)", &nrd)) {
		settings.Set("reflectionDenoiser", nrd ? "nrd" : "off", "overlay");
	}
	struct Slider { const char* key; const char* label; float value, lo, hi; const char* format; };
	Slider sliders[] = {
		{"shadowSoftness", "Shadow softness (texels)", s.shadowSoftness, 0.0f, 4.0f, "%.1f"},
		{"indirectStrength", "Indirect light strength", s.indirectStrength, 0.0f, 3.0f, "%.2f"},
		{"fogStrength", "Fog density (factor)", s.fogStrength, 0.0f, 20.0f, "%.1f"},
		{"moonlight", "Moonlight at night (factor)", s.moonlight, 0.0f, 4.0f, "%.2f"},
		{"skinnedRayUpdates", "Skinned poses read per frame (reflections)", s.skinnedRayUpdates, 0.0f, 4.0f, "%.0f"},
		{"motionBlurStrength", "Motion blur strength", s.motionBlurStrength, 0.0f, 2.0f, "%.2f"},
		{"bloomStrength", "Bloom strength", s.bloomStrength, 0.0f, 2.0f, "%.2f"},
		{"lensFlareStrength", "Lens flare strength", s.lensFlareStrength, 0.0f, 3.0f, "%.2f"},
		{"aoStrength", "Ambient occlusion strength", s.aoStrength, 0.0f, 2.0f, "%.2f"},
		{"aoRadius", "Ambient occlusion radius (m)", s.aoRadius, 0.2f, 4.0f, "%.2f"},
		{"smokeOpacity", "Tyre smoke opacity", s.smokeOpacity, 0.0f, 2.0f, "%.2f"},
		{"exposureEV", "Exposure bias (stops)", s.exposureEV, -3.0f, 3.0f, "%.2f"},
		{"lodBias", "Level of detail bias", s.lodBias, 0.3f, 8.0f, "%.1f"},
	};
	for (Slider& sl : sliders) {
		if (ImGui::SliderFloat(sl.label, &sl.value, sl.lo, sl.hi, sl.format)) {
			settings.Set(sl.key, sl.value, "overlay");
		}
	}
	int level = s.anisotropicLevel;
	if (ImGui::SliderInt("Anisotropic filtering", &level, 1, 16)) {
		settings.Set("anisotropicLevel", level, "overlay");
	}
	static const char* vsyncText[] = {"off", "every refresh", "every second refresh"};
	int vsync = s.vsync;
	if (ImGui::Combo("Vertical sync", &vsync, vsyncText, 3)) {
		settings.Set("vsync", vsync, "overlay");
	}
	ImGui::SeparatorText("Free camera and path tracing");
	ImGui::TextDisabled("F11 (\"freeCameraHotkey\") switches the free camera on and off; the game's UI is hidden while it is on.");
	Overlay::Get().FreeCameraStatus();
	static const char* ptModes[] = {"off", "freeCamera", "always"};
	static const char* ptText[] = {"Off", "While the free camera is on", "Always"};
	int ptMode = s.pathTracing == "off" ? 0 : s.pathTracing == "always" ? 2 : 1;
	if (ImGui::Combo("Path tracing", &ptMode, ptText, 3)) {
		settings.Set("pathTracing", ptModes[ptMode], "overlay");
	}
	int bounces = s.pathTracingBounces;
	if (ImGui::SliderInt("Path tracing bounces", &bounces, 1, 12)) {
		settings.Set("pathTracingBounces", bounces, "overlay");
	}
	int maxSamples = s.pathTracingMaxSamples;
	if (ImGui::InputInt("Path tracing samples per pixel", &maxSamples, 256, 4096)) {
		settings.Set("pathTracingMaxSamples", std::min(std::max(maxSamples, 1), 100000), "overlay");
	}
	float camSpeed = s.freeCameraSpeed;
	if (ImGui::SliderFloat("Free camera speed (m/s)", &camSpeed, 0.2f, 100.0f, "%.1f", ImGuiSliderFlags_Logarithmic)) {
		settings.Set("freeCameraSpeed", camSpeed, "overlay");
	}
	float camLook = s.freeCameraSensitivity;
	if (ImGui::SliderFloat("Free camera mouse sensitivity", &camLook, 0.01f, 1.0f, "%.2f", ImGuiSliderFlags_Logarithmic)) {
		settings.Set("freeCameraSensitivity", camLook, "overlay");
	}
	ImGui::SeparatorText("AI agent (MCP)");
	static const char* modes[] = {"deny", "ask", "allow"};
	static const char* modeText[] = {"deny: read only", "ask: every change needs approval here", "allow: changes without asking"};
	int mode = s.mcpControl == "deny" ? 0 : s.mcpControl == "allow" ? 2 : 1;
	if (ImGui::Combo("Control requests", &mode, modeText, 3)) {
		settings.Set("mcpControl", modes[mode], "overlay");
	}
	ImGui::SeparatorText("Overlay");
	float scale = s.overlayScale;
	if (ImGui::SliderFloat("UI scale", &scale, 0.5f, 3.0f, "%.2f")) {
		// applied when the slider is released (rescaling the style every frame fights the drag)
	}
	if (ImGui::IsItemDeactivatedAfterEdit()) {
		settings.Set("overlayScale", scale, "overlay");
	}
	ImGui::Text("Hotkey: virtual key %d (0x%02X). Change \"overlayHotkey\" in aether.json.", s.overlayHotkey, s.overlayHotkey);
	b = s.overlayVisibleAtStart;
	if (ImGui::Checkbox("Show the overlay at start", &b)) {
		settings.Set("overlayVisibleAtStart", b, "overlay");
	}
	ImGui::SeparatorText("Tools");
	if (ImGui::Button("Capture frame (PNG)")) {
		g_ui.lastCapture = Renderer::Get().RequestCapture(NewCapturePath(), true);
	}
	if (g_ui.lastCapture) {
		json st = Renderer::Get().CaptureStatus(g_ui.lastCapture);
		ImGui::SameLine();
		ImGui::TextWrapped("%s: %s %s", st.value("state", "").c_str(), st.value("file", "").c_str(), st.value("error", "").c_str());
	}
}

void LogTab() {
	static const char* levels[] = {"debug", "info", "warning", "error"};
	ImGui::SetNextItemWidth(140);
	ImGui::Combo("Minimum severity", &g_ui.logSeverity, levels, 4);
	ImGui::SameLine();
	ImGui::Checkbox("Auto scroll", &g_ui.logAutoScroll);
	ImGui::SameLine();
	ImGui::TextDisabled("%u errors this session", Log::Get().ErrorCount());
	ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
	for (const LogEntry& e : g_ui.logs) {
		if (int(e.severity) < g_ui.logSeverity) {
			continue;
		}
		ImVec4 color = e.severity >= Severity::Error ? ImVec4(1, 0.45f, 0.45f, 1) : e.severity == Severity::Warning ? ImVec4(1, 0.85f, 0.4f, 1) : ImVec4(0.85f, 0.85f, 0.85f, 1);
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::Text("%s [%s] %s%s%s: %s", e.timestamp.c_str() + 11, SeverityName(e.severity), e.component.c_str(), e.code.empty() ? "" : " ", e.code.c_str(), e.message.c_str());
		ImGui::PopStyleColor();
	}
	if (g_ui.logAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f) {
		ImGui::SetScrollHereY(1.0f);
	}
	ImGui::EndChild();
}

void ApprovalWindow(UINT width, UINT height) {
	ApprovalPrompt prompt;
	if (!Runtime::Get().CurrentApproval(prompt)) {
		return;
	}
	ImGui::SetNextWindowPos(ImVec2(float(width) * 0.5f, float(height) * 0.3f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(520.0f * ImGui::GetStyle().FontScaleMain, 0));
	ImGui::Begin("Aether: an AI agent asks for a change", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
	ImGui::TextWrapped("Request: %s", prompt.what.c_str());
	ImGui::TextWrapped("%s", prompt.detail.c_str());
	ImGui::Text("Denied automatically in %.0f s", std::max(0.0, prompt.deadline - QpcSeconds()));
	if (ImGui::Button("Approve", ImVec2(120, 0))) {
		Runtime::Get().ResolveApproval(prompt.id, true);
	}
	ImGui::SameLine();
	if (ImGui::Button("Deny", ImVec2(120, 0))) {
		Runtime::Get().ResolveApproval(prompt.id, false);
	}
	ImGui::End();
}

} // namespace

// The free camera's state in the overlay (nothing is drawn over the picture while it is on: the UI is hidden then).
void Overlay::FreeCameraStatus() {
	const SettingsData s = Settings::Get().Snapshot();
	if (freeCamera_.load()) {
		ImGui::Text("Free camera on (field of view %.0f deg), held while the overlay is open", freeCameraFov_.load());
	} else {
		ImGui::TextDisabled("Free camera off");
	}
	ImGui::TextDisabled("WASD / arrows move, E / Space up, Q / C down, mouse looks, wheel zooms, R resets the zoom, Shift faster, Ctrl slower.");
	const SceneRenderer::PathTraceStatus pt = SceneRenderer::Get().PathTracing();
	if (pt.active) {
		ImGui::Text("Path tracing: %u / %d samples per pixel%s", pt.samples, s.pathTracingMaxSamples, pt.samples >= uint32_t(s.pathTracingMaxSamples) ? " (done)" : "");
	} else if (s.pathTracing != "off" && (freeCamera_.load() || s.pathTracing == "always")) {
		ImGui::TextDisabled("Path tracing not running (it needs DXR 1.1 and the scene's ray tracing geometry)");
	}
}

void Overlay::BuildUi(UINT width, UINT height) {
	ApprovalWindow(width, height);
	if (!visible_.load()) {
		return;
	}
	double now = QpcSeconds();
	Runtime& rt = Runtime::Get();
	Profiler::Get().History(g_ui.intervals, g_ui.gpuMs, 300);
	if (now >= g_ui.nextSlow) {
		g_ui.nextSlow = now + 0.5;
		g_ui.gpu = GpuInfo::Get().ToJson();
		g_ui.vm = GpuInfo::Get().QueryVideoMemory();
		g_ui.resources = ResourceRegistry::Get().Snapshot();
		g_ui.shaders = ShaderManager::Get().List();
		g_ui.logs = Log::Get().Snapshot(400);
		g_ui.scene = rt.ManagedState("scene", &g_ui.sceneAge);
		g_ui.materials = rt.ManagedState("materials", &g_ui.materialsAge);
		g_ui.renderTextures = rt.ManagedState("renderTextures", &g_ui.rtAge);
		g_ui.managed = rt.ManagedState("managed");
		g_ui.sceneRenderer = SceneRenderer::Get().Stats();
		g_ui.sceneGraph = SceneRenderer::Get().GraphDescription();
		g_ui.sceneStore = SceneStore::Get().Summary();
		g_ui.gpuManager = GpuResources::Get().Report();
	}

	ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_FirstUseEver);
	bool open = true;
	if (ImGui::Begin(Format("Aether %s###aether", kAetherVersion).c_str(), &open)) {
		if (Renderer::Get().Failed()) {
			ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Aether's rendering stopped after an error (see Pipeline and Log).");
		}
		if (rt.PreviousSessionCrashed()) {
			ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "The previous session did not shut down cleanly (get_crash_report has the details).");
		}
		if (ImGui::BeginTabBar("tabs")) {
			if (ImGui::BeginTabItem("Performance")) { PerformanceTab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("GPU")) { GpuTab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("VRAM")) { VramTab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Resources")) { ResourcesTab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Pipeline")) { PipelineTab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Shaders")) { ShadersTab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Scene")) { SceneTab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Settings")) { SettingsTab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Log")) { LogTab(); ImGui::EndTabItem(); }
			ImGui::EndTabBar();
		}
	}
	ImGui::End();
	if (!open) {
		SetVisible(false);
	}
}

} // namespace aether
