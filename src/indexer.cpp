// usyseek-indexer.exe - on-demand scan + embedding.
// Lives only for the duration of an index run; the launcher kills it outright on stop.

#include <usyseek/settings.hpp>
#include <usyseek/status.hpp>
#include <usyseek/db.hpp>
#include <usyseek/indexer/media.hpp>

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "resource.h"

using namespace usyseek;

namespace {

void wlog(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    const std::string s = to_utf8(buf);
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------------------------
// Status publishing
// ---------------------------------------------------------------------------------------------

struct Progress {
    std::optional<StatusBlock> block;
    StatusData d;
    std::chrono::steady_clock::time_point last_publish{};

    void phase(Phase p, std::wstring_view msg = {}) {
        d.phase = p;
        set_message(msg);
        publish(true);
        wlog(L"[%s] %.*s\n", phase_label(p), static_cast<int>(msg.size()), msg.data());
    }
    void set_message(std::wstring_view msg) {
        wcsncpy_s(d.message, msg.data(), std::min(msg.size(), std::size(d.message) - 1));
    }
    void set_current(std::wstring_view path) {
        wcsncpy_s(d.current, path.data(), std::min(path.size(), std::size(d.current) - 1));
    }
    void publish(bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_publish < std::chrono::milliseconds(100)) return;
        last_publish = now;
        if (block) block->write(d);
    }
};

Progress g_progress;
HANDLE g_stop = nullptr;
bool g_no_everything = false; // --no-everything: walk included folders directly

bool stopping() noexcept {
    return g_stop && WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0;
}

// ---------------------------------------------------------------------------------------------
// Scope
// ---------------------------------------------------------------------------------------------

struct Scope {
    bool whole_pc = true;
    std::vector<std::wstring> includes;       // normalised
    std::vector<std::wstring> abs_excludes;   // normalised
    std::vector<std::wstring> name_excludes;  // "\name\" lowercase
    bool images = true;
    bool videos = true;
    uint32_t min_kb = 0;

    explicit Scope(const Settings& s) : whole_pc{s.whole_pc}, images{s.index_images}, videos{s.index_videos}, min_kb{s.min_file_kb} {
        for (const auto& d : s.include_dirs) {
            auto n = normalise_dir(to_wide(d));
            if (!n.empty()) includes.push_back(std::move(n));
        }
        auto add_exclude = [this](std::wstring raw) {
            raw = trim(raw);
            if (raw.empty()) return;
            const bool absolute = raw.size() >= 2 && raw[1] == L':';
            if (absolute) {
                abs_excludes.push_back(normalise_dir(raw));
            } else {
                std::wstring n = lower(raw);
                while (n.starts_with(L'*') || n.starts_with(L'\\')) n.erase(0, 1);
                while (n.ends_with(L'*') || n.ends_with(L'\\')) n.pop_back();
                if (!n.empty()) name_excludes.push_back(L"\\" + n + L"\\");
            }
        };
        for (const auto& d : locked_excludes()) add_exclude(d);
        for (const auto& d : s.exclude_dirs) add_exclude(to_wide(d));
    }

    [[nodiscard]] bool excluded(std::wstring_view path) const {
        const std::wstring l = lower(path);
        for (const auto& e : abs_excludes) {
            if (l.size() > e.size() && l.starts_with(e) && l[e.size()] == L'\\') return true;
        }
        for (const auto& n : name_excludes) {
            if (l.contains(n)) return true;
        }
        return false;
    }

    [[nodiscard]] bool in_scope(std::wstring_view path) const {
        if (excluded(path)) return false;
        if (whole_pc) return true;
        const std::wstring l = lower(path);
        for (const auto& i : includes) {
            if (l.size() > i.size() && l.starts_with(i) && l[i.size()] == L'\\') return true;
        }
        return false;
    }

    [[nodiscard]] std::set<wchar_t> drives() const {
        std::set<wchar_t> out;
        if (whole_pc) {
            const DWORD mask = GetLogicalDrives();
            for (int i = 0; i < 26; ++i) {
                if (!(mask & (1u << i))) continue;
                const wchar_t drive = static_cast<wchar_t>(L'A' + i);
                const wchar_t root[] = {drive, L':', L'\\', 0};
                if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
                
                // Check if the entire drive root (e.g. "D:\" or "D:") is in abs_excludes
                const wchar_t probe[] = {drive, L':', L'\\', L'*', 0};
                if (!excluded(probe)) {
                    out.insert(drive);
                }
            }
        } else {
            for (const auto& i : includes) {
                if (i.size() >= 2 && i[1] == L':') {
                    const wchar_t drive = static_cast<wchar_t>(towupper(i[0]));
                    const wchar_t probe[] = {drive, L':', L'\\', L'*', 0};
                    if (!excluded(probe)) {
                        out.insert(drive);
                    }
                }
            }
        }
        return out;
    }

    [[nodiscard]] std::wstring query() const {
        std::wstring q = L"file: ext:";
        bool first = true;
        auto add = [&](auto&& list) {
            for (auto e : list) {
                if (!first) q += L';';
                q += e;
                first = false;
            }
        };
        if (images) add(media::IMAGE_EXTS);
        if (videos) add(media::VIDEO_EXTS);
        if (min_kb > 0) q += L" size:>=" + std::to_wstring(min_kb) + L"kb";
        if (!whole_pc && !includes.empty()) {
            q += L" <";
            for (size_t i = 0; i < includes.size(); ++i) {
                if (i) q += L'|';
                q += L"path:\"" + includes[i] + L"\\\"";
            }
            q += L'>';
        }
        for (const auto& e : abs_excludes) q += L" !path:\"" + e + L"\\\"";
        for (const auto& n : name_excludes) q += L" !path:\"" + n + L"\"";
        return q;
    }

    [[nodiscard]] std::wstring everything_exclude_folders() const {
        std::wstring out;
        auto append = [&](std::wstring_view item) {
            if (!out.empty()) out += L';';
            out += item;
        };
        for (const auto& e : abs_excludes) {
            // Skip whole-drive excludes (e.g. "d:") since they are already omitted from -volumes
            if (e.size() == 2 && e[1] == L':') continue;
            append(e);
        }
        for (const auto& n : name_excludes) {
            // n is stored as "\name\" -> Everything folder filter wants "**\name" or just "name"
            if (n.size() > 2) {
                append(L"**\\" + std::wstring{n.substr(1, n.size() - 2)});
            }
        }
        return out;
    }
};

// ---------------------------------------------------------------------------------------------
// Everything
// ---------------------------------------------------------------------------------------------

struct FileEntry {
    std::wstring path;
    uint64_t size = 0;
    int64_t mtime = 0;
    int kind = 0;
    uint64_t id = 0;
    std::vector<std::string> tags;
};

bool write_everything_exe(const fs::path& target) {
    std::error_code ec;
    if (fs::exists(target, ec)) return true;
    const HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_EVERYTHING_EXE), RT_RCDATA);
    if (!res) return false;
    const HGLOBAL loaded = LoadResource(nullptr, res);
    const auto* ptr = static_cast<const char*>(LockResource(loaded));
    const DWORD size = SizeofResource(nullptr, res);
    if (!ptr || size == 0) return false;
    fs::create_directories(target.parent_path(), ec);
    return ulp::fs::write(target, std::span<const char>{ptr, size});
}

class EverythingSession {
public:
    bool open(const Scope& scope, std::wstring& error) {
        // Always run our own private Everything instance (never use or interfere with a user-running instance)
        const fs::path exe = data_dir() / L"Everything" / L"Everything.exe";
        if (!write_everything_exe(exe)) {
            error = L"Could not extract Everything.exe";
            return false;
        }

        std::wstring vols;
        for (wchar_t d : scope.drives()) {
            if (!vols.empty()) vols += L';';
            vols += d;
            vols += L':';
        }
        owned_.reset(new ulp::Everything(EVERYTHING_INSTANCE, exe.wstring()));
        switch (owned_->try_load(
            L"-safe-mode -no-auto-index"
            L" -volumes \"" + vols + L"\""
            L" -exclude-folders \"" + scope.everything_exclude_folders() + L"\""
        )) {
            case ulp::Everything::LoadStatus::Success: break;
            case ulp::Everything::LoadStatus::UACRejected:
                error = L"Everything needs administrator access to read the drive index";
                return false;
            default:
                error = L"Everything failed to start";
                return false;
        }

        return true;
    }

    std::vector<FileEntry> query(const Scope& scope, std::wstring& error) {
        std::vector<FileEntry> out;
        static constexpr auto flags = ulp::everything::RequestFlags{0}.file_name().parent_path().size().date_modified();
        static constexpr ulp::everything::EverythingSearch opts{.RequestFlags = flags};
        if (!ulp::Everything::do_query(scope.query(), opts)) {
            error = L"Everything query failed";
            return out;
        }
        out.reserve(ulp::Everything::query_results_count());
        ulp::Everything::walk_results<flags>(ulp::everything::Callbacks{
            .on_file = [&](const ulp::everything::File<flags>& f) {
                FileEntry e;
                e.path = std::wstring{f.parent_path()} + L"\\" + f.filename();
                e.kind = media::kind_of(e.path);
                if (e.kind < 0 || !scope.in_scope(e.path)) return;
                e.size = f.size_or_default();
                e.mtime = filetime_to_unix(f.date_modified_or_default());
                e.id = path_id(e.path);
                out.push_back(std::move(e));
            },
        });
        ulp::Everything::reset_results();
        return out;
    }

private:
    std::unique_ptr<ulp::Everything> owned_;
};

std::vector<FileEntry> walk_dirs(const Scope& scope) {
    std::vector<FileEntry> out;
    std::vector<std::wstring> stack{scope.includes.begin(), scope.includes.end()};
    const uint64_t min_bytes = static_cast<uint64_t>(scope.min_kb) * 1024;
    while (!stack.empty() && !stopping()) {
        const std::wstring dir = std::move(stack.back());
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        wil::unique_hfind h{FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH)};
        if (!h) continue;
        do {
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
            std::wstring path = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!scope.excluded(path + L"\\x")) stack.push_back(std::move(path));
                continue;
            }
            FileEntry e;
            e.kind = media::kind_of(path);
            if (e.kind < 0 || (e.kind == 0 && !scope.images) || (e.kind == 1 && !scope.videos)) continue;
            e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            if (e.size < min_bytes || !scope.in_scope(path)) continue;
            e.mtime = filetime_to_unix((static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime);
            e.id = path_id(path);
            e.path = std::move(path);
            out.push_back(std::move(e));
        } while (FindNextFileW(h.get(), &fd));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Failure cache (files that cannot be decoded are not retried until they change)
// ---------------------------------------------------------------------------------------------

struct FailureCache {
    fs::path path;
    std::unordered_map<uint64_t, FailedFile> entries;

    explicit FailureCache(std::string_view coll) : path{failed_path(coll)} {
        for (auto& f : load_failed_files(coll)) {
            entries[f.id] = std::move(f);
        }
    }
    [[nodiscard]] bool known(const FileEntry& e) {
        const auto it = entries.find(e.id);
        if (it == entries.end() || it->second.mtime != e.mtime || it->second.size != e.size) return false;
        if (it->second.path.empty()) it->second.path = e.path;
        return true;
    }
    void add(const FileEntry& e, std::wstring_view reason = L"Decode failed (corrupt or unsupported media)") {
        std::wstring clean{reason};
        for (wchar_t& c : clean) {
            if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
        }
        entries[e.id] = FailedFile{e.id, e.mtime, e.size, e.path, std::move(clean)};
    }
    void remove(uint64_t id) { entries.erase(id); }
    void prune(const Scope& scope) {
        std::erase_if(entries, [&](const auto& kv) {
            const auto& f = kv.second;
            if (f.path.empty()) return false;
            return !scope.in_scope(f.path) || GetFileAttributesW(f.path.c_str()) == INVALID_FILE_ATTRIBUTES;
        });
    }
    void save() const {
        std::string out;
        char prefix[96];
        for (const auto& [id, f] : entries) {
            std::snprintf(prefix, sizeof(prefix), "%llu\t%lld\t%llu\t",
                          static_cast<unsigned long long>(id),
                          static_cast<long long>(f.mtime),
                          static_cast<unsigned long long>(f.size));
            out += prefix;
            out += to_utf8(f.path);
            out += '\t';
            out += to_utf8(f.reason.empty() ? L"Decode failed" : f.reason);
            out += '\n';
        }
        (void)ulp::fs::write(path, std::string_view{out});
    }
};

// ---------------------------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------------------------

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t cap) : cap_{cap} {}

    bool push(T v) {
        std::unique_lock lk{m_};
        not_full_.wait(lk, [&] { return q_.size() < cap_ || closed_; });
        if (closed_) return false;
        q_.push_back(std::move(v));
        not_empty_.notify_one();
        return true;
    }
    // false = timeout or closed-and-empty (check done())
    bool pop(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock lk{m_};
        if (!not_empty_.wait_for(lk, timeout, [&] { return !q_.empty() || closed_; })) return false;
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        not_full_.notify_one();
        return true;
    }
    void close() {
        std::lock_guard lk{m_};
        closed_ = true;
        not_full_.notify_all();
        not_empty_.notify_all();
    }
    [[nodiscard]] bool done() {
        std::lock_guard lk{m_};
        return closed_ && q_.empty();
    }

private:
    std::mutex m_;
    std::condition_variable not_full_, not_empty_;
    std::deque<T> q_;
    size_t cap_;
    bool closed_ = false;
};

struct DecodedItem {
    size_t index = 0;
    std::optional<media::Decoded> data;
};

std::vector<std::string> ancestors_of(std::wstring_view path) {
    std::vector<std::string> out;
    const std::wstring l = lower(path);
    for (size_t pos = l.find(L'\\'); pos != std::wstring::npos; pos = l.find(L'\\', pos + 1)) {
        out.push_back(to_utf8(std::wstring_view{l}.substr(0, pos)));
    }
    return out;
}

MediaPayload make_payload(const FileEntry& e, const media::Decoded& d) {
    MediaPayload p;
    p.p = to_utf8(e.path);
    const auto slash = e.path.find_last_of(L'\\');
    p.n = to_utf8(slash == std::wstring::npos ? e.path : e.path.substr(slash + 1));
    p.a = ancestors_of(e.path);
    p.k = e.kind;
    p.m = e.mtime;
    p.s = e.size;
    p.w = d.width;
    p.h = d.height;
    p.d = d.duration_ms;
    p.t = e.tags;
    return p;
}

} // namespace

namespace usyseek {

struct PyLoadCfg {
    std::string model_dir;
    std::string model_name;
    std::string device;
};
struct PyLoadReq {
    std::string cmd = "load";
    PyLoadCfg cfg;
};
struct PyExportReq {
    std::string cmd = "export_text";
    std::string model_dir;
    std::string model_name;
};
struct PySuggestReq {
    std::string cmd = "suggest_batch";
    uint32_t limit = 128;
};
struct PyBenchmarkReq {
    std::string cmd = "benchmark_batch";
    uint32_t limit = 128;
};
struct PyEmbedReq {
    std::string cmd = "embed";
    std::string shm;
    uint64_t shm_bytes = 0;
    uint32_t frames = 0;
};
struct PyLoadInfo {
    uint32_t size = 0;
    uint32_t dim = 0;
    std::string resize_mode;
    std::string device;
    std::string backend;
};
struct PyResp {
    bool ok = false;
    std::string error;
    PyLoadInfo info{};
    std::string path;
    uint32_t batch = 16;
    uint32_t hint = 0;
};

} // namespace usyseek

namespace {

struct PyVision {
    uint32_t size = 0;
    uint32_t dim = 0;
    uint32_t last_hint = 0;
    media::ResizeMode mode = media::ResizeMode::Squash;
    std::wstring device;

    wil::unique_handle job;
    wil::unique_process_information proc;
    wil::unique_handle pipe;
    wil::unique_event_nothrow ov_event;
    wil::unique_handle shm;
    void* shm_view = nullptr;
    size_t shm_bytes = 0;
    uint32_t shm_gen = 0;
    std::string shm_name;
    std::string read_buf;

    ~PyVision() {
        unmap_shm();
    }

    void unmap_shm() noexcept {
        if (shm_view) {
            UnmapViewOfFile(shm_view);
            shm_view = nullptr;
        }
        shm.reset();
        shm_bytes = 0;
    }

    void start(const VenvInfo& venv, const Settings& settings) {
        std::error_code ec;
        if (!venv.exists || !fs::exists(venv.python_exe, ec)) {
            throw std::runtime_error("Python environment not found");
        }
        const fs::path worker_script = exe_dir() / L"python" / L"usyseek_vision.py";
        if (!fs::exists(worker_script, ec)) {
            throw std::runtime_error("usyseek_vision.py not found");
        }

        const DWORD pid = GetCurrentProcessId();
        const std::wstring pipe_name = L"\\\\.\\pipe\\usyseek.vision." + std::to_wstring(pid);
        pipe.reset(CreateNamedPipeW(
            pipe_name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 65536, 65536, 0, nullptr));
        if (!pipe) {
            throw std::runtime_error("Could not create vision IPC pipe");
        }
        ov_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        job = make_kill_on_close_job();

        std::wstring cmd = L"\"" + venv.python_exe + L"\" -u \"" + worker_script.wstring() + L"\" --worker \"" + pipe_name + L"\"";
        if (!settings.vision_script.empty()) {
            cmd += L" \"" + to_wide(settings.vision_script) + L"\"";
        }

        STARTUPINFOW si{sizeof(si)};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                            exe_dir().c_str(), &si, &proc)) {
            throw std::runtime_error("Failed to start Python vision worker");
        }
        if (job) AssignProcessToJobObject(job.get(), proc.hProcess);
        PROCESS_POWER_THROTTLING_STATE throttling{};
        throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        throttling.StateMask = 0; // Disable Windows EcoQoS / Efficiency Mode on worker
        SetProcessInformation(proc.hProcess, ProcessPowerThrottling, &throttling, sizeof(throttling));
        SetPriorityClass(proc.hProcess, ABOVE_NORMAL_PRIORITY_CLASS);
        ResumeThread(proc.hThread);

        OVERLAPPED ov{};
        ov.hEvent = ov_event.get();
        ResetEvent(ov_event.get());
        if (!ConnectNamedPipe(pipe.get(), &ov)) {
            const DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                wait_ov(ov, "Python worker exited before connecting");
            } else if (err != ERROR_PIPE_CONNECTED) {
                throw std::runtime_error("Vision pipe connection failed");
            }
        }

        PyResp r = call(PyLoadReq{.cfg = {settings.model_dir, settings.model_name, settings.device}});
        size = r.info.size;
        dim = r.info.dim;
        mode = media::parse_resize_mode(r.info.resize_mode);
        device = to_wide(r.info.device + " \xc2\xb7 " + r.info.backend);
    }

    void export_text(const std::string& model_dir, const std::string& model_name) {
        (void)call(PyExportReq{.model_dir = model_dir, .model_name = model_name});
    }

    uint32_t suggest_batch(uint32_t limit = 128) {
        return call(PySuggestReq{.limit = limit}).batch;
    }

    uint32_t benchmark_batch(uint32_t limit = 128) {
        return call(PyBenchmarkReq{.limit = limit}).batch;
    }

    uint8_t* prepare_shm(size_t frames) {
        const size_t in_bytes = frames * size * size * 3;
        const size_t out_bytes = frames * dim * sizeof(float);
        const size_t needed = std::max<size_t>(65536, std::max(in_bytes, out_bytes));
        if (needed > shm_bytes || !shm_view) {
            unmap_shm();
            const size_t min_alloc = static_cast<size_t>(std::max<size_t>(frames, 128)) * size * size * 3;
            const size_t alloc = std::max(needed, std::max(min_alloc, shm_bytes * 2));
            shm_name = "Local\\usyseek.vision.shm." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(++shm_gen);
            shm.reset(CreateFileMappingA(
                INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                static_cast<DWORD>(alloc >> 32), static_cast<DWORD>(alloc & 0xFFFFFFFFu),
                shm_name.c_str()));
            if (!shm) throw std::runtime_error("Shared memory allocation failed");
            shm_view = MapViewOfFile(shm.get(), FILE_MAP_ALL_ACCESS, 0, 0, alloc);
            if (!shm_view) {
                shm.reset();
                throw std::runtime_error("Shared memory map failed");
            }
            shm_bytes = alloc;
        }
        return static_cast<uint8_t*>(shm_view);
    }

    const float* embed(size_t frames) {
        PyResp r = call(PyEmbedReq{
            .shm = shm_name,
            .shm_bytes = shm_bytes,
            .frames = static_cast<uint32_t>(frames),
        });
        last_hint = r.hint;
        return static_cast<const float*>(shm_view);
    }

private:
    DWORD wait_ov(OVERLAPPED& ov, const char* crash_msg) {
        HANDLE handles[3] = {ov_event.get(), proc.hProcess, g_stop};
        const DWORD count = g_stop ? 3 : 2;
        const DWORD w = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) {
            CancelIoEx(pipe.get(), &ov);
            DWORD dummy = 0;
            GetOverlappedResult(pipe.get(), &ov, &dummy, TRUE);
            if (w == WAIT_OBJECT_0 + 2) throw std::runtime_error("Indexing stopped");
            throw std::runtime_error(crash_msg);
        }
        DWORD bytes = 0;
        if (!GetOverlappedResult(pipe.get(), &ov, &bytes, FALSE)) {
            throw std::runtime_error(crash_msg);
        }
        return bytes;
    }

    template <typename Req>
    PyResp call(const Req& req) {
        std::string line;
        (void)glz::write_json(req, line);
        line.push_back('\n');

        size_t written = 0;
        while (written < line.size()) {
            OVERLAPPED ov{};
            ov.hEvent = ov_event.get();
            ResetEvent(ov_event.get());
            DWORD n = 0;
            if (!WriteFile(pipe.get(), line.data() + written, static_cast<DWORD>(line.size() - written), &n, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING) throw std::runtime_error("Pipe write failed");
                n = wait_ov(ov, "Python worker terminated unexpectedly");
            }
            written += n;
        }

        for (;;) {
            if (const auto nl = read_buf.find('\n'); nl != std::string::npos) {
                std::string msg = read_buf.substr(0, nl);
                read_buf.erase(0, nl + 1);
                PyResp resp{};
                if (glz::read<JSON_READ>(resp, msg)) {
                    throw std::runtime_error("Invalid response from Python worker");
                }
                if (!resp.ok) {
                    throw std::runtime_error(resp.error.empty() ? "Python worker error" : resp.error);
                }
                return resp;
            }
            char chunk[4096];
            OVERLAPPED ov{};
            ov.hEvent = ov_event.get();
            ResetEvent(ov_event.get());
            DWORD n = 0;
            if (!ReadFile(pipe.get(), chunk, sizeof(chunk), &n, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING) throw std::runtime_error("Python worker closed pipe");
                n = wait_ov(ov, "Python worker terminated unexpectedly");
            }
            if (n == 0) throw std::runtime_error("Python worker closed pipe");
            read_buf.append(chunk, n);
        }
    }
};

std::wstring py_error_text(const std::exception& e) {
    std::string s = e.what();
    if (const auto nl = s.find_last_of('\n', s.size() > 1 ? s.size() - 2 : 0); nl != std::string::npos && s.size() > 300) s = s.substr(nl + 1);
    return to_wide(s.substr(0, 250));
}

int export_text_only(const Settings& settings) {
    const VenvInfo venv = read_venv(to_wide(settings.venv_dir));
    std::error_code ec;
    if (!venv.exists || !fs::exists(venv.python_exe, ec)) return 1;

    const fs::path script = settings.vision_script.empty() ? exe_dir() / L"python" / L"usyseek_vision.py" : fs::path{to_wide(settings.vision_script)};
    if (!fs::exists(script, ec)) return 1;

    std::wstring cmd = L"\"" + venv.python_exe + L"\" -u \"" + script.wstring() + L"\" --export-text \"" + to_wide(settings.model_dir) + L"\" \"" + to_wide(settings.model_name) + L"\"";
    wil::unique_handle job = make_kill_on_close_job();
    STARTUPINFOW si{sizeof(si)};
    wil::unique_process_information pi;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, exe_dir().c_str(), &si, &pi)) {
        return 1;
    }
    if (job) AssignProcessToJobObject(job.get(), pi.hProcess);
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    return static_cast<int>(code);
}

int run_indexer(Settings& settings) {
    auto& pr = g_progress;
    const std::string coll = settings.model_name;
    const Scope scope{settings};

    // --- Database
    pr.phase(Phase::Starting, L"Connecting to database");
    const bool has_launcher = find_launcher() != nullptr;
    if (has_launcher) send_to_launcher(Cmd::NeedQdrant);
    wil::unique_handle own_job;
    wil::unique_process_information own_qdrant;
    Db db;
    std::string err;
    if (!db.connect(settings.qdrant_grpc_port, &err)) {
        pr.phase(Phase::Error, to_wide(err));
        return 1;
    }
    if (!db.wait_ready(std::chrono::milliseconds(has_launcher ? 20'000 : 500), g_stop)) {
        if (!has_launcher) {
            own_job = make_kill_on_close_job();
            own_qdrant = start_qdrant(settings, own_job.get());
        }
        if (!db.wait_ready(std::chrono::seconds(30), g_stop)) {
            pr.phase(Phase::Error, L"Database did not start");
            return 1;
        }
    }

    // --- Scan concurrently with the model load
    std::vector<FileEntry> files;
    std::wstring scan_error;
    std::jthread scanner([&] {
        ulp::win::COMWrapper<false> com{COINIT_MULTITHREADED};
        EverythingSession ev;
        if (g_no_everything) scan_error = L"disabled";
        else if (ev.open(scope, scan_error)) {
            if (!stopping()) files = ev.query(scope, scan_error);
            return;
        }
        if (scope.whole_pc) return;
        // Specific folders can still be walked directly when Everything is unavailable
        wlog(L"Everything unavailable (%s), walking included folders\n", scan_error.c_str());
        scan_error.clear();
        files = walk_dirs(scope);
    });

    // --- Python vision tower (out-of-process worker via shared memory)
    pr.phase(Phase::LoadingModel, to_wide(settings.model_name));
    const VenvInfo venv = read_venv(to_wide(settings.venv_dir));
    std::error_code venv_ec;
    if (!venv.exists || !fs::exists(venv.python_exe, venv_ec)) {
        pr.phase(Phase::Error, L"Python environment not found");
        return 1;
    }

    PyVision vision;
    try {
        vision.start(venv, settings);
        wcsncpy_s(pr.d.device, vision.device.c_str(), std::size(pr.d.device) - 1);

        // The search UI needs the text tower as ONNX; export it once if missing
        const fs::path onnx = fs::path{to_wide(settings.model_dir)} / (to_wide(settings.model_name) + L"-text.onnx");
        std::error_code ec;
        if (!fs::exists(onnx, ec)) {
            pr.phase(Phase::LoadingModel, L"Exporting text model");
            vision.export_text(settings.model_dir, settings.model_name);
        }
    } catch (const std::exception& e) {
        scanner.request_stop();
        if (stopping()) return 2;
        pr.phase(Phase::Error, py_error_text(e));
        std::printf("%s\n", e.what());
        return 1;
    }

    if (!db.ensure_collection(coll, vision.dim, &err)) {
        pr.phase(Phase::Error, to_wide(err));
        return 1;
    }

    pr.phase(Phase::Scanning);
    scanner.join();
    if (stopping()) return 2;
    if (!scan_error.empty()) {
        pr.phase(Phase::Error, scan_error);
        return 1;
    }

    // --- Compare against the database (carry tags over for changed files)
    pr.phase(Phase::Comparing);
    uint64_t already_embedded = 0;
    FailureCache failures{coll};
    std::vector<size_t> todo;
    std::unordered_set<uint64_t> found_ids;
    found_ids.reserve(files.size());
    {
        constexpr size_t CHUNK = 1024;
        std::vector<uint64_t> ids;
        for (size_t i = 0; i < files.size() && !stopping(); i += CHUNK) {
            const size_t end = std::min(files.size(), i + CHUNK);
            ids.clear();
            for (size_t j = i; j < end; ++j) {
                ids.push_back(files[j].id);
                found_ids.insert(files[j].id);
            }
            std::unordered_map<uint64_t, MediaPayload> existing;
            if (auto pts = db.retrieve(coll, ids, {"m", "s", "t"})) {
                for (auto& p : pts->points) existing.emplace(p.id, std::move(p.p));
            }
            for (size_t j = i; j < end; ++j) {
                auto& f = files[j];
                const auto it = existing.find(f.id);
                if (it != existing.end()) {
                    if (it->second.m == f.mtime && it->second.s == f.size) {
                        ++already_embedded;
                        failures.remove(f.id);
                        continue;
                    }
                    f.tags = std::move(it->second.t);
                }
                if (failures.known(f)) {
                    ++pr.d.failed;
                    continue;
                }
                todo.push_back(j);
            }
            pr.publish();
        }
    }
    pr.d.found = files.size();
    pr.d.embedded = already_embedded;
    if (stopping()) return 2;

    // --- Remove entries for deleted / newly excluded files
    pr.phase(Phase::Cleaning);
    {
        failures.prune(scope);
        std::optional<uint64_t> cursor;
        std::vector<uint64_t> stale;
        do {
            ScrollRequest req;
            req.limit = 4096;
            req.offset = cursor;
            req.fields = {"p"};
            auto page = db.scroll(coll, req);
            if (!page) break;
            for (const auto& p : page->points) {
                if (found_ids.contains(p.id)) continue;
                const std::wstring path = to_wide(p.p.p);
                if (!scope.in_scope(path) || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) stale.push_back(p.id);
            }
            cursor = page->next;
        } while (cursor && !stopping());
        if (!stale.empty()) {
            wlog(L"Removing %zu stale entries\n", stale.size());
            db.remove(coll, stale);
        }
    }

    // Images first (fast), then videos; path order keeps directory reads sequential
    std::stable_sort(todo.begin(), todo.end(), [&](size_t a, size_t b) {
        if (files[a].kind != files[b].kind) return files[a].kind < files[b].kind;
        return a < b;
    });
    pr.d.todo = todo.size();
    if (todo.empty() || stopping()) {
        failures.save();
        IndexState st = load_json_file<IndexState>(state_path());
        st.last_index_unix = unix_now();
        st.last_index_found = files.size();
        st.last_index_embedded = pr.d.embedded;
        st.last_error.clear();
        save_json_file(state_path(), st);
        pr.phase(stopping() ? Phase::Stopped : Phase::Done);
        return 0;
    }

    // --- Batch size
    uint32_t batch = settings.batch_size;
    const bool auto_batch = batch == 0;
    const std::string auto_batch_key = settings.model_name + "|" + to_utf8(vision.device) + "|" + std::to_string(vision.size);
    if (auto_batch) {
        IndexState cached = load_json_file<IndexState>(state_path());
        if (cached.auto_batch > 0 && cached.auto_batch_key == auto_batch_key) {
            batch = cached.auto_batch;
        } else {
            try {
                batch = vision.benchmark_batch(128);
            } catch (const std::exception& e) {
                std::printf("benchmark_batch failed: %s\n", e.what());
                batch = 16;
            }
            cached.auto_batch = batch;
            cached.auto_batch_key = auto_batch_key;
            save_json_file(state_path(), cached);
        }
    }
    batch = std::max(1u, batch);
    pr.d.batch = batch;
    wlog(L"Batch size %u, %zu files to embed\n", batch, todo.size());

    // --- Decode workers
    pr.phase(Phase::Embedding);
    const uint32_t hw = std::max(2u, std::thread::hardware_concurrency());
    const uint32_t workers = settings.decode_threads ? settings.decode_threads : std::clamp(hw - 2, 2u, 16u);
    BoundedQueue<DecodedItem> queue{static_cast<size_t>(batch) * 2 + workers};
    std::atomic<size_t> next{0};
    std::atomic<uint32_t> alive{workers};
    std::vector<std::jthread> pool;
    pool.reserve(workers);
    for (uint32_t w = 0; w < workers; ++w) {
        pool.emplace_back([&] {
            ulp::win::COMWrapper<false> com{COINIT_MULTITHREADED};
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            media::Decoder dec{vision.size, vision.mode};
            for (;;) {
                const size_t i = next.fetch_add(1);
                if (i >= todo.size() || stopping()) break;
                const FileEntry& f = files[todo[i]];
                DecodedItem item{todo[i], {}};
                try {
                    item.data = f.kind == 0 ? dec.image(f.path) : dec.video(f.path, settings.video_frames);
                } catch (...) {
                    // Leaves item.data empty: the file is counted as failed and remembered so it is not retried
                }
                if (!queue.push(std::move(item))) break;
            }
            if (alive.fetch_sub(1) == 1) queue.close();
        });
    }

    // --- Upload (single background thread so the GPU never waits on gRPC)
    std::future<void> upload;
    std::vector<uint64_t> up_ids;
    std::vector<float> up_vecs;
    std::vector<MediaPayload> up_payloads;
    auto flush = [&](bool wait) {
        if (upload.valid()) upload.get();
        if (!up_ids.empty()) {
            upload = std::async(std::launch::async, [&db, &coll, ids = std::move(up_ids), vecs = std::move(up_vecs), pls = std::move(up_payloads), dim = vision.dim] {
                std::string e;
                if (!db.upsert(coll, ids, vecs, dim, pls, &e)) std::printf("upsert failed: %s\n", e.c_str());
            });
            up_ids = {};
            up_vecs = {};
            up_payloads = {};
        }
        if (wait && upload.valid()) upload.get();
    };

    const size_t frame_bytes = static_cast<size_t>(vision.size) * vision.size * 3;
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t done_this_run = 0;
    std::vector<DecodedItem> pending;

    while (!stopping()) {
        // Gather up to `batch` frames
        size_t frames = 0;
        pending.clear();
        while (frames < batch) {
            DecodedItem item;
            if (!queue.pop(item, std::chrono::milliseconds(50))) {
                if (queue.done() || (!pending.empty() && frames > 0)) break;
                if (stopping()) break;
                pr.publish();
                continue;
            }
            ++pr.d.processed;
            const FileEntry& f = files[item.index];
            pr.set_current(f.path);
            if (!item.data || item.data->frames.empty()) {
                ++pr.d.failed;
                failures.add(f);
                continue;
            }
            frames += item.data->frames.size();
            pending.push_back(std::move(item));
        }
        if (pending.empty()) {
            if (queue.done()) break;
            continue;
        }

        const float* rows = nullptr;
        try {
            // Pack directly into shared memory
            uint8_t* dst = vision.prepare_shm(frames);
            for (const auto& it : pending) {
                for (const auto& fr : it.data->frames) {
                    std::memcpy(dst, fr.rgb.data(), frame_bytes);
                    dst += frame_bytes;
                }
            }
            rows = vision.embed(frames);
            if (const uint32_t hint = vision.last_hint; hint > 0 && hint < batch) {
                batch = hint;
                pr.d.batch = batch;
                std::printf("Reduced batch size to %u after running out of VRAM\n", batch);
            }
        } catch (const std::exception& e) {
            if (stopping()) break;
            std::printf("embed failed: %s\n", e.what());
            const std::wstring reason = py_error_text(e);
            pr.set_message(reason);
            for (const auto& it : pending) {
                ++pr.d.failed;
                failures.add(files[it.index], reason);
            }
            continue;
        }

        const size_t dim = vision.dim;
        size_t row = 0;
        for (const auto& it : pending) {
            const size_t n = it.data->frames.size();
            std::vector<float> v(dim, 0.0f);
            for (size_t k = 0; k < n; ++k, ++row) {
                for (size_t c = 0; c < dim; ++c) v[c] += rows[row * dim + c];
            }
            if (n > 1) {
                double norm = 0;
                for (float x : v) norm += static_cast<double>(x) * x;
                const float inv = norm > 0 ? static_cast<float>(1.0 / std::sqrt(norm)) : 1.0f;
                for (float& x : v) x *= inv;
            }
            const FileEntry& f = files[it.index];
            failures.remove(f.id);
            up_ids.push_back(f.id);
            up_vecs.insert(up_vecs.end(), v.begin(), v.end());
            up_payloads.push_back(make_payload(f, *it.data));
            ++pr.d.embedded;
            ++done_this_run;
        }
        if (up_ids.size() >= 256) flush(false);

        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        pr.d.rate = secs > 0.5 ? static_cast<float>(done_this_run / secs) : 0.0f;
        pr.publish();
    }

    const bool was_stopped = stopping();
    if (was_stopped) {
        pr.set_message(L"Stopping indexer…");
        pr.publish();
    }
    queue.close();
    pool.clear();
    flush(true);
    failures.save();

    IndexState st = load_json_file<IndexState>(state_path());
    st.last_index_unix = unix_now();
    st.last_index_found = files.size();
    st.last_index_embedded = pr.d.embedded;
    st.last_error.clear();
    if (auto_batch && batch < st.auto_batch) st.auto_batch = batch;
    save_json_file(state_path(), st);
    pr.phase(was_stopped ? Phase::Stopped : Phase::Done);
    return was_stopped ? 2 : 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    bool export_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a{argv[i]};
        if (a == L"--no-everything") g_no_everything = true;
        else if (a == L"--export-text") export_only = true;
    }
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (export_only) {
        return export_text_only(load_settings());
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    ulp::win::COMWrapper<false> com{COINIT_MULTITHREADED};
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    // Only one indexer at a time
    wil::unique_mutex_nothrow single{CreateMutexW(nullptr, TRUE, L"Local\\usyseek.indexer")};
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 3;

    g_progress.block = StatusBlock::create();
    g_progress.d.pid = GetCurrentProcessId();
    g_progress.d.started_unix = unix_now();
    wil::unique_event_nothrow stop{OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, INDEXER_STOP_EVENT)};
    if (!stop) stop.reset(CreateEventW(nullptr, TRUE, FALSE, INDEXER_STOP_EVENT));
    g_stop = stop.get();

    Settings settings = load_settings();
    int code = 1;
    try {
        code = run_indexer(settings);
    } catch (const std::exception& e) {
        g_progress.phase(Phase::Error, to_wide(e.what()));
    }
    if (g_progress.d.phase == Phase::Error) {
        IndexState st = load_json_file<IndexState>(state_path());
        st.last_error = to_utf8(g_progress.d.message);
        save_json_file(state_path(), st);
    }
    // Keep the final status readable for a moment so the UI can pick it up
    Sleep(1500);
    MFShutdown();
    return code;
}
