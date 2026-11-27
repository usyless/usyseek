#pragma once

#include "common.hpp"

#include <usylibpp/windows/http.hpp>

namespace usyseek::http {

using usylibpp::windows::http::Progress;
using usylibpp::windows::http::Result;

inline std::optional<std::string> get(const std::wstring& url, size_t max_bytes = 16ull << 20u, const std::stop_token& stop = {}) {
    return usylibpp::windows::http::get(url, max_bytes, stop, APP_NAME);
}

inline Result download(const std::wstring& url, const fs::path& dest, const Progress& progress = {}, const std::stop_token& stop = {}) {
    return usylibpp::windows::http::download(url, dest, progress, stop, APP_NAME);
}

} // namespace usyseek::http
