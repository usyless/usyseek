#include "check.hpp"

#include <usyseek/storage_info.hpp>

#include <chrono>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using usyseek::DbCollectionInfo;
using usyseek::ModelStorageInfo;
using usyseek::delete_downloaded_model;
using usyseek::scan_downloaded_models;
using usyseek::scan_edge_collections_on_disk;

namespace {

struct TempDir {
    fs::path path;
    explicit TempDir(const char* tag) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("usyseek_" + std::string{tag} + "_" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_bytes(const fs::path& p, size_t n) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    std::string data(n, 'x');
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

void scans_and_deletes_downloaded_models() {
    TempDir dir("models");

    // Full model
    write_bytes(dir.path / "PE-Core-L-14-336.safetensors", 100);
    write_bytes(dir.path / "PE-Core-L-14-336-tokenizer.json", 20);
    write_bytes(dir.path / "PE-Core-L-14-336-text.onnx", 40);

    // Partial + orphaned ONNX model
    write_bytes(dir.path / "alpha-model-text.onnx", 15);
    write_bytes(dir.path / "alpha-model.safetensors.part", 50);

    // Shared tokenizer & unrelated file must be ignored
    write_bytes(dir.path / "tokenizer.json", 99);
    write_bytes(dir.path / "notes.txt", 12);

    const auto models = scan_downloaded_models(dir.path);
    CHECK(models.size() == 2);
    CHECK(models[0] == (ModelStorageInfo{
                           .name = "alpha-model",
                           .weights = false,
                           .tokenizer = false,
                           .onnx = true,
                           .partial = true,
                           .bytes = 65,
                       }));
    CHECK(models[1] == (ModelStorageInfo{
                           .name = "PE-Core-L-14-336",
                           .weights = true,
                           .tokenizer = true,
                           .onnx = true,
                           .partial = false,
                           .bytes = 160,
                       }));

    std::string err;
    CHECK(!delete_downloaded_model(dir.path, "../escape", &err));
    CHECK(delete_downloaded_model(dir.path, "alpha-model", &err));
    CHECK(!fs::exists(dir.path / "alpha-model-text.onnx"));
    CHECK(!fs::exists(dir.path / "alpha-model.safetensors.part"));
    // Other model and shared tokenizer remain intact
    CHECK(fs::exists(dir.path / "PE-Core-L-14-336.safetensors"));
    CHECK(fs::exists(dir.path / "tokenizer.json"));
}

void scans_edge_collections_on_disk() {
    TempDir dir("edge");

    write_bytes(dir.path / "Model-B" / "edge_config.json", 12);
    write_bytes(dir.path / "Model-B" / "segments" / "seg0" / "data.bin", 88);
    write_bytes(dir.path / "alpha_db" / "segments" / "wal", 35);
    // Non-shard folder should be skipped
    write_bytes(dir.path / "unrelated" / "readme.txt", 200);

    const auto cols = scan_edge_collections_on_disk(dir.path);
    CHECK(cols.size() == 2);
    CHECK(cols[0] == (DbCollectionInfo{.name = "alpha_db", .count = std::nullopt, .bytes = 35}));
    CHECK(cols[1] == (DbCollectionInfo{.name = "Model-B", .count = std::nullopt, .bytes = 100}));
}

} // namespace

int main() {
    scans_and_deletes_downloaded_models();
    scans_edge_collections_on_disk();
    return usyseek::test::failures != 0;
}
