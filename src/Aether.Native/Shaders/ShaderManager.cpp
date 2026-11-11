#include "ShaderManager.h"
#include "../Core/Log.h"

#include <d3dcompiler.h>

namespace aether {

ShaderManager& ShaderManager::Get() {
	static ShaderManager* s = new ShaderManager();
	return *s;
}

void ShaderManager::SetDirectory(const std::wstring& dir) {
	std::lock_guard lock(mutex_);
	dir_ = dir;
}

void ShaderManager::RequestReload() {
	generation_.fetch_add(1);
	AE_LOG(Info, "Shader", "Shader reload requested");
}

static std::string NowString() {
	SYSTEMTIME t;
	GetLocalTime(&t);
	return Format("%04u-%02u-%02uT%02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

ComPtr<ID3DBlob> ShaderManager::Compile(const char* file, const char* entry, const char* target) {
	std::wstring dir;
	{
		std::lock_guard lock(mutex_);
		dir = dir_;
	}
	ShaderStatus st;
	st.file = file;
	st.entry = entry;
	st.target = target;
	st.key = st.file + ":" + st.entry + ":" + st.target;
	st.generation = generation_.load();
	st.compiledAt = NowString();

	std::wstring path = dir + L"\\" + Utf8ToWide(file);
	double start = QpcSeconds();
	ComPtr<ID3DBlob> code, errors;
	UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
	HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, flags, 0, &code, &errors);
	st.compileMs = (QpcSeconds() - start) * 1000.0;
	if (errors && errors->GetBufferSize() > 0) {
		st.errors.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
		while (!st.errors.empty() && (st.errors.back() == '\0' || st.errors.back() == '\n' || st.errors.back() == '\r')) {
			st.errors.pop_back();
		}
	}
	if (FAILED(hr)) {
		st.ok = false;
		if (st.errors.empty()) {
			st.errors = hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)
				? "shader file not found: " + WideToUtf8(path)
				: "D3DCompileFromFile failed: " + HrToString(hr);
		}
		Log::Get().Write(Severity::Error, "Shader", "E_SHADER_COMPILE", "Shader compile failed: " + st.key, st.file, {{"hresult", HrToString(hr)}, {"compilerOutput", st.errors}});
		code.Reset();
	} else {
		st.ok = true;
		st.bytecodeSize = code->GetBufferSize();
		AE_LOG(Debug, "Shader", "Compiled %s (%zu bytes, %.1f ms)", st.key.c_str(), st.bytecodeSize, st.compileMs);
	}
	{
		std::lock_guard lock(mutex_);
		status_[st.key] = st;
	}
	return code;
}

static json StatusJson(const ShaderStatus& s) {
	return {{"key", s.key}, {"file", s.file}, {"entry", s.entry}, {"target", s.target}, {"ok", s.ok}, {"compilerOutput", s.errors},
		{"compileMs", s.compileMs}, {"bytecodeBytes", s.bytecodeSize}, {"generation", s.generation}, {"compiledAt", s.compiledAt}};
}

json ShaderManager::List() {
	std::lock_guard lock(mutex_);
	json list = json::array();
	for (const auto& [key, s] : status_) {
		list.push_back(StatusJson(s));
	}
	return {{"directory", WideToUtf8(dir_)}, {"generation", generation_.load()}, {"compiler", "D3DCompile (d3dcompiler_47), shader model 5.1"}, {"shaders", list}};
}

json ShaderManager::Errors() {
	std::lock_guard lock(mutex_);
	json list = json::array();
	size_t warnings = 0;
	for (const auto& [key, s] : status_) {
		if (!s.ok) {
			list.push_back(StatusJson(s));
		} else if (!s.errors.empty()) {
			++warnings;
		}
	}
	return {{"errorCount", list.size()}, {"shadersWithWarnings", warnings}, {"shaderCount", status_.size()}, {"errors", list}};
}

size_t ShaderManager::ErrorCount() {
	std::lock_guard lock(mutex_);
	size_t n = 0;
	for (const auto& [key, s] : status_) {
		n += s.ok ? 0 : 1;
	}
	return n;
}

} // namespace aether
