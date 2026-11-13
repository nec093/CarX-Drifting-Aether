// HLSL shaders compiled at run time from <data dir>/shaders: a compile error is data (kept per shader, shown in the
// overlay and to MCP), never a crash, and shaders can be reloaded without a restart.
// Compiled with DXC for shader model 6.5 (inline ray tracing): dxcompiler.dll and dxil.dll, the Windows SDK's
// redistributables, next to the native runtime. HLSL 2018 rules.
#pragma once
#include "../Core/Common.h"
#include "../Core/Json.h"

#include <d3dcommon.h>
#include <map>

namespace aether {

struct ShaderStatus {
	std::string key;      // file:entry:target
	std::string file, entry, target;
	bool ok = false;
	std::string errors;   // compiler output (errors and warnings)
	double compileMs = 0;
	size_t bytecodeSize = 0;
	uint64_t generation = 0;
	std::string compiledAt;
};

class ShaderManager {
public:
	static ShaderManager& Get();
	void SetDirectory(const std::wstring& dir);
	// Render thread. Returns null on failure (the reason is in the status and the log).
	// `define`: one macro set to 1 for this compile (part of the key), or null.
	ComPtr<ID3DBlob> Compile(const char* file, const char* entry, const char* target, const char* define = nullptr);
	// Render thread: DXC could be loaded. Tried once.
	bool DxcAvailable();
	// Any thread: asks the renderer to rebuild its pipelines from the files on disk at the next frame.
	void RequestReload();
	uint64_t Generation() const { return generation_.load(); }
	json List();
	json Errors();
	size_t ErrorCount();

private:
	std::mutex mutex_;
	std::wstring dir_;
	std::map<std::string, ShaderStatus> status_;
	std::atomic<uint64_t> generation_{1};
	int dxc_ = -1;              // -1 not tried, 0 missing, 1 loaded
	void* dxcCreate_ = nullptr; // DxcCreateInstance
	std::string dxcWhere_;
	ComPtr<ID3DBlob> CompileDxc(const std::wstring& path, const char* entry, const char* target, const char* define, std::string& errors, HRESULT& hr);
};

} // namespace aether
