// Process-wide runtime state that is not rendering: folders, the worker thread, what the managed side publishes
// (scene, materials, ...), crash reports and the approval prompts for control requests from AI agents.
#pragma once
#include "Core/Common.h"
#include "Core/Json.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <thread>

namespace aether {

// What the native side can ask the managed side to publish (Aether_TakeManagedRequests). Mirrored in Native.cs.
enum ManagedRequest : uint32_t {
	kRequestScene = 1,          // key "scene"
	kRequestMaterials = 2,      // key "materials"
	kRequestRenderTextures = 4, // key "renderTextures" (Unity's render targets: the HDRP side of the VRAM)
};

struct ApprovalPrompt {
	uint64_t id = 0;
	std::string what;   // e.g. "set_render_quality"
	std::string detail; // e.g. the arguments
	double deadline = 0;
};

class Runtime {
public:
	static Runtime& Get();
	void Initialize(const std::wstring& dataDir, const std::wstring& configDir);
	// Releases every thread waiting in RequestApproval / RefreshManaged (call before stopping the IPC threads).
	void BeginShutdown();
	void Shutdown();
	bool Initialized() const { return initialized_.load(); }
	std::wstring DataDir();
	std::wstring Subdir(const wchar_t* name); // created on demand
	bool PreviousSessionCrashed() const { return previousCrashed_; }

	// One background thread for slow work that must not run on the render thread (PNG encoding, file writes).
	void Post(std::function<void()> job);

	// ---- managed state (any thread)
	void SetManagedState(const std::string& key, const std::string& jsonText);
	json ManagedState(const std::string& key, double* ageSeconds = nullptr, uint64_t* seq = nullptr);
	void RequestManaged(uint32_t mask);
	uint32_t TakeManagedRequests(); // main thread, every frame
	// Asks for `mask`, then waits until `key` has been published again (or the timeout). False on timeout.
	bool RefreshManaged(uint32_t mask, const std::string& key, uint32_t timeoutMs);

	// ---- crash reports
	void WriteCrashReport(json report);
	json CrashReports(); // {current session's report, previous session's, unclean shutdown flag}

	// ---- approvals: blocks the calling (IPC) thread until the player answers in the overlay or the timeout passes
	bool RequestApproval(const std::string& what, const std::string& detail, int timeoutSec, std::string& whyNot);
	bool CurrentApproval(ApprovalPrompt& out);
	void ResolveApproval(uint64_t id, bool approved);
	bool ApprovalPending() const { return approvalPending_.load(); }

private:
	void WorkerMain();

	std::atomic<bool> initialized_{false}, stopping_{false};
	std::mutex mutex_;
	std::wstring dataDir_, configDir_;
	bool previousCrashed_ = false;
	json currentCrash_;

	std::thread worker_;
	std::mutex workerMutex_;
	std::condition_variable workerCv_;
	std::deque<std::function<void()>> jobs_;
	bool workerStop_ = false;

	struct ManagedEntry { json value; uint64_t seq = 0; double time = 0; };
	std::mutex managedMutex_;
	std::condition_variable managedCv_;
	std::map<std::string, ManagedEntry> managed_;
	std::atomic<uint32_t> managedRequests_{0};

	std::mutex approvalMutex_;
	std::condition_variable approvalCv_;
	ApprovalPrompt approval_;
	int approvalResult_ = 0; // 0 waiting, 1 approved, 2 denied
	uint64_t nextApprovalId_ = 1;
	std::atomic<bool> approvalPending_{false};
	bool shuttingDown_ = false;
};

} // namespace aether
