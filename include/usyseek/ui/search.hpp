#pragma once

#include <usyseek/db.hpp>

#include <onnxruntime_cxx_api.h>

#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <thread>

namespace usyseek::ui {

// ---------------------------------------------------------------------------------------------
// Query language
//   free text          semantic search
//   tag:x  -tag:x      require / exclude a tag ("quoted values" allowed)
//   similar:<path>     visually similar to an indexed file
//   name:x             file name contains (prefix tokens)
//   dir:<path> / a bare path   restrict to a folder (recursive)
//   type:image|video
// ---------------------------------------------------------------------------------------------

struct ParsedQuery {
    std::wstring text;
    std::vector<std::string> tags;
    std::vector<std::string> not_tags;
    std::vector<std::string> dirs;
    std::vector<std::string> names;
    std::optional<std::wstring> similar;
    std::optional<int> kind;
    std::optional<std::wstring> unknown_type;

    [[nodiscard]] bool empty() const noexcept {
        return text.empty() && tags.empty() && not_tags.empty() && dirs.empty() && names.empty() && !similar && !kind && !unknown_type;
    }
};

inline bool looks_like_path(std::wstring_view s) {
    return (s.size() >= 2 && s[1] == L':' && iswalpha(s[0])) || s.starts_with(L"\\\\");
}

inline std::string tag_key(std::wstring_view t) {
    return to_utf8(lower(trim(t)));
}

inline ParsedQuery parse_query(std::wstring_view q) {
    ParsedQuery out;
    std::vector<std::wstring> words;
    size_t i = 0;
    auto read_value = [&](size_t from) {
        std::wstring v;
        if (from < q.size() && q[from] == L'"') {
            const size_t end = q.find(L'"', from + 1);
            v = std::wstring{q.substr(from + 1, (end == std::wstring_view::npos ? q.size() : end) - from - 1)};
            i = end == std::wstring_view::npos ? q.size() : end + 1;
        } else {
            size_t end = from;
            while (end < q.size() && !iswspace(q[end])) ++end;
            v = std::wstring{q.substr(from, end - from)};
            i = end;
        }
        return v;
    };
    while (i < q.size()) {
        while (i < q.size() && iswspace(q[i])) ++i;
        if (i >= q.size()) break;
        const std::wstring_view rest = q.substr(i);
        auto starts = [&](std::wstring_view p) { return rest.size() >= p.size() && _wcsnicmp(rest.data(), p.data(), p.size()) == 0; };

        if (starts(L"similar:")) {
            std::wstring v = trim(q.substr(i + 8));
            if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') v = v.substr(1, v.size() - 2);
            out.similar = v;
            break;
        }
        if (starts(L"-tag:") || starts(L"!tag:")) {
            if (auto v = read_value(i + 5); !v.empty()) out.not_tags.push_back(tag_key(v));
            continue;
        }
        if (starts(L"tag:")) {
            if (auto v = read_value(i + 4); !v.empty()) out.tags.push_back(tag_key(v));
            continue;
        }
        if (starts(L"name:")) {
            if (auto v = read_value(i + 5); !v.empty()) out.names.push_back(to_utf8(v));
            continue;
        }
        if (starts(L"type:")) {
            const std::wstring v = lower(read_value(i + 5));
            if (v.starts_with(L"im") || v.starts_with(L"ph") || v.starts_with(L"pic")) out.kind = 0;
            else if (v.starts_with(L"vid")) out.kind = 1;
            else if (!v.empty()) out.unknown_type = v;
            continue;
        }
        if (starts(L"dir:") || starts(L"in:")) {
            const size_t off = starts(L"dir:") ? 4 : 3;
            // Paths often contain spaces: take everything up to the next modifier
            std::wstring v;
            if (i + off < q.size() && q[i + off] == L'"') {
                v = read_value(i + off);
            } else {
                size_t end = q.find(L" tag:", i + off);
                end = std::min(end, q.find(L" -tag:", i + off));
                end = std::min(end, q.find(L" name:", i + off));
                v = std::wstring{q.substr(i + off, end == std::wstring_view::npos ? std::wstring_view::npos : end - i - off)};
                i = end == std::wstring_view::npos ? q.size() : end;
            }
            if (auto d = normalise_dir(v); !d.empty()) out.dirs.push_back(to_utf8(d));
            continue;
        }
        if (looks_like_path(rest)) {
            out.dirs.push_back(to_utf8(normalise_dir(rest)));
            break;
        }
        words.push_back(read_value(i));
    }
    for (size_t w = 0; w < words.size(); ++w) {
        if (w) out.text += L' ';
        out.text += words[w];
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Text tower (ONNX Runtime, CPU)
// ---------------------------------------------------------------------------------------------

class TextEncoder {
public:
    enum class State : uint8_t { Idle, Exporting, Loading, Ready, Failed };
    std::function<void()> on_state_changed;

    ~TextEncoder() { stop(); }

    void stop() noexcept {
        stopping_.store(true, std::memory_order_release);
        if (stop_evt_) SetEvent(stop_evt_.get());
        loader_ = {};
    }

    /// Loads the text tower in the background. A request made while another load is running replaces it
    /// as soon as that one finishes; asking again for a model that failed retries it.
    void load_async(fs::path model_dir, std::string model_name) {
        if (stopping_.load(std::memory_order_acquire)) return;
        {
            const std::lock_guard lk{m_};
            if (stopping_.load(std::memory_order_acquire)) return;
            const bool same = model_name == name_ && model_dir == dir_;
            if (same && (busy_ || state_ == State::Ready)) return;
            name_ = std::move(model_name);
            dir_ = std::move(model_dir);
            ++wanted_;
            if (busy_) return; // the running loader picks the new request up
            busy_ = true;
            state_ = State::Loading;
        }
        notify();
        loader_ = std::jthread([this] { load_loop(); });
    }

    /// Tries a failed load again (after an index run, a model download or a Python install).
    void retry() {
        fs::path dir;
        std::string name;
        {
            const std::lock_guard lk{m_};
            if (busy_ || state_ != State::Failed) return;
            dir = dir_;
            name = name_;
        }
        load_async(std::move(dir), std::move(name));
    }

    /// Cheap enough to poll: retries a failed load once the model files on disk differ from the failed
    /// attempt and have stopped changing (so a file that is still being copied is left alone).
    void retry_if_files_changed() {
        fs::path dir;
        std::string name;
        {
            const std::lock_guard lk{m_};
            if (busy_ || state_ != State::Failed) return;
            dir = dir_;
            name = name_;
        }
        const FileSignature now = file_signature(dir, name);
        {
            const std::lock_guard lk{m_};
            const bool settled = now == seen_files_;
            seen_files_ = now;
            if (!settled || now == failed_files_) return;
        }
        load_async(std::move(dir), std::move(name));
    }

    /// Lock-free: drawn every frame by the UI thread.
    [[nodiscard]] State state() const noexcept { return state_.load(); }
    [[nodiscard]] std::wstring error() const {
        const std::lock_guard lk{m_};
        return error_;
    }

    /// Blocks until a pending load has finished, then runs the model. The lock is only held around the
    /// bookkeeping, never around inference, so `state()` / `error()` stay responsive while a query is encoded.
    std::optional<std::vector<float>> encode(const std::wstring& text, std::wstring* err = nullptr) {
        std::shared_ptr<const Model> model;
        {
            std::unique_lock lk{m_};
            cv_.wait(lk, [&] { return !busy_; });
            if (state_ != State::Ready || !model_) {
                if (err) *err = error_.empty() ? L"Text model not loaded" : error_;
                return std::nullopt;
            }
            for (auto it = cache_.begin(); it != cache_.end(); ++it) {
                if (it->first == text) {
                    cache_.splice(cache_.begin(), cache_, it);
                    return cache_.front().second;
                }
            }
            model = model_;
        }

        std::vector<int64_t> ids(model->ctx, 0);
        const std::string u = to_utf8(lower(text));
        if (usy_tok_encode(model->tok, ustr(u), ids.data()) == 0) {
            if (err) *err = L"Tokenizer failed";
            return std::nullopt;
        }
        try {
            const auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            const std::array<int64_t, 2> shape = {1, static_cast<int64_t>(model->ctx)};
            Ort::Value input = Ort::Value::CreateTensor<int64_t>(mem, ids.data(), ids.size(), shape.data(), shape.size());
            const char* in_names[] = {model->in_name.c_str()};
            const char* out_names[] = {model->out_name.c_str()};
            auto out = model->session->Run(Ort::RunOptions{nullptr}, in_names, &input, 1, out_names, 1);
            const float* p = out[0].GetTensorData<float>();
            const size_t n = out[0].GetTensorTypeAndShapeInfo().GetElementCount();
            std::vector<float> v(p, p + n);

            const std::lock_guard lk{m_};
            if (model == model_) { // not replaced by another model in the meantime
                cache_.emplace_front(text, v);
                if (cache_.size() > 64) cache_.pop_back();
            }
            return v;
        } catch (const std::exception& e) {
            if (err) *err = to_wide(e.what());
            return std::nullopt;
        }
    }

private:
    struct Model {
        std::unique_ptr<Ort::Session> session;
        UsyTokenizer* tok = nullptr;
        size_t ctx = 0;
        std::string in_name, out_name;

        Model() = default;
        Model(const Model&) = delete;
        Model& operator=(const Model&) = delete;
        Model(Model&&) = delete;
        Model& operator=(Model&&) = delete;
        ~Model() {
            if (tok) usy_tok_free(tok);
        }
    };

    struct ModelPaths {
        fs::path weights, onnx, tokenizer, shared_tokenizer;
    };
    static ModelPaths model_paths(const fs::path& dir, const std::string& name) {
        const std::wstring w = to_wide(name);
        return {dir / (w + L".safetensors"), dir / (w + L"-text.onnx"), dir / (w + L"-tokenizer.json"), dir / L"tokenizer.json"};
    }

    /// Size and modification time of every file a load depends on (0 = missing).
    using FileSignature = std::array<uint64_t, 8>;
    static FileSignature file_signature(const fs::path& dir, const std::string& name) {
        const ModelPaths p = model_paths(dir, name);
        FileSignature sig{};
        size_t i = 0;
        for (const fs::path* f : {&p.weights, &p.onnx, &p.tokenizer, &p.shared_tokenizer}) {
            WIN32_FILE_ATTRIBUTE_DATA d{};
            if (GetFileAttributesExW(f->c_str(), GetFileExInfoStandard, &d)) {
                sig[i] = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
                sig[i + 1] = (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
            }
            i += 2;
        }
        return sig;
    }

    void notify() const {
        if (on_state_changed) on_state_changed();
    }

    void load_loop() {
        for (;;) {
            if (stopping_.load(std::memory_order_acquire)) break;
            fs::path dir;
            std::string name;
            uint64_t generation = 0;
            {
                const std::lock_guard lk{m_};
                dir = dir_;
                name = name_;
                generation = wanted_;
                state_ = State::Loading;
            }
            notify();
            const FileSignature files = file_signature(dir, name);

            std::shared_ptr<const Model> model;
            std::wstring error;
            try {
                model = load_model(dir, name);
            } catch (const std::exception& e) {
                error = to_wide(e.what());
            }

            std::shared_ptr<const Model> previous; // released after the lock
            {
                const std::lock_guard lk{m_};
                if (stopping_.load(std::memory_order_acquire)) {
                    busy_ = false;
                    break;
                }
                if (generation != wanted_) continue; // another model was requested while this one loaded
                previous = std::exchange(model_, std::move(model));
                cache_.clear();
                error_ = std::move(error);
                failed_files_ = seen_files_ = files;
                state_ = model_ ? State::Ready : State::Failed;
                busy_ = false;
            }
            break;
        }
        cv_.notify_all();
        if (!stopping_.load(std::memory_order_acquire)) notify();
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
    }

    std::shared_ptr<const Model> load_model(const fs::path& model_dir, const std::string& model_name) {
        const ModelPaths paths = model_paths(model_dir, model_name);
        std::error_code ec;
        const fs::path& tok = !fs::exists(paths.tokenizer, ec) && fs::exists(paths.shared_tokenizer, ec) ? paths.shared_tokenizer : paths.tokenizer;
        if (!fs::exists(paths.onnx, ec)) {
            if (!fs::exists(paths.weights, ec)) {
                throw std::runtime_error("Model weights missing (" + to_utf8(paths.weights.filename().wstring()) + ") - download the model in Settings");
            }
            state_ = State::Exporting;
            notify();

            const fs::path idx_exe = exe_dir() / L"usyseek-indexer.exe";
            std::wstring cmd = L"\"" + idx_exe.wstring() + L"\" --export-text";
            STARTUPINFOW si{sizeof(si)};
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            wil::unique_handle job = make_kill_on_close_job();
            wil::unique_process_information pi;
            if (CreateProcessW(idx_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, exe_dir().c_str(), &si, &pi)) {
                if (job) AssignProcessToJobObject(job.get(), pi.hProcess);
                ResumeThread(pi.hThread);
                HANDLE waits[2]{pi.hProcess, stop_evt_.get()};
                const DWORD n = stop_evt_ ? 2u : 1u;
                WaitForMultipleObjects(n, waits, FALSE, 180'000);
            }
            if (stopping_.load(std::memory_order_acquire)) {
                throw std::runtime_error("Model loading cancelled");
            }
            state_ = State::Loading;
            notify();
            if (!fs::exists(paths.onnx, ec)) {
                throw std::runtime_error("Text ONNX export failed - check the Python environment in Settings");
            }
        }
        if (!fs::exists(tok, ec)) {
            throw std::runtime_error("Tokenizer missing (" + to_utf8(tok.filename().wstring()) + ") - download the model in Settings");
        }

        if (!env_) env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "usyseek");
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(static_cast<int>(std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u)));
        so.SetInterOpNumThreads(1);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        so.DisableCpuMemArena();

        auto model = std::make_shared<Model>();
        model->session = std::make_unique<Ort::Session>(*env_, paths.onnx.c_str(), so);
        const auto in_shape = model->session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        model->ctx = in_shape.size() >= 2 && in_shape[1] > 0 ? static_cast<size_t>(in_shape[1]) : 77;
        Ort::AllocatorWithDefaultOptions alloc;
        model->in_name = model->session->GetInputNameAllocated(0, alloc).get();
        model->out_name = model->session->GetOutputNameAllocated(0, alloc).get();

        RustBuf terr;
        const std::string tok_path = to_utf8(tok.wstring());
        model->tok = usy_tok_load(ustr(tok_path), model->ctx, &terr.b);
        if (!model->tok) throw std::runtime_error("Tokenizer failed to load: " + terr.str());
        return model;
    }

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> stopping_{false};
    wil::unique_event_nothrow stop_evt_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    bool busy_ = false;   // a loader thread is running
    uint64_t wanted_ = 0; // bumped by every request, so a loader can tell it was superseded
    std::wstring error_;
    std::string name_;
    fs::path dir_;
    FileSignature failed_files_{}, seen_files_{};
    std::unique_ptr<Ort::Env> env_; // loader thread only
    std::shared_ptr<const Model> model_;
    std::list<std::pair<std::wstring, std::vector<float>>> cache_;
    std::jthread loader_;
};

// ---------------------------------------------------------------------------------------------
// Search worker
// ---------------------------------------------------------------------------------------------

struct SearchRequest {
    uint64_t seq = 0;
    std::wstring query;
    int kind = -1;       // chip filter: -1 all, 0 images, 1 videos
    uint64_t offset = 0; // > 0 = next page
};

struct SearchResult {
    uint64_t seq = 0;
    bool append = false;
    bool semantic = false;
    bool has_more = false;
    uint64_t next_offset = 0;
    double ms = 0.0;
    std::vector<DbPoint> points;
    std::wstring error;
};

inline constexpr uint64_t PAGE = 90;

/// One background thread for everything touching the database or the text model, so the UI
/// thread never blocks. Searches are coalesced: only the newest pending request runs.
class Engine {
public:
    std::function<void(std::unique_ptr<SearchResult>)> on_result;
    std::function<void(std::function<void()>)> post_ui; // marshal a callback to the UI thread
    std::function<void(bool ready, std::string error)> on_db_state;
    std::function<void(std::string title, std::string detail)> on_job_dropped;

    void start(const Settings& s) {
        settings_ = s;
        db_want_event_.reset(CreateEventW(nullptr, TRUE, FALSE, INDEXER_DB_WANT_EVENT));
        arm_db_want_wait();
        thread_ = std::jthread([this] { run(); });
    }

    void stop() noexcept {
        disarm_db_want_wait();
        {
            std::lock_guard lk{m_};
            stopping_ = true;
        }
        cv_.notify_all();
        thread_ = {};
        encoder_.stop();
    }

    void set_indexing_active(bool active) noexcept {
        {
            std::lock_guard lk{m_};
            indexing_active_hint_ = active;
        }
        cv_.notify_one();
    }

    void search(SearchRequest r) noexcept {
        {
            std::lock_guard lk{m_};
            pending_search_ = std::move(r);
        }
        cv_.notify_one();
    }

    void post(std::function<void(Db&)> job) {
        {
            std::lock_guard lk{m_};
            jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

    /// Runs `job` as soon as the database is open (waiting through an indexing pause if needed).
    void post_when_ready(std::function<void(Db&)> job) {
        {
            std::lock_guard lk{m_};
            deferred_jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

    void set_settings(const Settings& s) {
        std::lock_guard lk{m_};
        settings_ = s;
    }

    TextEncoder& encoder() noexcept { return encoder_; }
    [[nodiscard]] bool db_ready() const noexcept { return db_ready_.load(); }
    [[nodiscard]] bool db_paused_for_indexing() const noexcept { return db_paused_for_indexing_.load(); }

    [[nodiscard]] std::string collection() const {
        std::lock_guard lk{m_};
        return settings_.model_name;
    }

private:
    static void CALLBACK on_db_want(PVOID ctx, BOOLEAN /*timed_out*/) noexcept {
        static_cast<Engine*>(ctx)->set_indexing_active(true);
    }

    void arm_db_want_wait() noexcept {
        if (!db_want_event_) return;
        std::lock_guard lk{wait_mx_};
        if (wait_handle_) {
            UnregisterWaitEx(wait_handle_, nullptr);
            wait_handle_ = nullptr;
        }
        RegisterWaitForSingleObject(&wait_handle_, db_want_event_.get(), &Engine::on_db_want, this, INFINITE, WT_EXECUTEONLYONCE);
    }

    void disarm_db_want_wait() noexcept {
        std::lock_guard lk{wait_mx_};
        if (wait_handle_) {
            UnregisterWaitEx(wait_handle_, INVALID_HANDLE_VALUE);
            wait_handle_ = nullptr;
        }
    }

    void run() {
        ulp::win::COMWrapper<false> com{COINIT_MULTITHREADED};
        Settings s;
        {
            std::lock_guard lk{m_};
            s = settings_;
        }

        auto open_db = [&] {
            std::string err;
            if (db_.open(edge_data_dir(), &err)) {
                db_paused_for_indexing_.store(false, std::memory_order_release);
                db_ready_.store(true, std::memory_order_release);
                arm_db_want_wait();
                if (on_db_state) on_db_state(true, {});
            } else {
                db_paused_for_indexing_.store(false, std::memory_order_release);
                db_ready_.store(false, std::memory_order_release);
                if (on_db_state) on_db_state(false, err);
            }
        };
        auto pause_db = [&] {
            const bool already_paused = db_paused_for_indexing_.load(std::memory_order_acquire) && !db_.opened();
            std::string err;
            (void)db_.close(&err);
            db_ready_.store(false, std::memory_order_release);
            db_paused_for_indexing_.store(true, std::memory_order_release);
            if (!already_paused && on_db_state) on_db_state(false, {});
        };

        if (is_indexer_db_wanted()) {
            pause_db();
        } else {
            open_db();
        }

        for (;;) {
            std::optional<SearchRequest> req;
            std::function<void(Db&)> job;
            bool need_pause = false;
            bool need_resume = false;
            {
                std::unique_lock lk{m_};
                const auto wake = [&] {
                    if (stopping_) return true;
                    const bool want = indexing_active_hint_ || is_indexer_db_wanted();
                    if (want && !db_paused_for_indexing_.load(std::memory_order_acquire)) return true;
                    if (!want && db_paused_for_indexing_.load(std::memory_order_acquire)) return true;
                    if (db_ready_.load(std::memory_order_acquire) && !deferred_jobs_.empty()) return true;
                    return pending_search_.has_value() || !jobs_.empty();
                };
                if (db_paused_for_indexing_.load(std::memory_order_acquire)) {
                    cv_.wait_for(lk, std::chrono::milliseconds(250), wake);
                } else {
                    cv_.wait(lk, wake);
                }
                if (stopping_) return;

                const bool want = indexing_active_hint_ || is_indexer_db_wanted();
                if (want) {
                    if (!db_paused_for_indexing_.load(std::memory_order_acquire) || db_.opened()) need_pause = true;
                } else if (db_paused_for_indexing_.load(std::memory_order_acquire)) {
                    indexing_active_hint_ = false;
                    need_resume = true;
                }

                if (!need_pause && !need_resume) {
                    if (!jobs_.empty()) {
                        job = std::move(jobs_.front());
                        jobs_.pop_front();
                    } else if (db_ready_.load(std::memory_order_acquire) && !deferred_jobs_.empty()) {
                        job = std::move(deferred_jobs_.front());
                        deferred_jobs_.pop_front();
                    } else if (pending_search_) {
                        req = std::move(pending_search_);
                        pending_search_.reset();
                    }
                }
                s = settings_;
            }

            if (need_pause) {
                pause_db();
                continue;
            }
            if (need_resume) {
                if (!is_indexer_db_wanted()) open_db();
                continue;
            }
            if (job) {
                if (db_ready_) {
                    job(db_);
                } else if (on_job_dropped) {
                    if (db_paused_for_indexing_) {
                        on_job_dropped("Database unavailable while indexing", "Database updates are paused while indexing is in progress.");
                    } else {
                        on_job_dropped("Database unavailable", "Queued database update could not run because the database is closed.");
                    }
                }
                continue;
            }
            if (req && on_result) on_result(execute(*req, s));
        }
    }

    std::unique_ptr<SearchResult> execute(const SearchRequest& r, const Settings& s) {
        auto res = std::make_unique<SearchResult>();
        res->seq = r.seq;
        res->append = r.offset > 0;
        const auto t0 = std::chrono::steady_clock::now();
        if (!db_ready_) {
            res->error = db_paused_for_indexing_ ? L"Database unavailable while indexing" : L"Database unavailable";
            return res;
        }

        ParsedQuery q = parse_query(r.query);
        if (q.unknown_type) {
            res->error = L"Unknown type filter 'type:" + *q.unknown_type + L"' (use image or video)";
            return res;
        }
        if (q.empty()) return res;

        DbFilter f;
        if (q.kind) f.kind = *q.kind;
        else if (r.kind >= 0) f.kind = r.kind;
        f.tags_all = q.tags;
        f.tags_none = q.not_tags;
        f.dirs = q.dirs;
        f.names = q.names;
        const bool has_filter = f.kind || !f.tags_all.empty() || !f.tags_none.empty() || !f.dirs.empty() || !f.names.empty();
        const std::vector<std::string> fields = {"p", "n", "k", "m", "s", "w", "h", "d", "t"};
        const std::string& coll = s.model_name;
        std::string err;

        if (q.similar || !q.text.empty()) {
            res->semantic = true;
            QueryRequest qr;
            qr.limit = PAGE;
            qr.offset = r.offset;
            qr.fields = fields;
            if (has_filter) qr.filter = f;
            std::optional<DbPoints> pts;
            uint64_t self_id = 0;
            if (q.similar) {
                self_id = path_id(*q.similar);
                if (db_.retrieve(coll, std::span{&self_id, 1}, {"k"}).value_or(DbPoints{}).points.empty()) {
                    res->error = L"Not indexed yet";
                    return res;
                }
                qr.id = self_id;
                pts = db_.query(coll, {}, qr, &err);
            } else {
                std::wstring enc_err;
                auto v = encoder_.encode(q.text, &enc_err);
                if (!v) {
                    res->error = enc_err;
                    return res;
                }
                pts = db_.query(coll, *v, qr, &err);
            }
            if (!pts) {
                res->error = to_wide(err);
                return res;
            }
            const uint64_t raw_count = pts->points.size();
            res->has_more = raw_count >= PAGE;
            res->next_offset = r.offset + raw_count;
            res->points = std::move(pts->points);
            if (self_id) std::erase_if(res->points, [&](const DbPoint& p) { return p.id == self_id; });
        } else {
            // Filter-only queries: newest first
            ScrollRequest sr;
            sr.limit = r.offset + PAGE;
            sr.order_mtime = true;
            sr.fields = fields;
            sr.filter = f;
            auto pts = db_.scroll(coll, sr, &err);
            if (!pts) {
                res->error = to_wide(err);
                return res;
            }
            res->has_more = pts->points.size() >= sr.limit;
            res->next_offset = pts->points.size();
            if (r.offset < pts->points.size()) {
                res->points.assign(std::make_move_iterator(pts->points.begin() + static_cast<ptrdiff_t>(r.offset)), std::make_move_iterator(pts->points.end()));
            }
        }
        res->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return res;
    }

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::optional<SearchRequest> pending_search_;
    std::deque<std::function<void(Db&)>> jobs_;
    std::deque<std::function<void(Db&)>> deferred_jobs_;
    bool stopping_ = false;
    bool indexing_active_hint_ = false;
    Settings settings_;
    Db db_;
    std::atomic<bool> db_ready_{false};
    std::atomic<bool> db_paused_for_indexing_{false};
    std::mutex wait_mx_;
    wil::unique_event_nothrow db_want_event_;
    HANDLE wait_handle_ = nullptr;
    TextEncoder encoder_;
    std::jthread thread_;
};

} // namespace usyseek::ui
