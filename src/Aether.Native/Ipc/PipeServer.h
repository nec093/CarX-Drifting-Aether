// Local IPC for the MCP server: a named pipe (\\.\pipe\Aether.Runtime, local clients of the same user only) carrying
// one JSON request per line and one JSON response per line. Requests are served on the pipe's own threads from
// snapshots; nothing here runs on, or waits for, the render thread.
#pragma once
#include "../Core/Common.h"
#include "../Core/Json.h"

#include <thread>

namespace aether {

// {"id":..,"method":"get_gpu_info","params":{..},"control":false} -> {"id":..,"ok":true,"result":{..}}
//                                                                   | {"id":..,"ok":false,"error":{"code":"..","message":".."}}
json HandleRequest(const json& request);

class PipeServer {
public:
	static PipeServer& Get();
	void Start();
	void Stop();
	std::wstring Name();
	int ClientCount() const { return clients_.load(); }

private:
	void AcceptMain();
	void ClientMain(HANDLE pipe);

	std::wstring name_;
	std::thread acceptThread_;
	std::atomic<bool> stop_{false}, running_{false};
	std::atomic<int> clients_{0};
	std::mutex mutex_;
	struct Client { HANDLE pipe; std::thread thread; std::shared_ptr<std::atomic<bool>> done; };
	std::vector<Client> clientThreads_;
};

} // namespace aether
