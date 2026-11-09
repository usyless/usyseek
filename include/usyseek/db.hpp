#pragma once

#include "settings.hpp"

#include <glaze/glaze.hpp>
#include <wil/resource.h>

#include <mutex>
#include <thread>

extern "C" {
struct UsyStr {
    const char* ptr;
    size_t len;
};
struct UsyBuf {
    char* ptr;
    size_t len;
    size_t cap;
};
struct UsyQdrant;
struct UsyTokenizer;

void usy_buf_free(UsyBuf b);
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
int32_t usy_qd_count(UsyQdrant* q, UsyStr name, UsyBuf* out);
int32_t usy_qd_facet_tags(UsyQdrant* q, UsyStr name, uint64_t limit, UsyBuf* out);

UsyTokenizer* usy_tok_load(UsyStr path, size_t context_length, UsyBuf* out);
void usy_tok_free(UsyTokenizer* t);
size_t usy_tok_encode(UsyTokenizer* t, UsyStr text, int64_t* ids);
}

namespace usyseek {

inline UsyStr ustr(std::string_view s) noexcept { return UsyStr{s.data(), s.size()}; }

/// RAII owner of a Rust-allocated buffer.
struct RustBuf {
    UsyBuf b{nullptr, 0, 0};
    RustBuf() = default;
    RustBuf(const RustBuf&) = delete;
    RustBuf& operator=(const RustBuf&) = delete;
    ~RustBuf() {
        if (b.ptr) usy_buf_free(b);
    }
    [[nodiscard]] std::string_view view() const noexcept { return {b.ptr ? b.ptr : "", b.len}; }
    [[nodiscard]] std::string str() const { return std::string{view()}; }
};

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

    bool ensure_collection(std::string_view name, uint64_t dim, std::string* error = nullptr) {
        RustBuf out;
        const bool ok = usy_qd_ensure_collection(q_, ustr(name), dim, &out.b) == 0;
        if (!ok && error) *error = out.str();
        return ok;
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
        if (!json) return std::nullopt;
        RustBuf out;
        if (usy_qd_query(q_, ustr(coll), vector.empty() ? nullptr : vector.data(), vector.size(), ustr(*json), &out.b) != 0) {
            if (error) *error = out.str();
            return std::nullopt;
        }
        return parse_points(out.view());
    }

    std::optional<DbPoints> scroll(std::string_view coll, const ScrollRequest& req, std::string* error = nullptr) {
        auto json = glz::write<JSON_SKIP_NULL>(req);
        if (!json) return std::nullopt;
        RustBuf out;
        if (usy_qd_scroll(q_, ustr(coll), ustr(*json), &out.b) != 0) {
            if (error) *error = out.str();
            return std::nullopt;
        }
        return parse_points(out.view());
    }

    std::optional<DbPoints> retrieve(std::string_view coll, std::span<const uint64_t> ids, const std::vector<std::string>& fields) {
        auto json = glz::write<JSON_SKIP_NULL>(FieldsRequest{fields});
        if (!json) return std::nullopt;
        RustBuf out;
        if (usy_qd_retrieve(q_, ustr(coll), ids.data(), ids.size(), ustr(*json), &out.b) != 0) return std::nullopt;
        return parse_points(out.view());
    }

    bool set_tags(std::string_view coll, std::span<const uint64_t> ids, const std::vector<std::string>& tags) {
        struct TagsOnly {
            std::vector<std::string> t;
        };
        auto json = glz::write_json(TagsOnly{tags});
        if (!json) return false;
        RustBuf out;
        return usy_qd_set_payload(q_, ustr(coll), ids.data(), ids.size(), ustr(*json), &out.b) == 0;
    }

    bool remove(std::string_view coll, std::span<const uint64_t> ids) {
        if (ids.empty()) return true;
        RustBuf out;
        return usy_qd_delete(q_, ustr(coll), ids.data(), ids.size(), &out.b) == 0;
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
    static std::optional<DbPoints> parse_points(std::string_view json) {
        DbPoints pts;
        if (glz::read<JSON_READ>(pts, json)) return std::nullopt;
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
    env.push_back(L'\0');

    std::wstring cmd = L"\"" + exe.wstring() + L"\" --disable-telemetry --config-path \"" + (exe_dir() / L"config" / L"qdrant.yaml").wstring() + L"\"";

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    wil::unique_handle log{CreateFileW((data_dir() / L"qdrant.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (log.get() != INVALID_HANDLE_VALUE && log) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = log.get();
        si.hStdError = log.get();
        si.hStdInput = nullptr;
    }

    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                        env.data(), storage.c_str(), &si, &pi)) {
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
