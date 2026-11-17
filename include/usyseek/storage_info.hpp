#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace usyseek {

struct DbCollectionInfo {
    std::string name;
    std::optional<uint64_t> count;
    uint64_t bytes = 0;

    bool operator==(const DbCollectionInfo&) const = default;
};

struct ModelStorageInfo {
    std::string name;
    bool weights = false;
    bool tokenizer = false;
    bool onnx = false;
    bool partial = false;
    uint64_t bytes = 0;

    bool operator==(const ModelStorageInfo&) const = default;
};

namespace detail {

[[nodiscard]] inline std::string path_to_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

[[nodiscard]] inline std::filesystem::path utf8_to_path(std::string_view s) {
    return std::filesystem::path{std::u8string_view{reinterpret_cast<const char8_t*>(s.data()), s.size()}};
}

[[nodiscard]] inline bool ascii_iless(std::string_view a, std::string_view b) noexcept {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const auto ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
        const auto cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) return ca < cb;
    }
    if (a.size() != b.size()) return a.size() < b.size();
    return a < b;
}

[[nodiscard]] inline bool valid_model_stem(std::string_view name) noexcept {
    return !name.empty() && name != "." && name != ".." && !name.contains('/') && !name.contains('\\');
}

[[nodiscard]] inline uint64_t directory_bytes(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    uint64_t total = 0;
    for (fs::recursive_directory_iterator it{root, fs::directory_options::skip_permission_denied, ec}, end; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (it->is_symlink(fec) || !it->is_regular_file(fec)) continue;
        const uint64_t sz = it->file_size(fec);
        if (!fec) total += sz;
    }
    return total;
}

} // namespace detail

/// Scans `model_dir` for downloaded model weights, tokenizers, exported text ONNX files, and partial downloads.
[[nodiscard]] inline std::vector<ModelStorageInfo> scan_downloaded_models(const std::filesystem::path& model_dir) {
    namespace fs = std::filesystem;
    std::vector<ModelStorageInfo> out;
    std::error_code ec;
    if (model_dir.empty() || !fs::exists(model_dir, ec)) return out;

    enum class Kind : uint8_t { Weights, Tokenizer, Onnx, Partial };
    struct SuffixRule {
        std::string_view suffix;
        Kind kind;
    };
    static constexpr std::array<SuffixRule, 7> rules = {{
        {".safetensors.part", Kind::Partial},
        {".safetensors", Kind::Weights},
        {"-tokenizer.json.part", Kind::Partial},
        {"-tokenizer.json", Kind::Tokenizer},
        {"-text.onnx.data", Kind::Onnx},
        {"-text.onnx.tmp", Kind::Partial},
        {"-text.onnx", Kind::Onnx},
    }};

    std::map<std::string, ModelStorageInfo> by_name;
    for (const auto& entry : fs::directory_iterator(model_dir, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code fec;
        if (entry.is_symlink(fec) || !entry.is_regular_file(fec)) continue;
        const std::string fname = detail::path_to_utf8(entry.path().filename());
        for (const SuffixRule& r : rules) {
            if (fname.size() <= r.suffix.size() || !fname.ends_with(r.suffix)) continue;
            const std::string stem = fname.substr(0, fname.size() - r.suffix.size());
            if (!detail::valid_model_stem(stem)) break;
            auto& info = by_name[stem];
            if (info.name.empty()) info.name = stem;
            switch (r.kind) {
                case Kind::Weights: info.weights = true; break;
                case Kind::Tokenizer: info.tokenizer = true; break;
                case Kind::Onnx: info.onnx = true; break;
                case Kind::Partial: info.partial = true; break;
            }
            const uint64_t sz = entry.file_size(fec);
            if (!fec) info.bytes += sz;
            break;
        }
    }

    out.reserve(by_name.size());
    for (auto& [_, info] : by_name) out.push_back(std::move(info));
    std::ranges::sort(out, [](const ModelStorageInfo& a, const ModelStorageInfo& b) {
        return detail::ascii_iless(a.name, b.name);
    });
    return out;
}

/// Returns the model-specific files belonging to `name` inside `model_dir` (never shared `tokenizer.json`).
[[nodiscard]] inline std::vector<std::filesystem::path> model_storage_paths(const std::filesystem::path& model_dir, std::string_view name) {
    if (model_dir.empty() || !detail::valid_model_stem(name)) return {};
    static constexpr std::array<std::string_view, 7> suffixes = {
        ".safetensors",
        ".safetensors.part",
        "-tokenizer.json",
        "-tokenizer.json.part",
        "-text.onnx",
        "-text.onnx.data",
        "-text.onnx.tmp",
    };
    std::vector<std::filesystem::path> paths;
    paths.reserve(suffixes.size());
    std::string fname;
    fname.reserve(name.size() + 24);
    for (std::string_view s : suffixes) {
        fname.assign(name);
        fname.append(s);
        paths.emplace_back(model_dir / detail::utf8_to_path(fname));
    }
    return paths;
}

/// Deletes all model-specific files for `name` inside `model_dir`. Returns false if any existing file fails to delete.
inline bool delete_downloaded_model(const std::filesystem::path& model_dir, std::string_view name, std::string* error = nullptr) {
    namespace fs = std::filesystem;
    if (model_dir.empty() || !detail::valid_model_stem(name)) {
        if (error) *error = "Invalid model name or directory";
        return false;
    }
    bool ok = true;
    std::string first_err;
    for (const fs::path& p : model_storage_paths(model_dir, name)) {
        std::error_code ec;
        if (!fs::exists(p, ec)) {
            if (ec && ok) {
                ok = false;
                first_err = ec.message();
            }
            continue;
        }
        if (!fs::remove(p, ec) && ec && ok) {
            ok = false;
            first_err = ec.message();
        }
    }
    if (!ok && error) *error = std::move(first_err);
    return ok;
}

/// Scans `edge_dir` on disk for Qdrant Edge collections and their disk usage (without opening shards for point counts).
[[nodiscard]] inline std::vector<DbCollectionInfo> scan_edge_collections_on_disk(const std::filesystem::path& edge_dir) {
    namespace fs = std::filesystem;
    std::vector<DbCollectionInfo> out;
    std::error_code ec;
    if (edge_dir.empty() || !fs::exists(edge_dir, ec)) return out;

    for (const auto& entry : fs::directory_iterator(edge_dir, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code dec;
        if (entry.is_symlink(dec) || !entry.is_directory(dec)) continue;
        const fs::path& dir = entry.path();
        if (!fs::is_regular_file(dir / "edge_config.json", dec) && !fs::is_directory(dir / "segments", dec)) continue;
        std::string name = detail::path_to_utf8(dir.filename());
        if (!detail::valid_model_stem(name)) continue;
        out.emplace_back(std::move(name), std::nullopt, detail::directory_bytes(dir));
    }
    std::ranges::sort(out, [](const DbCollectionInfo& a, const DbCollectionInfo& b) {
        return detail::ascii_iless(a.name, b.name);
    });
    return out;
}

} // namespace usyseek
