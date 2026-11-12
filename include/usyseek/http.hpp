#pragma once

#include "common.hpp"

#include <winhttp.h>
#include <wil/resource.h>

#include <functional>
#include <stop_token>

namespace usyseek::http {

struct Result {
    bool ok = false;
    bool cancelled = false;
    DWORD status = 0;   // HTTP status when the server answered
    std::wstring error; // empty on success
};

/// `total` is 0 when the server did not announce a length. Called at most ~10 times a second.
using Progress = std::function<void(uint64_t done, uint64_t total)>;

namespace detail {

/// Closing a WinHTTP handle from another thread is how a blocking call is cancelled, so the close is atomic.
class Handle {
public:
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&&) = delete;
    Handle& operator=(Handle&&) = delete;
    ~Handle() { close(); }

    void reset(HINTERNET h) noexcept {
        close();
        h_.store(h);
    }
    void close() noexcept {
        if (HINTERNET h = h_.exchange(nullptr)) WinHttpCloseHandle(h);
    }
    [[nodiscard]] HINTERNET get() const noexcept { return h_.load(); }
    explicit operator bool() const noexcept { return get() != nullptr; }

private:
    std::atomic<HINTERNET> h_{nullptr};
};

struct Response {
    Handle session, connection, request;
    DWORD status = 0;
    uint64_t content_length = 0; // 0 = unknown
};

inline std::wstring win_error(const wchar_t* what) {
    return std::wstring{what} + L" (WinHTTP error " + std::to_wstring(GetLastError()) + L")";
}

/// Sends a GET and waits for the response headers. Redirects are followed by WinHTTP.
inline bool open(const std::wstring& url, uint64_t range_from, Response& r, std::wstring& error) {
    wchar_t host[256]{};
    std::wstring path(url.size() + 1, L'\0');
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path.data();
    uc.dwUrlPathLength = static_cast<DWORD>(path.size());
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
        error = L"Invalid URL";
        return false;
    }

    r.session.reset(WinHttpOpen(L"usyseek", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!r.session) {
        error = win_error(L"Could not start WinHTTP");
        return false;
    }
    WinHttpSetTimeouts(r.session.get(), 15'000, 15'000, 30'000, 30'000);
    r.connection.reset(WinHttpConnect(r.session.get(), host, uc.nPort, 0));
    if (!r.connection) {
        error = win_error(L"Could not connect");
        return false;
    }
    r.request.reset(WinHttpOpenRequest(r.connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!r.request) {
        error = win_error(L"Could not create request");
        return false;
    }
    if (range_from > 0) {
        const std::wstring range = L"Range: bytes=" + std::to_wstring(range_from) + L"-";
        WinHttpAddRequestHeaders(r.request.get(), range.c_str(), static_cast<DWORD>(range.size()), WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (!WinHttpSendRequest(r.request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(r.request.get(), nullptr)) {
        error = win_error(L"Could not reach server");
        return false;
    }

    DWORD size = sizeof(r.status);
    WinHttpQueryHeaders(r.request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &r.status, &size, WINHTTP_NO_HEADER_INDEX);
    wchar_t len[32]{};
    size = sizeof(len);
    if (WinHttpQueryHeaders(r.request.get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &size, WINHTTP_NO_HEADER_INDEX)) {
        r.content_length = _wcstoui64(len, nullptr, 10);
    }
    return true;
}

inline std::wstring status_text(DWORD status) {
    switch (status) {
        case 401:
        case 403: return L"Access denied (HTTP " + std::to_wstring(status) + L") — the file may be gated or private";
        case 404: return L"Not found on the server (HTTP 404)";
        default: return L"Server answered HTTP " + std::to_wstring(status);
    }
}

} // namespace detail

/// Small GET into memory (API responses). Fails on anything but HTTP 200 or when the body exceeds `max_bytes`.
inline std::optional<std::string> get(const std::wstring& url, size_t max_bytes = 16u << 20) {
    detail::Response r;
    std::wstring error;
    if (!detail::open(url, 0, r, error) || r.status != 200) return std::nullopt;
    std::string out;
    std::vector<char> buf(64u << 10);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(r.request.get(), buf.data(), static_cast<DWORD>(buf.size()), &got)) return std::nullopt;
        if (got == 0) break;
        out.append(buf.data(), got);
        if (out.size() > max_bytes) return std::nullopt;
    }
    return out;
}

/// Streams `url` into `dest`. Data lands in `dest` + ".part" first and is renamed once complete, so `dest`
/// only ever exists whole; an interrupted or cancelled download leaves the part file and resumes from it next time.
inline Result download(const std::wstring& url, const fs::path& dest, const Progress& progress = {}, const std::stop_token& stop = {}) {
    Result res;
    const fs::path part = dest.wstring() + L".part";
    std::error_code ec;
    uint64_t have = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
    if (ec) have = 0;

    detail::Response r;
    // Declared after `r` so it is gone before the handles: a stop request aborts whichever call is blocking
    const std::stop_callback abort{stop, [&r] { r.request.close(); }};
    const auto fail = [&](std::wstring why) {
        res.cancelled = stop.stop_requested();
        res.error = res.cancelled ? L"Cancelled" : std::move(why);
        return res;
    };

    if (!detail::open(url, have, r, res.error)) return fail(res.error);
    if (have > 0 && r.status == 416) {
        // The part file is not a prefix of what the server has now: start over
        fs::remove(part, ec);
        have = 0;
        if (!detail::open(url, 0, r, res.error)) return fail(res.error);
    }
    res.status = r.status;
    if (r.status == 200) have = 0; // the server ignored the range
    else if (r.status != 206) return fail(detail::status_text(r.status));
    const uint64_t total = r.content_length > 0 ? have + r.content_length : 0;

    wil::unique_hfile file{CreateFileW(part.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, have > 0 ? OPEN_EXISTING : CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!file) return fail(L"Could not write " + part.filename().wstring());
    if (have > 0) {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(have);
        if (!SetFilePointerEx(file.get(), pos, nullptr, FILE_BEGIN)) return fail(L"Could not resume " + part.filename().wstring());
    }

    uint64_t done = have;
    std::vector<char> buf(256u << 10);
    auto last_report = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(r.request.get(), buf.data(), static_cast<DWORD>(buf.size()), &got)) {
            return fail(detail::win_error(L"Connection lost"));
        }
        if (got == 0) break;
        DWORD written = 0;
        if (!WriteFile(file.get(), buf.data(), got, &written, nullptr) || written != got) {
            return fail(L"Could not write " + part.filename().wstring() + L" (disk full?)");
        }
        done += got;
        if (const auto now = std::chrono::steady_clock::now(); progress && now - last_report >= std::chrono::milliseconds(100)) {
            last_report = now;
            progress(done, total);
        }
    }
    if (stop.stop_requested()) return fail({});
    if (total > 0 && done != total) return fail(L"Connection closed early — try again to resume");
    if (progress) progress(done, total);

    file.reset();
    if (!MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) return fail(L"Could not create " + dest.filename().wstring());
    res.ok = true;
    return res;
}

} // namespace usyseek::http
