#pragma once

#include <usyseek/db.hpp>

#include <onnxruntime_cxx_api.h>

#include <condition_variable>
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

    [[nodiscard]] bool empty() const {
        return text.empty() && tags.empty() && not_tags.empty() && dirs.empty() && names.empty() && !similar;
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
    enum class State { Idle, Exporting, Loading, Ready, Failed };
    std::function<void()> on_state_changed;

    ~TextEncoder() {
        if (tok_) usy_tok_free(tok_);
    }

    void load_async(fs::path model_dir, std::string model_name) {
        {
            std::lock_guard lk{m_};
            if (state_ == State::Loading || state_ == State::Exporting ||
                (state_ == State::Ready && model_name == name_ && model_dir == dir_)) return;
            state_ = State::Loading;
            name_ = model_name;
            dir_ = model_dir;
        }
        if (on_state_changed) on_state_changed();
        loader_ = std::jthread([this, model_dir = std::move(model_dir), model_name = std::move(model_name)] {
            try {
                const fs::path weights = model_dir / (to_wide(model_name) + L".safetensors");
                const fs::path onnx = model_dir / (to_wide(model_name) + L"-text.onnx");
                fs::path tok = model_dir / (to_wide(model_name) + L"-tokenizer.json");
                std::error_code ec;
                if (!fs::exists(tok, ec)) {
                    const fs::path alt = model_dir / L"tokenizer.json";
                    if (fs::exists(alt, ec)) tok = alt;
                }
                if (!fs::exists(onnx, ec)) {
                    if (!fs::exists(weights, ec)) {
                        throw std::runtime_error("Model weights missing (" + to_utf8(weights.filename().wstring()) + ")");
                    }
                    {
                        std::lock_guard lk{m_};
                        state_ = State::Exporting;
                    }
                    if (on_state_changed) on_state_changed();

                    const fs::path idx_exe = exe_dir() / L"usyseek-indexer.exe";
                    std::wstring cmd = L"\"" + idx_exe.wstring() + L"\" --export-text";
                    STARTUPINFOW si{sizeof(si)};
                    wil::unique_process_information pi;
                    if (CreateProcessW(idx_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, exe_dir().c_str(), &si, &pi)) {
                        WaitForSingleObject(pi.hProcess, 180'000);
                    }
                    {
                        std::lock_guard lk{m_};
                        state_ = State::Loading;
                    }
                    if (on_state_changed) on_state_changed();
                    if (!fs::exists(onnx, ec)) {
                        throw std::runtime_error("Text ONNX export failed - check Python .venv in Settings");
                    }
                }
                if (!fs::exists(tok, ec)) throw std::runtime_error("Tokenizer missing (" + to_utf8(tok.filename().wstring()) + ")");

                if (!env_) env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "usyseek");
                Ort::SessionOptions so;
                so.SetIntraOpNumThreads(static_cast<int>(std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u)));
                so.SetInterOpNumThreads(1);
                so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
                so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
                so.DisableCpuMemArena();
                auto session = std::make_unique<Ort::Session>(*env_, onnx.c_str(), so);

                const auto in_shape = session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
                const auto out_shape = session->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
                const size_t ctx = in_shape.size() >= 2 && in_shape[1] > 0 ? static_cast<size_t>(in_shape[1]) : 77;
                const size_t dim = out_shape.empty() ? 0 : static_cast<size_t>(out_shape.back());
                Ort::AllocatorWithDefaultOptions alloc;
                const std::string in_name = session->GetInputNameAllocated(0, alloc).get();
                const std::string out_name = session->GetOutputNameAllocated(0, alloc).get();

                RustBuf terr;
                const std::string tok_path = to_utf8(tok.wstring());
                UsyTokenizer* t = usy_tok_load(ustr(tok_path), ctx, &terr.b);
                if (!t) throw std::runtime_error("Tokenizer failed to load: " + terr.str());

                std::lock_guard lk{m_};
                if (tok_) usy_tok_free(tok_);
                tok_ = t;
                session_ = std::move(session);
                ctx_ = ctx;
                dim_ = dim;
                in_name_ = in_name;
                out_name_ = out_name;
                cache_.clear();
                error_.clear();
                state_ = State::Ready;
            } catch (const std::exception& e) {
                std::lock_guard lk{m_};
                error_ = to_wide(e.what());
                state_ = State::Failed;
            }
            cv_.notify_all();
            if (on_state_changed) on_state_changed();
            SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
        });
    }

    [[nodiscard]] State state() const {
        std::lock_guard lk{m_};
        return state_;
    }
    [[nodiscard]] std::wstring error() const {
        std::lock_guard lk{m_};
        return error_;
    }

    /// Blocks until the model is ready (or failed).
    std::optional<std::vector<float>> encode(const std::wstring& text, std::wstring* err = nullptr) {
        std::unique_lock lk{m_};
        cv_.wait(lk, [&] { return state_ == State::Ready || state_ == State::Failed || state_ == State::Idle; });
        if (state_ != State::Ready) {
            if (err) *err = error_.empty() ? L"Text model not loaded" : error_;
            return std::nullopt;
        }
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->first == text) {
                cache_.splice(cache_.begin(), cache_, it);
                return cache_.front().second;
            }
        }
        std::vector<int64_t> ids(ctx_, 0);
        const std::string u = to_utf8(lower(text));
        if (usy_tok_encode(tok_, ustr(u), ids.data()) == 0) {
            if (err) *err = L"Tokenizer failed";
            return std::nullopt;
        }
        try {
            const auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            const int64_t shape[2] = {1, static_cast<int64_t>(ctx_)};
            Ort::Value input = Ort::Value::CreateTensor<int64_t>(mem, ids.data(), ids.size(), shape, 2);
            const char* in_names[] = {in_name_.c_str()};
            const char* out_names[] = {out_name_.c_str()};
            auto out = session_->Run(Ort::RunOptions{nullptr}, in_names, &input, 1, out_names, 1);
            const float* p = out[0].GetTensorData<float>();
            const size_t n = out[0].GetTensorTypeAndShapeInfo().GetElementCount();
            std::vector<float> v(p, p + n);
            cache_.emplace_front(text, v);
            if (cache_.size() > 64) cache_.pop_back();
            return v;
        } catch (const std::exception& e) {
            if (err) *err = to_wide(e.what());
            return std::nullopt;
        }
    }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    State state_ = State::Idle;
    std::wstring error_;
    std::string name_;
    fs::path dir_;
    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;
    UsyTokenizer* tok_ = nullptr;
    size_t ctx_ = 0, dim_ = 0;
    std::string in_name_, out_name_;
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

    void start(const Settings& s) {
        settings_ = s;
        thread_ = std::jthread([this](std::stop_token st) { run(st); });
    }

    void stop() {
        {
            std::lock_guard lk{m_};
            stopping_ = true;
        }
        cv_.notify_all();
        thread_ = {};
    }

    void search(SearchRequest r) {
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

    void set_settings(const Settings& s) {
        std::lock_guard lk{m_};
        settings_ = s;
    }

    TextEncoder& encoder() { return encoder_; }
    [[nodiscard]] bool db_ready() const noexcept { return db_ready_.load(); }

    [[nodiscard]] std::string collection() const {
        std::lock_guard lk{m_};
        return settings_.model_name;
    }

private:
    void run(std::stop_token) {
        ulp::win::COMWrapper<false> com{COINIT_MULTITHREADED};
        Settings s;
        {
            std::lock_guard lk{m_};
            s = settings_;
        }
        if (db_.connect(s.qdrant_grpc_port)) {
            send_to_launcher(Cmd::NeedQdrant);
            db_ready_ = db_.wait_ready(std::chrono::seconds(30));
        }
        for (;;) {
            std::optional<SearchRequest> req;
            std::function<void(Db&)> job;
            {
                std::unique_lock lk{m_};
                cv_.wait(lk, [&] { return stopping_ || pending_search_ || !jobs_.empty(); });
                if (stopping_) return;
                if (!jobs_.empty()) {
                    job = std::move(jobs_.front());
                    jobs_.pop_front();
                } else {
                    req = std::move(pending_search_);
                    pending_search_.reset();
                }
                s = settings_;
            }
            if (job) {
                if (db_ready_) job(db_);
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
            res->error = L"Database unavailable";
            return res;
        }

        ParsedQuery q = parse_query(r.query);
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
                if (!db_.retrieve(coll, std::span{&self_id, 1}, {"k"}).value_or(DbPoints{}).points.size()) {
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
            res->has_more = pts->points.size() >= PAGE;
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
    bool stopping_ = false;
    Settings settings_;
    Db db_;
    std::atomic<bool> db_ready_{false};
    TextEncoder encoder_;
    std::jthread thread_;
};

} // namespace usyseek::ui
