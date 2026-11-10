#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace usyseek {

inline constexpr std::string_view default_cuda_version = "cu126";
inline constexpr std::array<std::string_view, 4> cuda_versions = {"cu126", "cu130", "cu132", "cu134"};

inline std::string normalize_cuda_version(std::string_view raw) {
    std::string v{raw};
    std::ranges::transform(v, v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool digits = v.size() > 2 && std::all_of(v.begin() + 2, v.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    if (!v.starts_with("cu") || !digits) {
        return std::string{default_cuda_version};
    }
    return v;
}

inline std::string torch_index_url(std::string_view backend, std::string_view cuda_version) {
    const std::string variant = backend == "cuda" ? normalize_cuda_version(cuda_version) : "cpu";
    return "https://download.pytorch.org/whl/" + variant;
}

}  // namespace usyseek
