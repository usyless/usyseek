#include "check.hpp"

#include <usyseek/tag_store.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using usyseek::TagStore;
using usyseek::TaggedFileEntry;

namespace {

struct TempDbDir {
    fs::path path;
    explicit TempDbDir(const char* tag) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("usyseek_" + std::string{tag} + "_" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempDbDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void roundtrips_tags_and_high_bit_ids() {
    TempDbDir dir("tagstore");
    const fs::path db_path = dir.path / "tags.db";

    TagStore store;
    std::string err;
    CHECK(store.open(db_path, &err));

    const uint64_t high_id = 0x8000000000001234ULL;
    const uint64_t low_id = 42ULL;

    CHECK(store.set_file_tags(high_id, "C:\\Media\\hero.png", {"fav", "art", "fav", ""}, &err));
    CHECK(store.set_file_tags(low_id, "D:\\Clips\\vid.mp4", {"clip"}, &err));

    auto all = store.load_all(&err);
    CHECK(all.has_value());
    CHECK(all->size() == 2);
    CHECK((*all)[high_id].path == "C:\\Media\\hero.png");
    CHECK((*all)[high_id].tags == (std::vector<std::string>{"art", "fav"}));
    CHECK((*all)[low_id].tags == (std::vector<std::string>{"clip"}));

    // Empty tag list deletes the file row cleanly
    CHECK(store.set_file_tags(low_id, "D:\\Clips\\vid.mp4", {}, &err));
    auto st = store.stats(&err);
    CHECK(st.has_value());
    CHECK(st->file_count == 1);
    CHECK(st->tag_count == 2);
    CHECK(st->bytes > 0);
}

void merges_backfill_and_tracks_collections() {
    TempDbDir dir("tagmerge");
    const fs::path db_path = dir.path / "tags.db";

    TagStore store;
    std::string err;
    CHECK(store.open(db_path, &err));

    CHECK(store.set_file_tags(10, "C:\\a.jpg", {"fav"}, &err));
    CHECK(store.merge_batch(
        {
            {.id = 10, .path = "C:\\a.jpg", .tags = {"fav", "portrait"}},
            {.id = 20, .path = "C:\\b.jpg", .tags = {"landscape"}},
            {.id = 30, .path = "C:\\c.jpg", .tags = {}},
        },
        &err));

    auto all = store.load_all(&err);
    CHECK(all.has_value());
    CHECK(all->size() == 2);
    CHECK((*all)[10].tags == (std::vector<std::string>{"fav", "portrait"}));
    CHECK((*all)[20].tags == (std::vector<std::string>{"landscape"}));

    auto bf = store.is_collection_backfilled("PE-Core-L-14-336", &err);
    CHECK(bf.has_value() && !*bf);
    CHECK(store.mark_collection_backfilled("PE-Core-L-14-336", &err));
    bf = store.is_collection_backfilled("PE-Core-L-14-336", &err);
    CHECK(bf.has_value() && *bf);
    CHECK(store.clear_collection_backfilled("PE-Core-L-14-336", &err));
    bf = store.is_collection_backfilled("PE-Core-L-14-336", &err);
    CHECK(bf.has_value() && !*bf);

    CHECK(store.remove_ids({10}, &err));
    all = store.load_all(&err);
    CHECK(all.has_value());
    CHECK(all->size() == 1);
    CHECK(all->contains(20));
}

void supports_concurrent_connections_in_wal_mode() {
    TempDbDir dir("tagwal");
    const fs::path db_path = dir.path / "tags.db";

    TagStore ui_store;
    TagStore idx_store;
    std::string err;
    CHECK(ui_store.open(db_path, &err));
    CHECK(idx_store.open(db_path, &err));

    CHECK(ui_store.set_file_tags(100, "C:\\x.png", {"hero"}, &err));
    auto list = idx_store.list_all(&err);
    CHECK(list.has_value());
    CHECK(list->size() == 1);
    CHECK((*list)[0].id == 100);
    CHECK((*list)[0].tags == (std::vector<std::string>{"hero"}));
}

} // namespace

int main() {
    roundtrips_tags_and_high_bit_ids();
    merges_backfill_and_tracks_collections();
    supports_concurrent_connections_in_wal_mode();
    return usyseek::test::failures != 0;
}
