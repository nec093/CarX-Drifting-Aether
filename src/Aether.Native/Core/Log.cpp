#include "Log.h"

#include <cstdarg>

namespace aether {

std::string WideToUtf8(const std::wstring& w) {
	if (w.empty()) {
		return {};
	}
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(size_t(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring Utf8ToWide(const std::string& s) {
	if (s.empty()) {
		return {};
	}
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
	std::wstring w(size_t(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
	return w;
}

std::string Format(const char* fmt, ...) {
	char stack[1024];
	va_list args;
	va_start(args, fmt);
	va_list copy;
	va_copy(copy, args);
	int n = vsnprintf(stack, sizeof(stack), fmt, args);
	va_end(args);
	if (n < 0) {
		va_end(copy);
		return fmt;
	}
	if (size_t(n) < sizeof(stack)) {
		va_end(copy);
		return std::string(stack, size_t(n));
	}
	std::string out(size_t(n), '\0');
	vsnprintf(out.data(), size_t(n) + 1, fmt, copy);
	va_end(copy);
	return out;
}

std::string HrToString(HRESULT hr) {
	const char* name = nullptr;
	switch (hr) {
	case DXGI_ERROR_DEVICE_REMOVED: name = "DXGI_ERROR_DEVICE_REMOVED"; break;
	case DXGI_ERROR_DEVICE_HUNG: name = "DXGI_ERROR_DEVICE_HUNG"; break;
	case DXGI_ERROR_DEVICE_RESET: name = "DXGI_ERROR_DEVICE_RESET"; break;
	case DXGI_ERROR_DRIVER_INTERNAL_ERROR: name = "DXGI_ERROR_DRIVER_INTERNAL_ERROR"; break;
	case DXGI_ERROR_INVALID_CALL: name = "DXGI_ERROR_INVALID_CALL"; break;
	case E_OUTOFMEMORY: name = "E_OUTOFMEMORY"; break;
	case E_INVALIDARG: name = "E_INVALIDARG"; break;
	case E_FAIL: name = "E_FAIL"; break;
	case E_NOINTERFACE: name = "E_NOINTERFACE"; break;
	case S_OK: name = "S_OK"; break;
	default: break;
	}
	return name ? Format("%s (0x%08X)", name, unsigned(hr)) : Format("0x%08X", unsigned(hr));
}

const char* SeverityName(Severity s) {
	switch (s) {
	case Severity::Debug: return "debug";
	case Severity::Info: return "info";
	case Severity::Warning: return "warning";
	case Severity::Error: return "error";
	case Severity::Fatal: return "fatal";
	}
	return "info";
}

json ToJson(const LogEntry& e) {
	json j = {{"seq", e.seq}, {"timestamp", e.timestamp}, {"severity", SeverityName(e.severity)}, {"component", e.component}, {"message", e.message}};
	if (!e.code.empty()) {
		j["code"] = e.code;
	}
	if (!e.resource.empty()) {
		j["resource"] = e.resource;
	}
	if (!e.extra.is_null()) {
		j["extra"] = e.extra;
	}
	return j;
}

static const size_t kRingSize = 2000;

Log& Log::Get() {
	static Log* log = new Log(); // never destroyed: the log must outlive every static (DLL unload order)
	return *log;
}

void Log::Open(const std::wstring& dir) {
	std::lock_guard lock(mutex_);
	dir_ = dir;
	CreateDirectoryW(dir.c_str(), nullptr);
	std::wstring path = dir + L"\\aether.jsonl";
	std::wstring prev = dir + L"\\aether.prev.jsonl";
	MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
	if (file_ != INVALID_HANDLE_VALUE) {
		CloseHandle(file_);
	}
	file_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Log::Close() {
	std::lock_guard lock(mutex_);
	if (file_ != INVALID_HANDLE_VALUE) {
		CloseHandle(file_);
		file_ = INVALID_HANDLE_VALUE;
	}
}

std::wstring Log::Directory() {
	std::lock_guard lock(mutex_);
	return dir_;
}

void Log::Write(Severity s, const char* component, const char* code, const std::string& message, const std::string& resource, json extra) {
	SYSTEMTIME t;
	GetLocalTime(&t);
	LogEntry e;
	e.timestamp = Format("%04u-%02u-%02uT%02u:%02u:%02u.%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	e.severity = s;
	e.component = component ? component : "";
	e.code = code ? code : "";
	e.message = message;
	e.resource = resource;
	e.extra = std::move(extra);
	if (s >= Severity::Error) {
		errors_.fetch_add(1);
	}

	std::lock_guard lock(mutex_);
	e.seq = ++seq_;
	if (file_ != INVALID_HANDLE_VALUE) {
		std::string line = Dump(ToJson(e)) + "\n";
		DWORD written = 0;
		WriteFile(file_, line.data(), DWORD(line.size()), &written, nullptr);
		if (s >= Severity::Error) {
			FlushFileBuffers(file_);
		}
	}
	if (ring_.size() < kRingSize) {
		ring_.push_back(std::move(e));
	} else {
		ring_[head_] = std::move(e);
		head_ = (head_ + 1) % kRingSize;
	}
}

std::vector<LogEntry> Log::Snapshot(size_t maxCount, Severity minSeverity, uint64_t afterSeq, const std::string& component) {
	std::lock_guard lock(mutex_);
	std::vector<LogEntry> out;
	size_t n = ring_.size();
	// walk newest to oldest, then reverse
	for (size_t i = 0; i < n && out.size() < maxCount; ++i) {
		size_t index = (head_ + n - 1 - i) % n;
		const LogEntry& e = ring_[index];
		if (e.seq <= afterSeq) {
			break;
		}
		if (e.severity < minSeverity) {
			continue;
		}
		if (!component.empty() && e.component != component) {
			continue;
		}
		out.push_back(e);
	}
	std::reverse(out.begin(), out.end());
	return out;
}

uint64_t Log::LastSeq() {
	std::lock_guard lock(mutex_);
	return seq_;
}

} // namespace aether
