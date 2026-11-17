#pragma once

#include <sqlite3.h>

#include <concepts>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace usyseek {

struct TaggedFileEntry {
    uint64_t id = 0;
    std::string path;
    std::vector<std::string> tags;

    bool operator==(const TaggedFileEntry&) const = default;
};

struct TagStoreStats {
    uint64_t file_count = 0;
    uint64_t tag_count = 0;
    uint64_t bytes = 0;

    bool operator==(const TagStoreStats&) const = default;
};

/// Multi-process SQLite (WAL) store for persistent path_id -> tags mappings across model switches.
class TagStore {
public:
    TagStore() = default;
    TagStore(const TagStore&) = delete;
    TagStore& operator=(const TagStore&) = delete;
    TagStore(TagStore&&) = delete;
    TagStore& operator=(TagStore&&) = delete;
    ~TagStore() { close(); }

    bool open(const std::filesystem::path& db_path, std::string* error = nullptr) {
        std::scoped_lock lk{mx_};
        close_locked();

        std::error_code ec;
        if (db_path.has_parent_path()) {
            std::filesystem::create_directories(db_path.parent_path(), ec);
        }

        const auto u8 = db_path.u8string();
        const std::string utf8{reinterpret_cast<const char*>(u8.data()), u8.size()};
        sqlite3* raw = nullptr;
        const int rc = sqlite3_open_v2(utf8.c_str(), &raw,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (rc != SQLITE_OK) {
            if (error) *error = raw ? sqlite3_errmsg(raw) : sqlite3_errstr(rc);
            if (raw) sqlite3_close(raw);
            return false;
        }
        db_ = raw;
        path_ = db_path;
        sqlite3_busy_timeout(db_, 5000);

        static constexpr const char* INIT_SQL = R"sql(
            PRAGMA journal_mode=WAL;
            PRAGMA synchronous=NORMAL;
            PRAGMA foreign_keys=ON;
            CREATE TABLE IF NOT EXISTS tagged_files (
                id   INTEGER PRIMARY KEY,
                path TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS file_tags (
                file_id INTEGER NOT NULL REFERENCES tagged_files(id) ON DELETE CASCADE,
                tag     TEXT NOT NULL,
                PRIMARY KEY (file_id, tag)
            );
            CREATE TABLE IF NOT EXISTS meta (
                key   TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );
        )sql";
        if (!exec_locked(INIT_SQL, error)) {
            close_locked();
            return false;
        }
        return true;
    }

    void close() noexcept {
        std::scoped_lock lk{mx_};
        close_locked();
    }

    [[nodiscard]] bool opened() const noexcept {
        std::scoped_lock lk{mx_};
        return db_ != nullptr;
    }

    /// Replaces the tag set for `id` (deleting the file row when `tags` has no non-empty entries).
    template <std::ranges::input_range R>
        requires std::convertible_to<std::ranges::range_reference_t<R>, std::string_view>
    bool set_file_tags(uint64_t id, std::string_view path_utf8, const R& tags, std::string* error = nullptr) {
        std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return false;

        std::vector<std::string_view> clean;
        if constexpr (std::ranges::sized_range<R>) clean.reserve(std::ranges::size(tags));
        for (const std::string_view t : tags) {
            if (!t.empty()) clean.emplace_back(t);
        }

        return with_tx_locked([&] {
            if (clean.empty()) {
                return delete_file_locked(id, error);
            }
            if (!upsert_file_locked(id, path_utf8, false, error)) return false;
            Stmt del;
            if (!prepare_locked("DELETE FROM file_tags WHERE file_id = ?1;", del, error)) return false;
            sqlite3_bind_int64(del.s, 1, static_cast<int64_t>(id));
            if (!step_done_locked(del.s, error)) return false;

            Stmt ins;
            if (!prepare_locked("INSERT OR IGNORE INTO file_tags(file_id, tag) VALUES(?1, ?2);", ins, error)) return false;
            for (std::string_view t : clean) {
                sqlite3_reset(ins.s);
                sqlite3_clear_bindings(ins.s);
                sqlite3_bind_int64(ins.s, 1, static_cast<int64_t>(id));
                sqlite3_bind_text(ins.s, 2, t.data(), static_cast<int>(t.size()), SQLITE_TRANSIENT);
                if (!step_done_locked(ins.s, error)) return false;
            }
            return true;
        }, error);
    }

    /// Unions `entries` into the store inside a single transaction (ignores entries with no tags).
    bool merge_batch(std::span<const TaggedFileEntry> entries, std::string* error = nullptr) {
        std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return false;
        if (entries.empty()) return true;

        return with_tx_locked([&] {
            Stmt up_file;
            if (!prepare_locked(
                    "INSERT INTO tagged_files(id, path) VALUES(?1, ?2) "
                    "ON CONFLICT(id) DO UPDATE SET path = CASE WHEN excluded.path != '' THEN excluded.path ELSE tagged_files.path END;",
                    up_file, error)) {
                return false;
            }
            Stmt ins_tag;
            if (!prepare_locked("INSERT OR IGNORE INTO file_tags(file_id, tag) VALUES(?1, ?2);", ins_tag, error)) {
                return false;
            }

            for (const TaggedFileEntry& e : entries) {
                bool any_tag = false;
                for (const std::string& t : e.tags) {
                    if (!t.empty()) {
                        any_tag = true;
                        break;
                    }
                }
                if (!any_tag) continue;

                sqlite3_reset(up_file.s);
                sqlite3_clear_bindings(up_file.s);
                sqlite3_bind_int64(up_file.s, 1, static_cast<int64_t>(e.id));
                sqlite3_bind_text(up_file.s, 2, e.path.data(), static_cast<int>(e.path.size()), SQLITE_TRANSIENT);
                if (!step_done_locked(up_file.s, error)) return false;

                for (const std::string& t : e.tags) {
                    if (t.empty()) continue;
                    sqlite3_reset(ins_tag.s);
                    sqlite3_clear_bindings(ins_tag.s);
                    sqlite3_bind_int64(ins_tag.s, 1, static_cast<int64_t>(e.id));
                    sqlite3_bind_text(ins_tag.s, 2, t.data(), static_cast<int>(t.size()), SQLITE_TRANSIENT);
                    if (!step_done_locked(ins_tag.s, error)) return false;
                }
            }
            return true;
        }, error);
    }

    bool remove_ids(std::span<const uint64_t> ids, std::string* error = nullptr) {
        std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return false;
        if (ids.empty()) return true;

        return with_tx_locked([&] {
            Stmt del;
            if (!prepare_locked("DELETE FROM tagged_files WHERE id = ?1;", del, error)) return false;
            for (uint64_t id : ids) {
                sqlite3_reset(del.s);
                sqlite3_clear_bindings(del.s);
                sqlite3_bind_int64(del.s, 1, static_cast<int64_t>(id));
                if (!step_done_locked(del.s, error)) return false;
            }
            return true;
        }, error);
    }

    [[nodiscard]] std::optional<bool> is_collection_backfilled(std::string_view coll, std::string* error = nullptr) {
        std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return std::nullopt;
        const std::string key = backfill_key(coll);
        Stmt st;
        if (!prepare_locked("SELECT value FROM meta WHERE key = ?1;", st, error)) return std::nullopt;
        sqlite3_bind_text(st.s, 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
        const int rc = sqlite3_step(st.s);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        if (error) *error = sqlite3_errmsg(db_);
        return std::nullopt;
    }

    bool mark_collection_backfilled(std::string_view coll, std::string* error = nullptr) {
        std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return false;
        const std::string key = backfill_key(coll);
        Stmt st;
        if (!prepare_locked("INSERT INTO meta(key, value) VALUES(?1, '1') ON CONFLICT(key) DO UPDATE SET value = '1';", st, error)) {
            return false;
        }
        sqlite3_bind_text(st.s, 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
        return step_done_locked(st.s, error);
    }

    bool clear_collection_backfilled(std::string_view coll, std::string* error = nullptr) {
        std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return false;
        const std::string key = backfill_key(coll);
        Stmt st;
        if (!prepare_locked("DELETE FROM meta WHERE key = ?1;", st, error)) return false;
        sqlite3_bind_text(st.s, 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
        return step_done_locked(st.s, error);
    }

    [[nodiscard]] std::optional<std::vector<TaggedFileEntry>> list_all(std::string* error = nullptr) {
        std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return std::nullopt;

        Stmt st;
        if (!prepare_locked(
                "SELECT f.id, f.path, t.tag "
                "FROM tagged_files f "
                "JOIN file_tags t ON t.file_id = f.id "
                "ORDER BY f.id, t.tag;",
                st, error)) {
            return std::nullopt;
        }

        std::vector<TaggedFileEntry> out;
        for (;;) {
            const int rc = sqlite3_step(st.s);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) {
                if (error) *error = sqlite3_errmsg(db_);
                return std::nullopt;
            }
            const auto id = static_cast<uint64_t>(sqlite3_column_int64(st.s, 0));
            const auto* p_txt = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 1));
            const int p_len = sqlite3_column_bytes(st.s, 1);
            const auto* t_txt = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 2));
            const int t_len = sqlite3_column_bytes(st.s, 2);

            if (out.empty() || out.back().id != id) {
                auto& e = out.emplace_back();
                e.id = id;
                if (p_txt && p_len > 0) e.path.assign(p_txt, static_cast<size_t>(p_len));
            }
            if (t_txt && t_len > 0) {
                out.back().tags.emplace_back(t_txt, static_cast<size_t>(t_len));
            }
        }
        return out;
    }

    [[nodiscard]] std::optional<std::unordered_map<uint64_t, TaggedFileEntry>> load_all(std::string* error = nullptr) {
        auto rows = list_all(error);
        if (!rows) return std::nullopt;
        std::unordered_map<uint64_t, TaggedFileEntry> map;
        map.reserve(rows->size());
        for (auto& e : *rows) {
            const uint64_t id = e.id;
            map.emplace(id, std::move(e));
        }
        return map;
    }

    [[nodiscard]] std::optional<TagStoreStats> stats(std::string* error = nullptr) {
        std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return std::nullopt;

        TagStoreStats out;
        auto count_query = [&](const char* sql, uint64_t& dst) -> bool {
            Stmt st;
            if (!prepare_locked(sql, st, error)) return false;
            const int rc = sqlite3_step(st.s);
            if (rc != SQLITE_ROW) {
                if (error) *error = sqlite3_errmsg(db_);
                return false;
            }
            dst = static_cast<uint64_t>(sqlite3_column_int64(st.s, 0));
            return true;
        };
        if (!count_query("SELECT COUNT(DISTINCT file_id) FROM file_tags;", out.file_count)) return std::nullopt;
        if (!count_query("SELECT COUNT(*) FROM file_tags;", out.tag_count)) return std::nullopt;

        std::error_code ec;
        auto add_size = [&](const std::filesystem::path& p) {
            if (std::filesystem::is_regular_file(p, ec)) {
                const uint64_t sz = std::filesystem::file_size(p, ec);
                if (!ec) out.bytes += sz;
            }
        };
        add_size(path_);
        std::filesystem::path wal_path = path_;
        wal_path += L"-wal";
        add_size(wal_path);
        return out;
    }

private:
    struct Stmt {
        sqlite3_stmt* s = nullptr;
        Stmt() = default;
        Stmt(const Stmt&) = delete;
        Stmt& operator=(const Stmt&) = delete;
        ~Stmt() {
            if (s) sqlite3_finalize(s);
        }
    };

    static std::string backfill_key(std::string_view coll) {
        std::string key{"backfilled:"};
        key.append(coll);
        return key;
    }

    void close_locked() noexcept {
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        path_.clear();
    }

    bool require_open_locked(std::string* error) const {
        if (db_) return true;
        if (error) *error = "tag database is closed";
        return false;
    }

    bool exec_locked(const char* sql, std::string* error) {
        char* msg = nullptr;
        const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &msg);
        if (rc == SQLITE_OK) return true;
        if (error) *error = msg ? msg : sqlite3_errmsg(db_);
        if (msg) sqlite3_free(msg);
        return false;
    }

    bool prepare_locked(const char* sql, Stmt& out, std::string* error) {
        if (sqlite3_prepare_v2(db_, sql, -1, &out.s, nullptr) == SQLITE_OK) return true;
        if (error) *error = sqlite3_errmsg(db_);
        return false;
    }

    bool step_done_locked(sqlite3_stmt* s, std::string* error) {
        const int rc = sqlite3_step(s);
        if (rc == SQLITE_DONE) return true;
        if (error) *error = sqlite3_errmsg(db_);
        return false;
    }

    bool delete_file_locked(uint64_t id, std::string* error) {
        Stmt del;
        if (!prepare_locked("DELETE FROM tagged_files WHERE id = ?1;", del, error)) return false;
        sqlite3_bind_int64(del.s, 1, static_cast<int64_t>(id));
        return step_done_locked(del.s, error);
    }

    bool upsert_file_locked(uint64_t id, std::string_view path_utf8, bool preserve_existing_path, std::string* error) {
        const char* sql = preserve_existing_path
            ? "INSERT INTO tagged_files(id, path) VALUES(?1, ?2) "
              "ON CONFLICT(id) DO UPDATE SET path = CASE WHEN excluded.path != '' THEN excluded.path ELSE tagged_files.path END;"
            : "INSERT INTO tagged_files(id, path) VALUES(?1, ?2) "
              "ON CONFLICT(id) DO UPDATE SET path = excluded.path;";
        Stmt st;
        if (!prepare_locked(sql, st, error)) return false;
        sqlite3_bind_int64(st.s, 1, static_cast<int64_t>(id));
        sqlite3_bind_text(st.s, 2, path_utf8.data(), static_cast<int>(path_utf8.size()), SQLITE_TRANSIENT);
        return step_done_locked(st.s, error);
    }

    template <typename Fn>
    bool with_tx_locked(Fn&& fn, std::string* error) {
        if (!exec_locked("BEGIN IMMEDIATE;", error)) return false;
        if (!fn()) {
            std::string rb_err;
            (void)exec_locked("ROLLBACK;", &rb_err);
            return false;
        }
        if (!exec_locked("COMMIT;", error)) {
            std::string rb_err;
            (void)exec_locked("ROLLBACK;", &rb_err);
            return false;
        }
        return true;
    }

    mutable std::mutex mx_;
    sqlite3* db_ = nullptr;
    std::filesystem::path path_;
};

} // namespace usyseek
