#include "ShaderManager.h"
#include "../Core/Log.h"

#include <d3dcompiler.h>
#include <dxcapi.h>

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

static std::wstring ModuleDirectory() {
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ModuleDirectory), &self);
	wchar_t buffer[MAX_PATH] = {};
	GetModuleFileNameW(self, buffer, MAX_PATH);
	std::wstring path = buffer;
	const size_t slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

bool ShaderManager::DxcAvailable() {
	if (dxc_ >= 0) {
		return dxc_ == 1;
	}
	std::wstring dir;
	{
		std::lock_guard lock(mutex_);
		dir = dir_;
	}
	// The mod's native folder (beside the shaders folder: the runtime itself runs from the game's plugin folder), else
	// the runtime's own folder (the test host). dxil.dll first, by its full path: dxcompiler.dll asks for it by name
	// only, which would search the game's folder.
	std::vector<std::wstring> places;
	const size_t slash = dir.find_last_of(L"\\/");
	if (slash != std::wstring::npos) {
		places.push_back(dir.substr(0, slash) + L"\\native");
	}
	places.push_back(ModuleDirectory());
	dxc_ = 0;
	for (const std::wstring& place : places) {
		const std::wstring dxilPath = place + L"\\dxil.dll", compilerPath = place + L"\\dxcompiler.dll";
		if (GetFileAttributesW(dxilPath.c_str()) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(compilerPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
			continue;
		}
		HMODULE dxil = LoadLibraryW(dxilPath.c_str());
		HMODULE compiler = dxil ? LoadLibraryW(compilerPath.c_str()) : nullptr;
		void* create = compiler ? reinterpret_cast<void*>(GetProcAddress(compiler, "DxcCreateInstance")) : nullptr;
		if (create) {
			dxcCreate_ = create;
			dxcWhere_ = WideToUtf8(place);
			dxc_ = 1;
			AE_LOG(Info, "Shader", "DXC loaded from %s (shader model 6 targets compile)", dxcWhere_.c_str());
			return true;
		}
	}
	AE_LOG_CODE(Error, "Shader", "E_DXC_MISSING", "dxcompiler.dll / dxil.dll were not found next to the native runtime: no shader compiles (Aether draws nothing)");
	return false;
}

ComPtr<ID3DBlob> ShaderManager::CompileDxc(const std::wstring& path, const char* entry, const char* target, const char* define, std::string& errors, HRESULT& hr) {
	auto create = reinterpret_cast<DxcCreateInstanceProc>(dxcCreate_);
	ComPtr<IDxcUtils> utils;
	ComPtr<IDxcCompiler3> compiler;
	ComPtr<IDxcIncludeHandler> includes;
	hr = create(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
	if (SUCCEEDED(hr)) {
		hr = create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
	}
	if (SUCCEEDED(hr)) {
		hr = utils->CreateDefaultIncludeHandler(&includes);
	}
	ComPtr<IDxcBlobEncoding> source;
	if (SUCCEEDED(hr)) {
		hr = utils->LoadFile(path.c_str(), nullptr, &source);
	}
	if (FAILED(hr)) {
		return nullptr;
	}
	const std::wstring wentry = Utf8ToWide(entry), wtarget = Utf8ToWide(target), wdefine = define ? Utf8ToWide(define) + L"=1" : L"";
	std::vector<LPCWSTR> args = {path.c_str(), L"-E", wentry.c_str(), L"-T", wtarget.c_str(), L"-HV", L"2018", L"-O3"};
	if (define) {
		args.push_back(L"-D");
		args.push_back(wdefine.c_str());
	}
	DxcBuffer buffer = {source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_ACP};
	ComPtr<IDxcResult> result;
	hr = compiler->Compile(&buffer, args.data(), UINT32(args.size()), includes.Get(), IID_PPV_ARGS(&result));
	if (FAILED(hr)) {
		return nullptr;
	}
	ComPtr<IDxcBlobUtf8> text;
	if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&text), nullptr)) && text && text->GetStringLength() > 0) {
		errors.assign(text->GetStringPointer(), text->GetStringLength());
	}
	HRESULT status = S_OK;
	result->GetStatus(&status);
	if (FAILED(status)) {
		hr = status;
		return nullptr;
	}
	ComPtr<IDxcBlob> object;
	ComPtr<ID3DBlob> out;
	hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
	if (SUCCEEDED(hr) && object) {
		hr = D3DCreateBlob(object->GetBufferSize(), &out);
	}
	if (SUCCEEDED(hr) && out) {
		memcpy(out->GetBufferPointer(), object->GetBufferPointer(), object->GetBufferSize());
	}
	return out;
}

ComPtr<ID3DBlob> ShaderManager::Compile(const char* file, const char* entry, const char* target, const char* define) {
	std::wstring dir;
	{
		std::lock_guard lock(mutex_);
		dir = dir_;
	}
	ShaderStatus st;
	st.file = file;
	st.entry = entry;
	st.target = target;
	st.key = st.file + ":" + st.entry + ":" + st.target + (define ? std::string(":") + define : std::string());
	st.generation = generation_.load();
	st.compiledAt = NowString();

	std::wstring path = dir + L"\\" + Utf8ToWide(file);
	double start = QpcSeconds();
	ComPtr<ID3DBlob> code;
	HRESULT hr = E_FAIL;
	if (DxcAvailable()) {
		code = CompileDxc(path, entry, target, define, st.errors, hr);
		if (SUCCEEDED(hr) && !code) {
			hr = E_FAIL;
		}
	} else {
		st.errors = "DXC (dxcompiler.dll, dxil.dll) was not found next to the native runtime";
	}
	st.compileMs = (QpcSeconds() - start) * 1000.0;
	while (!st.errors.empty() && (st.errors.back() == '\0' || st.errors.back() == '\n' || st.errors.back() == '\r')) {
		st.errors.pop_back();
	}
	if (FAILED(hr)) {
		st.ok = false;
		if (st.errors.empty()) {
			st.errors = hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)
				? "shader file not found: " + WideToUtf8(path)
				: std::string("DXC failed: ") + HrToString(hr);
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
	return {{"directory", WideToUtf8(dir_)}, {"generation", generation_.load()}, {"compiler", "DXC, shader model 6.5, HLSL 2018"}, {"dxc", dxc_ == 1 ? dxcWhere_ : dxc_ == 0 ? "missing" : "not tried"}, {"shaders", list}};
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
