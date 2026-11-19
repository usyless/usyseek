#pragma once

#include "rust_bridge.hpp"
#include "settings.hpp"
#include "storage_info.hpp"

#include <glaze/glaze.hpp>
#include <wil/resource.h>

#include <charconv>
#include <mutex>
#include <shared_mutex>
#include <span>

extern "C" {
struct UsyQdrant;
struct UsyTokenizer;

USY_RUST_API UsyQdrant* usy_qd_open(UsyStr dir, UsyBuf* out);
USY_RUST_API int32_t usy_qd_close_all(UsyQdrant* q, UsyBuf* out);
USY_RUST_API void usy_qd_free(UsyQdrant* q);
USY_RUST_API int32_t usy_qd_flush(UsyQdrant* q, UsyStr name, UsyBuf* out);
USY_RUST_API int32_t usy_qd_optimize(UsyQdrant* q, UsyStr name, UsyBuf* out);
USY_RUST_API int32_t usy_qd_ensure_collection(UsyQdrant* q, UsyStr name, uint64_t dim, UsyBuf* out);
USY_RUST_API int32_t usy_qd_check_collection(UsyQdrant* q, UsyStr name, UsyBuf* out);
USY_RUST_API int32_t usy_qd_upsert(UsyQdrant* q, UsyStr name, const uint64_t* ids, size_t count, const float* vectors, size_t dim, UsyStr payloads, UsyBuf* out);
USY_RUST_API int32_t usy_qd_query(UsyQdrant* q, UsyStr name, const float* vector, size_t dim, UsyStr request, UsyBuf* out);
USY_RUST_API int32_t usy_qd_scroll(UsyQdrant* q, UsyStr name, UsyStr request, UsyBuf* out);
USY_RUST_API int32_t usy_qd_retrieve(UsyQdrant* q, UsyStr name, const uint64_t* ids, size_t count, UsyStr request, UsyBuf* out);
USY_RUST_API int32_t usy_qd_set_payload(UsyQdrant* q, UsyStr name, const uint64_t* ids, size_t count, UsyStr payload, UsyBuf* out);
USY_RUST_API int32_t usy_qd_delete(UsyQdrant* q, UsyStr name, const uint64_t* ids, size_t count, UsyBuf* out);
USY_RUST_API int32_t usy_qd_drop_collection(UsyQdrant* q, UsyStr name, UsyBuf* out);
USY_RUST_API int32_t usy_qd_count(UsyQdrant* q, UsyStr name, UsyBuf* out);
USY_RUST_API int32_t usy_qd_list_collections(UsyQdrant* q, UsyBuf* out);
USY_RUST_API int32_t usy_qd_facet_tags(UsyQdrant* q, UsyStr name, uint64_t limit, UsyBuf* out);

USY_RUST_API UsyTokenizer* usy_tok_load(UsyStr path, size_t context_length, UsyBuf* out);
USY_RUST_API void usy_tok_free(UsyTokenizer* t);
USY_RUST_API size_t usy_tok_encode(UsyTokenizer* t, UsyStr text, int64_t* ids);
}

namespace usyseek {

struct MediaPayload {
    std::string p;                 // full path
    std::string n;                 // file name
    int32_t k = 0;                 // 0 image, 1 video
    int64_t m = 0;                 // modified (unix seconds)
    uint64_t s = 0;                // size in bytes
    uint32_t w = 0;
    uint32_t h = 0;
    uint64_t d = 0;                // video duration (ms)
    std::vector<std::string> t;    // tags
    std::vector<std::string> a;    // ancestor directories (normalised)
};

struct DbPoint {
    uint64_t id = 0;
    float s = 0.0f;
    MediaPayload p;
};

struct DbPoints {
    std::vector<DbPoint> points;
    std::optional<uint64_t> next;
};

struct TagCount {
    std::string v;
    uint64_t c = 0;
};

struct DbFilter {
    std::optional<int32_t> kind;
    std::vector<std::string> tags_all;
    std::vector<std::string> tags_none;
    std::vector<std::string> dirs;
    std::vector<std::string> names;
};

struct QueryRequest {
    uint64_t limit = 100;
    uint64_t offset = 0;
    std::optional<float> threshold;
    std::optional<uint64_t> id;
    std::optional<DbFilter> filter;
    std::span<const std::string_view> fields;
};

struct ScrollRequest {
    uint64_t limit = 100;
    std::optional<uint64_t> offset;
    bool order_mtime = false;
    std::optional<DbFilter> filter;
    std::span<const std::string_view> fields;
};

struct FieldsRequest {
    std::span<const std::string_view> fields;
};

inline constexpr auto JSON_SKIP_NULL = glz::opts{.skip_null_members = true};

/// True when a Qdrant Edge error indicates file-lock / sharing contention rather than on-disk corruption.
[[nodiscard]] inline bool is_db_lock_error(std::string_view err) noexcept {
    if (err.empty()) return false;
    const auto contains_ci = [&](std::string_view needle) noexcept {
        if (needle.size() > err.size()) return false;
        const auto eq = [](char a, char b) noexcept {
            const auto la = (a >= 'A' && a <= 'Z') ? static_cast<char>(a + ('a' - 'A')) : a;
            const auto lb = (b >= 'A' && b <= 'Z') ? static_cast<char>(b + ('a' - 'A')) : b;
            return la == lb;
        };
        return !std::ranges::search(err, needle, eq).empty();
    };
    return contains_ci("being used by another process") ||
           contains_ci("locked a portion of the file") ||
           contains_ci("sharing violation") ||
           contains_ci("lock violation") ||
           contains_ci("access is denied") ||
           contains_ci("permission denied") ||
           contains_ci("resource temporarily unavailable") ||
           contains_ci("wouldblock") ||
           contains_ci("os error 32") ||
           contains_ci("os error 33") ||
           contains_ci("os error 5") ||
           contains_ci("lock") ||
           contains_ci("busy");
}

/// Thread-safe handle to an embedded Qdrant Edge storage root.
class Db {
public:
    Db() = default;
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    Db(Db&&) = delete;
    Db& operator=(Db&&) = delete;
    ~Db() {
        const std::unique_lock lk{mx_};
        if (q_) {
            usy_qd_free(q_);
            q_ = nullptr;
        }
    }

    bool open(const fs::path& dir, std::string* error = nullptr) {
        const std::unique_lock lk{mx_};
        if (q_) {
            usy_qd_free(q_);
            q_ = nullptr;
        }
        const std::string utf8 = to_utf8(dir.wstring());
        RustBuf out;
        q_ = usy_qd_open(ustr(utf8), &out.b);
        if (!q_ && error) *error = out.str();
        return q_ != nullptr;
    }

    /// Flushes and closes all open edge shards, releasing file locks.
    bool close(std::string* error = nullptr) {
        const std::unique_lock lk{mx_};
        if (!q_) return true;
        RustBuf out;
        const bool ok = usy_qd_close_all(q_, &out.b) == 0;
        if (!ok && error) *error = out.str();
        usy_qd_free(q_);
        q_ = nullptr;
        return ok;
    }

    [[nodiscard]] bool opened() const noexcept {
        const std::shared_lock lk{mx_};
        return q_ != nullptr;
    }

    bool flush(std::string_view coll = {}, std::string* error = nullptr) {
        const std::shared_lock lk{mx_};
        if (!q_) return true;
        RustBuf out;
        const bool ok = usy_qd_flush(q_, ustr(coll), &out.b) == 0;
        if (!ok && error) *error = out.str();
        return ok;
    }

    bool optimize(std::string_view coll, std::string* error = nullptr) {
        const std::shared_lock lk{mx_};
        if (!q_) return false;
        RustBuf out;
        const bool ok = usy_qd_optimize(q_, ustr(coll), &out.b) == 0;
        if (!ok && error) *error = out.str();
        return ok;
    }

    enum class CollectionStatus : uint8_t {
        Ready,     // collection exists or was created
        NeedsDims, // collection does not exist and dim == 0
        Error,
    };

    /// Checks or creates `name`. Pass `dim == 0` to check existence without requiring dimensions.
    CollectionStatus ensure_collection_status(std::string_view name, uint64_t dim = 0, std::string* error = nullptr) {
        const std::shared_lock lk{mx_};
        if (!q_) {
            if (error) *error = "database is closed";
            return CollectionStatus::Error;
        }
        RustBuf out;
        const int32_t rc = usy_qd_ensure_collection(q_, ustr(name), dim, &out.b);
        if (rc == 0) return CollectionStatus::Ready;
        if (error) *error = out.str();
        return rc == 2 ? CollectionStatus::NeedsDims : CollectionStatus::Error;
    }

    bool ensure_collection(std::string_view name, uint64_t dim, std::string* error = nullptr) {
        return ensure_collection_status(name, dim, error) == CollectionStatus::Ready;
    }

    bool upsert(std::string_view coll, std::span<const uint64_t> ids, std::span<const float> vectors, size_t dim,
                std::span<const MediaPayload> payloads, std::string* error = nullptr) {
        const std::string_view json = write_json_tls<JSON_SKIP_NULL>(payloads);
        if (json.empty()) return false;
        const std::shared_lock lk{mx_};
        if (!q_) {
            if (error) *error = "database is closed";
            return false;
        }
        RustBuf out;
        const bool ok = usy_qd_upsert(q_, ustr(coll), ids.data(), ids.size(), vectors.data(), dim, ustr(json), &out.b) == 0;
        if (!ok && error) *error = out.str();
        return ok;
    }

    std::optional<DbPoints> query(std::string_view coll, std::span<const float> vector, const QueryRequest& req, std::string* error = nullptr) {
        const std::string_view json = write_json_tls<JSON_SKIP_NULL>(req);
        if (json.empty()) {
            if (error) *error = "Failed to serialize query request";
            return std::nullopt;
        }
        const std::shared_lock lk{mx_};
        if (!q_) {
            if (error) *error = "database is closed";
            return std::nullopt;
        }
        RustBuf out;
        if (usy_qd_query(q_, ustr(coll), vector.empty() ? nullptr : vector.data(), vector.size(), ustr(json), &out.b) != 0) {
            if (error) *error = out.str();
            return std::nullopt;
        }
        return parse_points(out.view(), error);
    }

    std::optional<DbPoints> scroll(std::string_view coll, const ScrollRequest& req, std::string* error = nullptr) {
        const std::string_view json = write_json_tls<JSON_SKIP_NULL>(req);
        if (json.empty()) {
            if (error) *error = "Failed to serialize scroll request";
            return std::nullopt;
        }
        const std::shared_lock lk{mx_};
        if (!q_) {
            if (error) *error = "database is closed";
            return std::nullopt;
        }
        RustBuf out;
        if (usy_qd_scroll(q_, ustr(coll), ustr(json), &out.b) != 0) {
            if (error) *error = out.str();
            return std::nullopt;
        }
        return parse_points(out.view(), error);
    }

    std::optional<DbPoints> retrieve(std::string_view coll, std::span<const uint64_t> ids, std::span<const std::string_view> fields, std::string* error = nullptr) {
        const std::string_view json = write_json_tls<JSON_SKIP_NULL>(FieldsRequest{fields});
        if (json.empty()) {
            if (error) *error = "Failed to serialize retrieve request";
            return std::nullopt;
        }
        const std::shared_lock lk{mx_};
        if (!q_) {
            if (error) *error = "database is closed";
            return std::nullopt;
        }
        RustBuf out;
        if (usy_qd_retrieve(q_, ustr(coll), ids.data(), ids.size(), ustr(json), &out.b) != 0) {
            if (error) *error = out.str();
            return std::nullopt;
        }
        return parse_points(out.view(), error);
    }

    bool set_tags(std::string_view coll, std::span<const uint64_t> ids, std::span<const std::string> tags, std::string* error = nullptr) {
        struct TagsOnly {
            std::span<const std::string> t;
        };
        const std::string_view json = write_json_tls<glz::opts{}>(TagsOnly{tags});
        if (json.empty()) {
            if (error) *error = "Failed to serialize tags payload";
            return false;
        }
        const std::shared_lock lk{mx_};
        if (!q_) {
            if (error) *error = "database is closed";
            return false;
        }
        RustBuf out;
        const bool ok = usy_qd_set_payload(q_, ustr(coll), ids.data(), ids.size(), ustr(json), &out.b) == 0;
        if (!ok && error) *error = out.str();
        return ok;
    }

    bool remove(std::string_view coll, std::span<const uint64_t> ids, std::string* error = nullptr) {
        if (ids.empty()) return true;
        const std::shared_lock lk{mx_};
        if (!q_) {
            if (error) *error = "database is closed";
            return false;
        }
        RustBuf out;
        const bool ok = usy_qd_delete(q_, ustr(coll), ids.data(), ids.size(), &out.b) == 0;
        if (!ok && error) *error = out.str();
        return ok;
    }

    bool drop_collection(std::string_view coll, std::string* error = nullptr) {
        const std::unique_lock lk{mx_};
        if (!q_) {
            if (error) *error = "database is closed";
            return false;
        }
        RustBuf out;
        const bool ok = usy_qd_drop_collection(q_, ustr(coll), &out.b) == 0;
        if (!ok && error) *error = out.str();
        return ok;
    }

    /// True when `coll` is absent on disk or loads cleanly. False with `error` when loading an on-disk shard fails.
    bool check_collection(std::string_view coll, std::string* error = nullptr) {
        const std::shared_lock lk{mx_};
        if (!q_) {
            if (error) *error = "database is closed";
            return false;
        }
        RustBuf out;
        const bool ok = usy_qd_check_collection(q_, ustr(coll), &out.b) == 0;
        if (!ok && error) *error = out.str();
        return ok;
    }

    std::optional<uint64_t> count(std::string_view coll) {
        const std::shared_lock lk{mx_};
        if (!q_) return std::nullopt;
        RustBuf out;
        if (usy_qd_count(q_, ustr(coll), &out.b) != 0) return std::nullopt;
        const std::string_view sv = out.view();
        uint64_t val = 0;
        std::from_chars(sv.data(), sv.data() + sv.size(), val);
        return val;
    }

    std::optional<std::vector<DbCollectionInfo>> list_collections(std::string* error = nullptr) {
        const std::shared_lock lk{mx_};
        if (!q_) {
            if (error) *error = "database is closed";
            return std::nullopt;
        }
        RustBuf out;
        if (usy_qd_list_collections(q_, &out.b) != 0) {
            if (error) *error = out.str();
            return std::nullopt;
        }
        std::vector<DbCollectionInfo> list;
        if (const auto err = glz::read<JSON_READ>(list, out.view(), read_ctx_tls())) {
            if (error) *error = ulp::str::concat_strings("Failed to parse collections list: ", glz::format_error(err, out.view()));
            return std::nullopt;
        }
        return list;
    }

    std::vector<TagCount> tags(std::string_view coll, uint64_t limit = 2000) {
        const std::shared_lock lk{mx_};
        std::vector<TagCount> v;
        if (!q_) return v;
        RustBuf out;
        if (usy_qd_facet_tags(q_, ustr(coll), limit, &out.b) == 0) {
            (void)glz::read<JSON_READ>(v, out.view(), read_ctx_tls());
        }
        return v;
    }

private:
    static glz::context& read_ctx_tls() noexcept {
        thread_local glz::context ctx{};
        return ctx;
    }

    template <auto Opts, typename T>
    static std::string_view write_json_tls(const T& value) {
        thread_local std::string buf;
        thread_local glz::context ctx{};
        buf.clear();
        if (glz::write<Opts>(value, buf, ctx)) return {};
        return buf;
    }

    static std::optional<DbPoints> parse_points(std::string_view json, std::string* error = nullptr) {
        DbPoints pts;
        if (const auto err = glz::read<JSON_READ>(pts, json, read_ctx_tls())) {
            if (error) *error = ulp::str::concat_strings("Failed to parse database response: ", glz::format_error(err, json));
            return std::nullopt;
        }
        return pts;
    }

    mutable std::shared_mutex mx_;
    UsyQdrant* q_ = nullptr;
};

/// Job object that kills every assigned process when the last handle closes, while permitting explicit breakaway.
inline wil::unique_handle make_kill_on_close_job() {
    wil::unique_handle job{CreateJobObjectW(nullptr, nullptr)};
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &info, sizeof(info));
    }
    return job;
}

} // namespace usyseek
