#include "Runtime.h"
#include "Core/DevOptions.h"
#include "Core/Config.h"
#include "Core/Log.h"
#include "Device/GpuInfo.h"

#include <fstream>
#include <sstream>

namespace aether {

Runtime& Runtime::Get() {
	static Runtime* r = new Runtime();
	return *r;
}

static void EnsureDir(const std::wstring& dir) {
	// creates every missing level
	for (size_t i = 3; i <= dir.size(); ++i) {
		if (i == dir.size() || dir[i] == L'\\' || dir[i] == L'/') {
			CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
		}
	}
}

static json ReadJsonFile(const std::wstring& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return nullptr;
	}
	std::stringstream buffer;
	buffer << in.rdbuf();
	json j = json::parse(buffer.str(), nullptr, false);
	return j.is_discarded() ? json(nullptr) : j;
}

void Runtime::Initialize(const std::wstring& dataDir, const std::wstring& configDir) {
	if (initialized_.exchange(true)) {
		return;
	}
	{
		std::lock_guard lock(mutex_);
		dataDir_ = dataDir;
		configDir_ = configDir;
	}
	{
		std::lock_guard lock(approvalMutex_);
		shuttingDown_ = false;
	}
	stopping_.store(false);
	EnsureDir(dataDir);
	EnsureDir(configDir);
	std::wstring logs = Subdir(L"logs");
	Log::Get().Open(logs);

	// a session marker: still there at the next start = the last session did not shut down cleanly
	std::wstring marker = logs + L"\\session.running";
	previousCrashed_ = GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
	{
		std::ofstream out(marker, std::ios::binary | std::ios::trunc);
		out << GetCurrentProcessId() << "\n";
	}
	// keep the last session's crash report readable as "previous"
	MoveFileExW((logs + L"\\crash-current.json").c_str(), (logs + L"\\crash-previous.json").c_str(), MOVEFILE_REPLACE_EXISTING);

	Settings::Get().Load(configDir + L"\\aether.json");
	DevOptions::Get().Load(configDir);
	if (DevOptions::Get().source != "defaults") {
		Log::Get().Write(Severity::Warning, "Runtime", "W_DEV_OPTIONS", "Experiment switches are active (dev-options.json in the config folder)", {}, DevOptions::Get().ToJson());
	}
	{
		std::lock_guard lock(workerMutex_);
		workerStop_ = false;
	}
	worker_ = std::thread([this] { WorkerMain(); });
	AE_LOG(Info, "Runtime", "Aether native runtime started (pid %lu, data %s)%s", GetCurrentProcessId(), WideToUtf8(dataDir).c_str(),
		previousCrashed_ ? "; the previous session did not shut down cleanly" : "");
}

void Runtime::BeginShutdown() {
	stopping_.store(true);
	{
		std::lock_guard lock(approvalMutex_);
		shuttingDown_ = true;
		if (approvalResult_ == 0) {
			approvalResult_ = 2;
		}
	}
	approvalCv_.notify_all();
	{
		std::lock_guard lock(managedMutex_);
	}
	managedCv_.notify_all();
}

void Runtime::Shutdown() {
	BeginShutdown();
	if (!initialized_.exchange(false)) {
		return;
	}
	{
		std::lock_guard lock(workerMutex_);
		workerStop_ = true;
	}
	workerCv_.notify_all();
	if (worker_.joinable()) {
		worker_.join();
	}
	Settings::Get().Save();
	AE_LOG(Info, "Runtime", "Aether native runtime stopped cleanly");
	std::wstring marker = Subdir(L"logs") + L"\\session.running";
	DeleteFileW(marker.c_str());
	Log::Get().Close();
}

std::wstring Runtime::DataDir() {
	std::lock_guard lock(mutex_);
	return dataDir_;
}

std::wstring Runtime::Subdir(const wchar_t* name) {
	std::wstring dir = DataDir() + L"\\" + name;
	EnsureDir(dir);
	return dir;
}

void Runtime::Post(std::function<void()> job) {
	{
		std::lock_guard lock(workerMutex_);
		if (workerStop_) {
			return;
		}
		jobs_.push_back(std::move(job));
	}
	workerCv_.notify_one();
}

void Runtime::WorkerMain() {
	SetThreadDescription(GetCurrentThread(), L"Aether worker");
	for (;;) {
		std::function<void()> job;
		{
			std::unique_lock lock(workerMutex_);
			workerCv_.wait(lock, [this] { return workerStop_ || !jobs_.empty(); });
			if (jobs_.empty()) {
				return; // stop requested and nothing left
			}
			job = std::move(jobs_.front());
			jobs_.pop_front();
		}
		try {
			job();
		} catch (const std::exception& e) {
			AE_LOG_CODE(Error, "Runtime", "E_WORKER_JOB", "A background job failed: %s", e.what());
		}
	}
}

// ---- managed state

void Runtime::SetManagedState(const std::string& key, const std::string& jsonText) {
	json value = json::parse(jsonText, nullptr, false);
	if (value.is_discarded()) {
		AE_LOG_CODE(Warning, "Runtime", "W_MANAGED_JSON", "The managed side published invalid JSON for '%s'", key.c_str());
		return;
	}
	{
		std::lock_guard lock(managedMutex_);
		ManagedEntry& e = managed_[key];
		e.value = std::move(value);
		e.seq++;
		e.time = QpcSeconds();
	}
	managedCv_.notify_all();
}

json Runtime::ManagedState(const std::string& key, double* ageSeconds, uint64_t* seq) {
	std::lock_guard lock(managedMutex_);
	auto it = managed_.find(key);
	if (it == managed_.end()) {
		if (ageSeconds) *ageSeconds = -1;
		if (seq) *seq = 0;
		return nullptr;
	}
	if (ageSeconds) *ageSeconds = QpcSeconds() - it->second.time;
	if (seq) *seq = it->second.seq;
	return it->second.value;
}

void Runtime::RequestManaged(uint32_t mask) {
	managedRequests_.fetch_or(mask);
}

uint32_t Runtime::TakeManagedRequests() {
	return managedRequests_.exchange(0);
}

bool Runtime::RefreshManaged(uint32_t mask, const std::string& key, uint32_t timeoutMs) {
	uint64_t before = 0;
	ManagedState(key, nullptr, &before);
	RequestManaged(mask);
	std::unique_lock lock(managedMutex_);
	return managedCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
		auto it = managed_.find(key);
		return stopping_.load() || (it != managed_.end() && it->second.seq > before);
	}) && !stopping_.load();
}

// ---- crash reports

void Runtime::WriteCrashReport(json report) {
	SYSTEMTIME t;
	GetLocalTime(&t);
	report["timestamp"] = Format("%04u-%02u-%02uT%02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	report["schema"] = "aether.crash/1";
	report["settings"] = Settings::Get().ToJson();
	std::wstring path = Subdir(L"logs") + L"\\crash-current.json";
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << Dump(report, 2) << "\n";
	}
	std::lock_guard lock(mutex_);
	currentCrash_ = std::move(report);
}

json Runtime::CrashReports() {
	json out;
	{
		std::lock_guard lock(mutex_);
		out["currentSession"] = currentCrash_;
	}
	std::wstring logs = Subdir(L"logs");
	out["previousSession"] = ReadJsonFile(logs + L"\\crash-previous.json");
	out["previousSessionEndedUncleanly"] = previousCrashed_;
	if (previousCrashed_) {
		// the tail of the previous run's log: the last things Aether did before the process went away
		std::ifstream in(logs + L"\\aether.prev.jsonl", std::ios::binary);
		std::deque<std::string> tail;
		std::string line;
		while (std::getline(in, line)) {
			tail.push_back(line);
			if (tail.size() > 30) {
				tail.pop_front();
			}
		}
		json lines = json::array();
		for (const std::string& l : tail) {
			json j = json::parse(l, nullptr, false);
			if (!j.is_discarded()) {
				lines.push_back(std::move(j));
			}
		}
		out["previousSessionLogTail"] = lines;
		out["note"] = "The previous session ended without a clean Aether shutdown (a game crash, a kill, or a crash outside Aether's guarded code). Aether installs no global crash handler; Unity's own crash dumps are in the game's Player.log folder.";
	}
	return out;
}

// ---- approvals

bool Runtime::RequestApproval(const std::string& what, const std::string& detail, int timeoutSec, std::string& whyNot) {
	std::unique_lock lock(approvalMutex_);
	if (shuttingDown_) {
		whyNot = "Aether is shutting down";
		return false;
	}
	if (approvalPending_.load()) {
		whyNot = "another request is already waiting for the player's approval";
		return false;
	}
	approval_.id = nextApprovalId_++;
	approval_.what = what;
	approval_.detail = detail;
	approval_.deadline = QpcSeconds() + timeoutSec;
	approvalResult_ = 0;
	approvalPending_.store(true);
	AE_LOG(Info, "Approval", "Waiting for the player's approval: %s %s", what.c_str(), detail.c_str());
	bool answered = approvalCv_.wait_for(lock, std::chrono::seconds(timeoutSec), [this] { return approvalResult_ != 0; });
	approvalPending_.store(false);
	if (!answered) {
		whyNot = Format("the player did not answer within %d seconds", timeoutSec);
		AE_LOG(Info, "Approval", "Timed out: %s", what.c_str());
		return false;
	}
	if (approvalResult_ != 1) {
		whyNot = shuttingDown_ ? "Aether is shutting down" : "the player denied the request";
		AE_LOG(Info, "Approval", "Denied: %s", what.c_str());
		return false;
	}
	AE_LOG(Info, "Approval", "Approved: %s", what.c_str());
	return true;
}

bool Runtime::CurrentApproval(ApprovalPrompt& out) {
	if (!approvalPending_.load()) {
		return false;
	}
	std::lock_guard lock(approvalMutex_);
	if (approvalResult_ != 0) {
		return false;
	}
	out = approval_;
	return true;
}

void Runtime::ResolveApproval(uint64_t id, bool approved) {
	{
		std::lock_guard lock(approvalMutex_);
		if (!approvalPending_.load() || approval_.id != id || approvalResult_ != 0) {
			return;
		}
		approvalResult_ = approved ? 1 : 2;
	}
	approvalCv_.notify_all();
}

} // namespace aether
