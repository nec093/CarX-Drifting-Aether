#include "PipeServer.h"
#include "../Core/Log.h"

#include <sddl.h>

namespace aether {

static const DWORD kMaxInstances = 4;
static const size_t kMaxRequestBytes = 1 << 20;

PipeServer& PipeServer::Get() {
	static PipeServer* p = new PipeServer();
	return *p;
}

std::wstring PipeServer::Name() {
	std::lock_guard lock(mutex_);
	return name_;
}

// A security descriptor that lets only the current user (and SYSTEM) open the pipe. Null = the default descriptor.
static PSECURITY_DESCRIPTOR CurrentUserOnly() {
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		return nullptr;
	}
	BYTE buffer[256];
	DWORD size = 0;
	PSECURITY_DESCRIPTOR sd = nullptr;
	if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size)) {
		LPWSTR sid = nullptr;
		if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &sid)) {
			std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")(A;;GA;;;SY)";
			LocalFree(sid);
			ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr);
		}
	}
	CloseHandle(token);
	return sd;
}

void PipeServer::Start() {
	if (running_.exchange(true)) {
		return;
	}
	wchar_t custom[128];
	DWORD n = GetEnvironmentVariableW(L"AETHER_PIPE_NAME", custom, _countof(custom));
	{
		std::lock_guard lock(mutex_);
		name_ = std::wstring(L"\\\\.\\pipe\\") + ((n > 0 && n < _countof(custom)) ? custom : L"Aether.Runtime");
	}
	stop_.store(false);
	acceptThread_ = std::thread([this] { AcceptMain(); });
}

void PipeServer::Stop() {
	if (!running_.exchange(false)) {
		return;
	}
	stop_.store(true);
	// wake the accept thread out of ConnectNamedPipe
	HANDLE self = CreateFileW(Name().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	if (self != INVALID_HANDLE_VALUE) {
		CloseHandle(self);
	}
	if (acceptThread_.joinable()) {
		acceptThread_.join();
	}
	std::vector<Client> clients;
	{
		std::lock_guard lock(mutex_);
		clients.swap(clientThreads_);
	}
	for (Client& c : clients) {
		if (!c.done->load()) {
			CancelIoEx(c.pipe, nullptr); // unblocks the client thread's ReadFile
		}
	}
	for (Client& c : clients) {
		if (c.thread.joinable()) {
			c.thread.join();
		}
	}
}

void PipeServer::AcceptMain() {
	SetThreadDescription(GetCurrentThread(), L"Aether IPC accept");
	PSECURITY_DESCRIPTOR sd = CurrentUserOnly();
	SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
	std::wstring name = Name();
	bool logged = false;
	while (!stop_.load()) {
		HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			kMaxInstances, 1 << 16, 1 << 16, 0, sd ? &sa : nullptr);
		if (pipe == INVALID_HANDLE_VALUE) {
			DWORD error = GetLastError();
			if (error != ERROR_PIPE_BUSY && !logged) {
				AE_LOG_CODE(Error, "Ipc", "E_PIPE_CREATE", "Cannot create the pipe %s (error %lu): MCP has no connection to this session", WideToUtf8(name).c_str(), error);
				logged = true;
			}
			Sleep(250); // all instances taken, or a transient failure
			continue;
		}
		if (!logged) {
			AE_LOG(Info, "Ipc", "Listening on %s (local clients of this user only)", WideToUtf8(name).c_str());
			logged = true;
		}
		BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
		if (stop_.load() || !connected) {
			CloseHandle(pipe);
			continue;
		}
		std::lock_guard lock(mutex_);
		// forget finished clients
		for (size_t i = 0; i < clientThreads_.size();) {
			if (clientThreads_[i].done->load()) {
				clientThreads_[i].thread.join();
				clientThreads_.erase(clientThreads_.begin() + ptrdiff_t(i));
			} else {
				++i;
			}
		}
		Client c;
		c.pipe = pipe;
		c.done = std::make_shared<std::atomic<bool>>(false);
		auto done = c.done;
		c.thread = std::thread([this, pipe, done] {
			ClientMain(pipe);
			done->store(true);
		});
		clientThreads_.push_back(std::move(c));
	}
	if (sd) {
		LocalFree(sd);
	}
}

void PipeServer::ClientMain(HANDLE pipe) {
	SetThreadDescription(GetCurrentThread(), L"Aether IPC client");
	clients_.fetch_add(1);
	AE_LOG(Info, "Ipc", "A client connected");
	std::string pending;
	char buffer[4096];
	bool alive = true;
	while (alive && !stop_.load()) {
		DWORD read = 0;
		if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
			break; // disconnected or cancelled
		}
		pending.append(buffer, read);
		if (pending.size() > kMaxRequestBytes) {
			break; // not a client that speaks this protocol
		}
		size_t newline;
		while (alive && (newline = pending.find('\n')) != std::string::npos) {
			std::string line = pending.substr(0, newline);
			pending.erase(0, newline + 1);
			if (line.find_first_not_of(" \r\t") == std::string::npos) {
				continue;
			}
			json response;
			json request = json::parse(line, nullptr, false);
			if (request.is_discarded() || !request.is_object()) {
				response = {{"id", nullptr}, {"ok", false}, {"error", {{"code", "E_BAD_REQUEST"}, {"message", "the request is not a JSON object"}}}};
			} else {
				try {
					response = HandleRequest(request);
				} catch (const std::exception& e) {
					response = {{"id", request.contains("id") ? request["id"] : json(nullptr)}, {"ok", false}, {"error", {{"code", "E_INTERNAL"}, {"message", e.what()}}}};
					AE_LOG_CODE(Error, "Ipc", "E_REQUEST", "A request failed: %s", e.what());
				}
			}
			std::string out = Dump(response) + "\n";
			size_t offset = 0;
			while (offset < out.size()) {
				DWORD written = 0;
				if (!WriteFile(pipe, out.data() + offset, DWORD(std::min<size_t>(out.size() - offset, 1 << 16)), &written, nullptr) || written == 0) {
					alive = false;
					break;
				}
				offset += written;
			}
		}
	}
	DisconnectNamedPipe(pipe);
	CloseHandle(pipe);
	clients_.fetch_sub(1);
	AE_LOG(Info, "Ipc", "A client disconnected");
}

} // namespace aether
