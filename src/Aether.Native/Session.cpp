#include "Session.h"
#include "Core/Config.h"
#include "Core/Log.h"
#include "Device/GpuInfo.h"
#include "Diagnostics.h"
#include "ImGui/Overlay.h"
#include "Ipc/PipeServer.h"
#include "Renderer/Renderer.h"
#include "Runtime.h"
#include "Shaders/ShaderManager.h"

namespace aether::session {

namespace {

std::atomic<bool> g_started{false}, g_stopping{false}, g_dead{false};
std::mutex g_frameMutex; // one frame entry point at a time (and never during Stop)
HANDLE g_released = nullptr; // manual-reset event: device objects were released on the render thread

enum Entry { kBegin, kEnd, kRelease };

void Body(Entry entry, const HostGfx* host, uint32_t waitMs) {
	std::lock_guard lock(g_frameMutex);
	if (!g_started.load()) {
		return;
	}
	switch (entry) {
	case kBegin:
		if (!g_stopping.load()) {
			Renderer::Get().FrameBegin(*host);
		}
		break;
	case kEnd:
		if (!g_stopping.load()) {
			Renderer::Get().FrameEnd(*host);
		}
		break;
	case kRelease:
		Renderer::Get().ReleaseDeviceObjects(host, waitMs);
		if (g_released) {
			SetEvent(g_released);
		}
		break;
	}
}

int Filter(EXCEPTION_POINTERS* info, DWORD* code, void** address) {
	*code = info->ExceptionRecord->ExceptionCode;
	*address = info->ExceptionRecord->ExceptionAddress;
	// C++ exceptions are handled by the catch below the SEH frame; debugger breaks pass through
	if (*code == 0xE06D7363 || *code == EXCEPTION_BREAKPOINT || *code == EXCEPTION_SINGLE_STEP) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	return EXCEPTION_EXECUTE_HANDLER;
}

void ReportFault(const char* what, DWORD code, void* address) {
	if (g_dead.exchange(true)) {
		return;
	}
	HMODULE module = nullptr;
	wchar_t name[MAX_PATH] = L"";
	uintptr_t offset = 0;
	if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module) && module) {
		GetModuleFileNameW(module, name, MAX_PATH);
		offset = uintptr_t(address) - uintptr_t(module);
	}
	json extra = {{"exceptionCode", Format("0x%08lX", code)}, {"module", WideToUtf8(name)}, {"moduleOffset", Format("0x%llX", static_cast<unsigned long long>(offset))}, {"gpu", GpuInfo::Get().Brief()}};
	Log::Get().Write(Severity::Fatal, "Session", "E_FAULT", std::string(what) + ": Aether stopped rendering for this session; the game keeps running", {}, extra);
	json report = extra;
	report["code"] = "E_FAULT";
	report["description"] = what;
	report["logTail"] = json::array();
	for (const LogEntry& e : Log::Get().Snapshot(40)) {
		report["logTail"].push_back(ToJson(e));
	}
	Runtime::Get().WriteCrashReport(std::move(report));
}

void Caught(Entry entry, const HostGfx* host, uint32_t waitMs) {
	try {
		Body(entry, host, waitMs);
	} catch (const std::exception& e) {
		ReportFault((std::string("unhandled C++ exception in the frame: ") + e.what()).c_str(), 0xE06D7363, nullptr);
	}
}

// No C++ objects with destructors in this function (structured exception handling).
void Guarded(Entry entry, const HostGfx* host, uint32_t waitMs) {
	DWORD code = 0;
	void* address = nullptr;
	__try {
		Caught(entry, host, waitMs);
	} __except (Filter(GetExceptionInformation(), &code, &address)) {
		ReportFault("structured exception in the frame", code, address);
	}
}

} // namespace

bool Start(const std::wstring& dataDir, const std::wstring& configDir) {
	std::lock_guard lock(g_frameMutex);
	if (g_started.load()) {
		return false;
	}
	if (!g_released) {
		g_released = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	}
	ResetEvent(g_released);
	g_stopping.store(false);
	Runtime::Get().Initialize(dataDir, configDir);
	ShaderManager::Get().SetDirectory(dataDir + L"\\shaders");
	Overlay::Get().SetVisible(Settings::Get().Snapshot().overlayVisibleAtStart);
	PipeServer::Get().Start();
	g_started.store(true);
	AE_LOG(Info, "Session", "Aether %s session started", kAetherVersion);
	return true;
}

void Stop() {
	if (!g_started.load()) {
		return;
	}
	g_stopping.store(true);
	Overlay::Get().SetVisible(false);
	Overlay::Get().DetachWindow();
	Runtime::Get().BeginShutdown();
	PipeServer::Get().Stop();
	{
		std::lock_guard lock(g_frameMutex);
		if (Renderer::Get().Ready()) {
			// the render thread did not get to release (it is gone, or the release event was never issued)
			Renderer::Get().ReleaseDeviceObjects(nullptr, 0);
		}
		g_started.store(false);
	}
	Runtime::Get().Shutdown();
}

bool Started() { return g_started.load(); }
bool Dead() { return g_dead.load(); }

void FrameBegin(const HostGfx& host) {
	if (g_started.load() && !g_dead.load() && !g_stopping.load()) {
		Guarded(kBegin, &host, 0);
	}
}

void FrameEnd(const HostGfx& host) {
	if (g_started.load() && !g_dead.load() && !g_stopping.load()) {
		Guarded(kEnd, &host, 0);
	}
}

void ReleaseOnRenderThread(const HostGfx& host, uint32_t waitMs) {
	if (!g_started.load()) {
		return;
	}
	if (g_dead.load()) {
		// after a fault the renderer's state is not trusted: abandon instead of releasing
		std::lock_guard lock(g_frameMutex);
		Renderer::Get().ReleaseDeviceObjects(nullptr, 0);
		if (g_released) {
			SetEvent(g_released);
		}
		return;
	}
	Guarded(kRelease, &host, waitMs);
}

bool WaitReleased(uint32_t timeoutMs) {
	return g_released && WaitForSingleObject(g_released, timeoutMs) == WAIT_OBJECT_0;
}

void MainThreadUpdate() {
	if (!g_started.load() || g_stopping.load()) {
		return;
	}
	Overlay& overlay = Overlay::Get();
	if (!overlay.Attached()) {
		HWND hwnd = Renderer::Get().Window();
		if (hwnd) {
			overlay.AttachWindow(hwnd);
		}
	}
}

} // namespace aether::session
