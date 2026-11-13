// Switches for controlled experiments on the integration with the host (docs/architecture/dx12-integration.md).
// Read once at start from <config dir>\dev-options.json; without that file everything is at its default and this
// header changes nothing. Not settings: nothing here is shown in the overlay, saved, or reachable through MCP.
#pragma once
#include "Common.h"
#include "Json.h"

#include <fstream>

namespace aether {

struct DevOptions {
	// How a recorded list reaches the GPU.
	//   false: IUnityGraphicsD3D12v7::ExecuteCommandList (Unity runs it "on a worker thread")
	//   true:  ID3D12CommandQueue::ExecuteCommandLists on Unity's queue, inside the event (queue access "Allow")
	bool directSubmit = false;
	// Event configuration overrides, -1 = Aether's default. Flags are UnityD3D12EventConfigFlagBits.
	int beginFlags = -1, endFlags = -1, sceneFlags = -1;
	// Completion is taken from the markers alone: Aether's own fence is never signalled on the host's queue.
	bool noSignal = false;
	// The frame-end event does exactly one kind of D3D12 call and nothing else:
	//   1 = IDXGISwapChain::GetBuffer on the host's swap chain (and Release)
	//   2 = Reset and Close of one command allocator / list of Aether's own (nothing recorded, nothing submitted)
	//   3 = CreateRenderTargetView on the back buffer (GetBuffer included)
	int probe = 0;
	// Lists are recorded and closed but never submitted (isolates recording from submission).
	bool recordOnly = false;
	// The frame-end event only waits this long on the render thread (no D3D12 call of Aether's is needed for it):
	// tells a race that merely depends on the render thread's timing from one that depends on what Aether submits.
	int sleepMs = 0;
	// Scene capture experiments (read by the managed side): 1 = no textures at all, 2 = no textures Unity keeps
	// changing (render textures, textures with a non-zero update count), 4 = never ask again for a texture's pointer.
	int captureFlags = 0;
	// Seconds between the "Submission statistics" log lines (0 = off).
	int statsSeconds = 0;
	std::string source = "defaults";

	static DevOptions& Get() {
		static DevOptions o;
		return o;
	}

	void Load(const std::wstring& configDir) {
		std::ifstream in(configDir + L"\\dev-options.json");
		if (!in) {
			return;
		}
		json j = json::parse(in, nullptr, false);
		if (!j.is_object()) {
			source = "dev-options.json (not valid JSON, ignored)";
			return;
		}
		directSubmit = j.value("directSubmit", directSubmit);
		beginFlags = j.value("beginFlags", beginFlags);
		endFlags = j.value("endFlags", endFlags);
		sceneFlags = j.value("sceneFlags", sceneFlags);
		recordOnly = j.value("recordOnly", recordOnly);
		probe = j.value("probe", probe);
		noSignal = j.value("noSignal", noSignal);
		sleepMs = j.value("sleepMs", sleepMs);
		captureFlags = j.value("captureFlags", captureFlags);
		statsSeconds = j.value("statsSeconds", statsSeconds);
		source = "dev-options.json";
	}

	json ToJson() const {
		return {{"source", source}, {"directSubmit", directSubmit}, {"beginFlags", beginFlags}, {"endFlags", endFlags}, {"sceneFlags", sceneFlags}, {"recordOnly", recordOnly}, {"probe", probe}, {"noSignal", noSignal}, {"sleepMs", sleepMs}, {"captureFlags", captureFlags},
			{"statsSeconds", statsSeconds}};
	}
};

} // namespace aether
