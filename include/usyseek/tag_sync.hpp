#pragma once

#include "db.hpp"
#include "tag_store.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace usyseek {

[[nodiscard]] inline std::vector<std::string> normalize_tag_list(std::vector<std::string> tags) {
    std::erase_if(tags, [](const std::string& t) { return t.empty(); });
    std::ranges::sort(tags);
    const auto dup = std::ranges::unique(tags);
    tags.erase(dup.begin(), dup.end());
    return tags;
}

[[nodiscard]] inline bool same_tag_set(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    return normalize_tag_list(a) == normalize_tag_list(b);
}

/// Imports any existing non-empty tags from `coll` into `store` once per collection.
inline bool backfill_collection(Db& db, TagStore& store, std::string_view coll, std::string* error = nullptr) {
    const auto done = store.is_collection_backfilled(coll, error);
    if (!done) return false;
    if (*done) return true;

    static constexpr std::array<std::string_view, 2> fields = {"p", "t"};
    std::vector<TaggedFileEntry> batch;
    std::optional<uint64_t> cursor;
    do {
        ScrollRequest req;
        req.limit = 4096;
        req.offset = cursor;
        req.fields = fields;
        auto page = db.scroll(coll, req, error);
        if (!page) return false;
        for (auto& pt : page->points) {
            if (pt.p.t.empty()) continue;
            batch.emplace_back(pt.id, std::move(pt.p.p), std::move(pt.p.t));
        }
        cursor = page->next;
    } while (cursor);

    if (!store.merge_batch(batch, error)) return false;
    return store.mark_collection_backfilled(coll, error);
}

/// Imports tags from every un-backfilled Qdrant Edge collection into `store`.
inline bool backfill_all_collections(Db& db, TagStore& store, std::string* error = nullptr) {
    const auto cols = db.list_collections(error);
    if (!cols) return false;
    for (const DbCollectionInfo& col : *cols) {
        if (!backfill_collection(db, store, col.name, error)) return false;
    }
    return true;
}

} // namespace usyseek
