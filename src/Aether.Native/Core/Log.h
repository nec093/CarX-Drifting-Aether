// Structured log: every entry is one JSON object (JSON Lines on disk, a ring buffer in memory for the overlay and MCP).
#pragma once
#include "Common.h"
#include "Json.h"

namespace aether {

enum class Severity { Debug = 0, Info = 1, Warning = 2, Error = 3, Fatal = 4 };
const char* SeverityName(Severity s);

struct LogEntry {
	uint64_t seq = 0;
	std::string timestamp; // ISO 8601, local time with milliseconds
	Severity severity = Severity::Info;
	std::string component;
	std::string code;      // stable error code ("" for plain messages), e.g. "E_DEVICE_REMOVED"
	std::string message;
	std::string resource;  // relevant resource name, if any
	json extra;            // optional structured details (hresult, gpu, ...)
};

json ToJson(const LogEntry& e);

class Log {
public:
	static Log& Get();
	void Open(const std::wstring& dir); // logs/aether.jsonl (the previous run is kept as aether.prev.jsonl)
	void Close();
	void Write(Severity s, const char* component, const char* code, const std::string& message, const std::string& resource = {}, json extra = nullptr);
	// newest last; minSeverity filters; afterSeq returns only entries with seq > afterSeq
	std::vector<LogEntry> Snapshot(size_t maxCount, Severity minSeverity = Severity::Debug, uint64_t afterSeq = 0, const std::string& component = {});
	uint64_t LastSeq();
	uint32_t ErrorCount() const { return errors_.load(); }
	std::wstring Directory();

private:
	std::mutex mutex_;
	std::vector<LogEntry> ring_;
	size_t head_ = 0;
	uint64_t seq_ = 0;
	std::atomic<uint32_t> errors_{0};
	HANDLE file_ = INVALID_HANDLE_VALUE;
	std::wstring dir_;
};

#define AE_LOG(sev, component, ...) ::aether::Log::Get().Write(::aether::Severity::sev, component, "", ::aether::Format(__VA_ARGS__))
#define AE_LOG_CODE(sev, component, code, ...) ::aether::Log::Get().Write(::aether::Severity::sev, component, code, ::aether::Format(__VA_ARGS__))

} // namespace aether
