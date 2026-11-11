#pragma once
// nlohmann/json without exceptions leaking into the render thread: callers use json::parse(text, nullptr, false) and
// check is_discarded(), and the value(key, default) accessors.
#include <nlohmann/json.hpp>

namespace aether {
using json = nlohmann::json;

// dump that never throws on invalid UTF-8 (replaces bad sequences)
inline std::string Dump(const json& j, int indent = -1) {
	return j.dump(indent, ' ', false, json::error_handler_t::replace);
}
} // namespace aether
