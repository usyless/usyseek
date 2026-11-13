#pragma once

#include "rust_bridge.hpp"
#include "settings.hpp"

#include <glaze/glaze.hpp>
#include <wil/resource.h>

#include <thread>

extern "C" {
struct UsyQdrant;
struct UsyTokenizer;

UsyQdrant* usy_qd_connect(UsyStr url, UsyBuf* out);
void usy_qd_free(UsyQdrant* q);
int32_t usy_qd_health(UsyQdrant* q, UsyBuf* out);
int32_t usy_qd_ensure_collection(UsyQdrant* q, UsyStr name, uint64_t dim, UsyBuf* out);
int32_t usy_qd_upsert(UsyQdrant* q, UsyStr name, const uint64_t* ids, size_t count, const float* vectors, size_t dim, UsyStr payloads, UsyBuf* out);
int32_t usy_qd_query(UsyQdrant* q, UsyStr name, const float* vector, size_t dim, UsyStr request, UsyBuf* out);
int32_t usy_qd_scroll(UsyQdrant* q, UsyStr name, UsyStr request, UsyBuf* out);
int32_t usy_qd_retrieve(UsyQdrant* q, UsyStr name, const uint64_t* ids, size_t count, UsyStr request, UsyBuf* out);
int32_t usy_qd_set_payload(UsyQdrant* q, UsyStr name, const uint64_t* ids, size_t count, UsyStr payload, UsyBuf* out);
int32_t usy_qd_delete(UsyQdrant* q, UsyStr name, const uint64_t* ids, size_t count, UsyBuf* out);
int32_t usy_qd_drop_collection(UsyQdrant* q, UsyStr name, UsyBuf* out);
int32_t usy_qd_count(UsyQdrant* q, UsyStr name, UsyBuf* out);
int32_t usy_qd_facet_tags(UsyQdrant* q, UsyStr name, uint64_t limit, UsyBuf* out);

UsyTokenizer* usy_tok_load(UsyStr path, size_t context_length, UsyBuf* out);
void usy_tok_free(UsyTokenizer* t);
size_t usy_tok_encode(UsyTokenizer* t, UsyStr text, int64_t* ids);
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
    std::vector<std::string> fields;
};

struct ScrollRequest {
    uint64_t limit = 100;
    std::optional<uint64_t> offset;
    bool order_mtime = false;
    std::optional<DbFilter> filter;
    std::vector<std::string> fields;
};

struct FieldsRequest {
    std::vector<std::string> fields;
};

inline constexpr auto JSON_SKIP_NULL = glz::opts{.skip_null_members = true};

/// Thread-safe (the Rust client multiplexes over one gRPC channel).
class Db {
public:
    Db() = default;
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    Db(Db&&) = delete;
    Db& operator=(Db&&) = delete;
    ~Db() {
        if (q_) usy_qd_free(q_);
    }

    bool connect(uint16_t grpc_port, std::string* error = nullptr) {
        const std::string url = "http://127.0.0.1:" + std::to_string(grpc_port);
        RustBuf out;
        q_ = usy_qd_connect(ustr(url), &out.b);
        if (!q_ && error) *error = out.str();
        return q_ != nullptr;
    }

    [[nodiscard]] bool connected() const noexcept { return q_ != nullptr; }

    /// Polls the health endpoint until the server answers or the timeout elapses.
    bool wait_ready(std::chrono::milliseconds timeout, HANDLE abort_event = nullptr) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            RustBuf out;
            if (usy_qd_health(q_, &out.b) == 0) return true;
            if (abort_event && WaitForSingleObject(abort_event, 0) == WAIT_OBJECT_0) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    enum class CollectionStatus : uint8_t {
        Ready,     // collection exists or was created
        NeedsDims, // collection does not exist and dim == 0
        Error,
    };

    /// Checks or creates `name`. Pass `dim == 0` to check existence without requiring dimensions.
    CollectionStatus ensure_collection_status(std::string_view name, uint64_t dim = 0, std::string* error = nullptr) {
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
        auto json = glz::write<JSON_SKIP_NULL>(payloads);
        if (!json) return false;
        RustBuf out;
        const bool ok = usy_qd_upsert(q_, ustr(coll), ids.data(), ids.size(), vectors.data(), dim, ustr(*json), &out.b) == 0;
        if (!ok && error) *error = out.str();
        return ok;
    }

    std::optional<DbPoints> query(std::string_view coll, std::span<const float> vector, const QueryRequest& req, std::string* error = nullptr) {
        auto json = glz::write<JSON_SKIP_NULL>(req);
        if (!json) {
            if (error) *error = "Failed to serialize query request";
            return std::nullopt;
        }
        RustBuf out;
        if (usy_qd_query(q_, ustr(coll), vector.empty() ? nullptr : vector.data(), vector.size(), ustr(*json), &out.b) != 0) {
            if (error) *error = out.str();
            return std::nullopt;
        }
        return parse_points(out.view(), error);
    }

    std::optional<DbPoints> scroll(std::string_view coll, const ScrollRequest& req, std::string* error = nullptr) {
        auto json = glz::write<JSON_SKIP_NULL>(req);
        if (!json) {
            if (error) *error = "Failed to serialize scroll request";
            return std::nullopt;
        }
        RustBuf out;
        if (usy_qd_scroll(q_, ustr(coll), ustr(*json), &out.b) != 0) {
            if (error) *error = out.str();
            return std::nullopt;
        }
        return parse_points(out.view(), error);
    }

    std::optional<DbPoints> retrieve(std::string_view coll, std::span<const uint64_t> ids, const std::vector<std::string>& fields, std::string* error = nullptr) {
        auto json = glz::write<JSON_SKIP_NULL>(FieldsRequest{fields});
        if (!json) {
            if (error) *error = "Failed to serialize retrieve request";
            return std::nullopt;
        }
        RustBuf out;
        if (usy_qd_retrieve(q_, ustr(coll), ids.data(), ids.size(), ustr(*json), &out.b) != 0) {
            if (error) *error = out.str();
            return std::nullopt;
        }
        return parse_points(out.view(), error);
    }

    bool set_tags(std::string_view coll, std::span<const uint64_t> ids, const std::vector<std::string>& tags, std::string* error = nullptr) {
        struct TagsOnly {
            std::vector<std::string> t;
        };
        auto json = glz::write_json(TagsOnly{tags});
        if (!json) {
            if (error) *error = "Failed to serialize tags payload";
            return false;
        }
        RustBuf out;
        const bool ok = usy_qd_set_payload(q_, ustr(coll), ids.data(), ids.size(), ustr(*json), &out.b) == 0;
        if (!ok && error) *error = out.str();
        return ok;
    }

    bool remove(std::string_view coll, std::span<const uint64_t> ids, std::string* error = nullptr) {
        if (ids.empty()) return true;
        RustBuf out;
        const bool ok = usy_qd_delete(q_, ustr(coll), ids.data(), ids.size(), &out.b) == 0;
        if (!ok && error) *error = out.str();
        return ok;
    }

    bool drop_collection(std::string_view coll, std::string* error = nullptr) {
        RustBuf out;
        const bool ok = usy_qd_drop_collection(q_, ustr(coll), &out.b) == 0;
        if (!ok && error) *error = out.str();
        return ok;
    }

    std::optional<uint64_t> count(std::string_view coll) {
        RustBuf out;
        if (usy_qd_count(q_, ustr(coll), &out.b) != 0) return std::nullopt;
        return std::strtoull(out.str().c_str(), nullptr, 10);
    }

    std::vector<TagCount> tags(std::string_view coll, uint64_t limit = 2000) {
        RustBuf out;
        std::vector<TagCount> v;
        if (usy_qd_facet_tags(q_, ustr(coll), limit, &out.b) == 0) {
            (void)glz::read<JSON_READ>(v, out.view());
        }
        return v;
    }

private:
    static std::optional<DbPoints> parse_points(std::string_view json, std::string* error = nullptr) {
        DbPoints pts;
        if (const auto err = glz::read<JSON_READ>(pts, json)) {
            if (error) *error = "Failed to parse database response: " + glz::format_error(err, json);
            return std::nullopt;
        }
        return pts;
    }

    UsyQdrant* q_ = nullptr;
};

// ---------------------------------------------------------------------------------------------
// qdrant.exe lifecycle
// ---------------------------------------------------------------------------------------------

inline fs::path qdrant_exe() { return exe_dir() / L"qdrant" / L"qdrant.exe"; }

/// Starts qdrant.exe bound to localhost. The process is put in `job` (kill-on-close) when given.
inline wil::unique_process_information start_qdrant(const Settings& s, HANDLE job) {
    wil::unique_process_information pi;
    const fs::path exe = qdrant_exe();
    const fs::path storage = data_dir() / L"qdrant";
    std::error_code ec;
    fs::create_directories(storage / L"storage", ec);

    std::wstring env;
    auto add_env = [&env](std::wstring_view kv) {
        env += kv;
        env.push_back(L'\0');
    };
    // Inherit the current environment, then override qdrant specifics
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            if (_wcsnicmp(p, L"QDRANT__", 8) != 0) add_env(p);
        }
        FreeEnvironmentStringsW(block);
    }
    add_env(L"QDRANT__TELEMETRY_DISABLED=true");
    add_env(L"QDRANT__STORAGE__STORAGE_PATH=" + (storage / L"storage").wstring());
    add_env(L"QDRANT__STORAGE__SNAPSHOTS_PATH=" + (storage / L"snapshots").wstring());
    add_env(L"QDRANT__SERVICE__HOST=127.0.0.1");
    add_env(L"QDRANT__SERVICE__HTTP_PORT=" + std::to_wstring(s.qdrant_http_port));
    add_env(L"QDRANT__SERVICE__GRPC_PORT=" + std::to_wstring(s.qdrant_grpc_port));
    add_env(L"QDRANT__SERVICE__STATIC_CONTENT_DIR=" + (exe.parent_path() / L"static").wstring());
    env.push_back(L'\0');

    std::wstring cmd = L"\"" + exe.wstring() + L"\" --disable-telemetry --config-path \"" + (exe_dir() / L"config" / L"qdrant.yaml").wstring() + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);

    const fs::path work_dir = exe.parent_path();
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                        env.data(), work_dir.c_str(), &si, &pi)) {
        return {};
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    return pi;
}

/// Job object that kills every assigned process when the last handle closes.
inline wil::unique_handle make_kill_on_close_job() {
    wil::unique_handle job{CreateJobObjectW(nullptr, nullptr)};
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &info, sizeof(info));
    }
    return job;
}

} // namespace usyseek
