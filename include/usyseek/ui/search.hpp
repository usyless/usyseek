#pragma once

#include <usyseek/db.hpp>
#include <usyseek/ui/query.hpp>

#include <onnxruntime_cxx_api.h>

#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <list>
#include <mutex>
#include <thread>
#include <utility>

namespace usyseek::ui {

// ---------------------------------------------------------------------------------------------
// Text tower (ONNX Runtime, CPU)
// ---------------------------------------------------------------------------------------------

class TextEncoder {
public:
    enum class State : uint8_t { Idle, Exporting, Loading, Ready, Failed };
    std::function<void()> on_state_changed;

    TextEncoder() = default;
    TextEncoder(const TextEncoder&) = delete;
    TextEncoder& operator=(const TextEncoder&) = delete;
    TextEncoder(TextEncoder&&) = delete;
    TextEncoder& operator=(TextEncoder&&) = delete;
    ~TextEncoder() { stop(); }

    void stop() noexcept {
        stopping_.store(true, std::memory_order_release);
        if (stop_evt_) SetEvent(stop_evt_.get());
        {
            const std::scoped_lock lk{m_};
        }
        cv_.notify_all();
        {
            const std::scoped_lock llk{loader_mx_};
            loader_ = {};
        }
    }

    /// Loads the text tower in the background. A request made while another load is running replaces it
    /// as soon as that one finishes; asking again for a model that failed retries it.
    void load_async(fs::path model_dir, std::string model_name) {
        if (stopping_.load(std::memory_order_acquire)) return;
        {
            const std::scoped_lock lk{m_};
            if (stopping_.load(std::memory_order_acquire)) return;
            const bool same = model_name == name_ && model_dir == dir_;
            if (same && (busy_ || state_.load(std::memory_order_acquire) == State::Ready)) return;
            name_ = std::move(model_name);
            dir_ = std::move(model_dir);
            ++wanted_;
            if (busy_) return; // the running loader picks the new request up
            busy_ = true;
            state_.store(State::Loading, std::memory_order_release);
        }
        notify();
        bool started = false;
        const auto rollback_busy = wil::scope_exit([&] {
            if (!started) {
                const bool stopped = stopping_.load(std::memory_order_acquire);
                {
                    const std::scoped_lock lk{m_};
                    busy_ = false;
                    if (!stopped) {
                        error_ = L"Failed to start text model loader thread";
                        state_.store(State::Failed, std::memory_order_release);
                    }
                }
                cv_.notify_all();
                if (!stopped) notify();
            }
        });
        const std::scoped_lock llk{loader_mx_};
        if (stopping_.load(std::memory_order_acquire)) return;
        loader_ = std::jthread([this] { load_loop(); });
        started = true;
    }

    /// Tries a failed load again (after an index run, a model download or a Python install).
    void retry() {
        fs::path dir;
        std::string name;
        {
            const std::scoped_lock lk{m_};
            if (busy_ || state_.load(std::memory_order_acquire) != State::Failed) return;
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
            const std::scoped_lock lk{m_};
            if (busy_ || state_.load(std::memory_order_acquire) != State::Failed) return;
            dir = dir_;
            name = name_;
        }
        const FileSignature now = file_signature(dir, name);
        {
            const std::scoped_lock lk{m_};
            if (busy_ || state_.load(std::memory_order_acquire) != State::Failed || dir != dir_ || name != name_) return;
            const bool settled = now == seen_files_;
            seen_files_ = now;
            if (!settled || now == failed_files_) return;
        }
        load_async(std::move(dir), std::move(name));
    }

    /// Lock-free: drawn every frame by the UI thread.
    [[nodiscard]] State state() const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] std::wstring error() const {
        const std::scoped_lock lk{m_};
        return error_;
    }

    /// Blocks until a pending load has finished, then runs the model. The lock is only held around the
    /// bookkeeping, never around inference, so `state()` / `error()` stay responsive while a query is encoded.
    std::optional<std::vector<float>> encode(const std::wstring& text, std::wstring* err = nullptr,
                                             const std::function<bool()>& should_abort = {}) {
        std::shared_ptr<const Model> model;
        {
            std::unique_lock lk{m_};
            while (busy_ && !stopping_.load(std::memory_order_acquire)) {
                if (should_abort && should_abort()) {
                    if (err) *err = L"Database unavailable while indexing";
                    return std::nullopt;
                }
                cv_.wait_for(lk, std::chrono::milliseconds(50));
            }
            if (stopping_.load(std::memory_order_acquire)) {
                if (err) *err = L"Model loading cancelled";
                return std::nullopt;
            }
            if (state_.load(std::memory_order_acquire) != State::Ready || !model_) {
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

        try {
            std::array<int64_t, 256> stack_ids; // NOLINT(cppcoreguidelines-pro-type-member-init)
            std::vector<int64_t> heap_ids;
            int64_t* ids_ptr = stack_ids.data();
            if (model->ctx > stack_ids.size()) {
                heap_ids.assign(model->ctx, 0);
                ids_ptr = heap_ids.data();
            }
            const std::string u = to_utf8(lower(text));
            if (usy_tok_encode(model->tok, ustr(u), ids_ptr) != model->ctx) {
                if (err) *err = L"Tokenizer failed";
                return std::nullopt;
            }

            const auto mem = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);
            const std::array<int64_t, 2> shape = {1, static_cast<int64_t>(model->ctx)};

            std::array<int32_t, 256> stack_ids32; // NOLINT(cppcoreguidelines-pro-type-member-init)
            std::vector<int32_t> heap_ids32;
            Ort::Value input{nullptr};
            if (model->in_elem_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32) {
                int32_t* ids32_ptr = stack_ids32.data();
                if (model->ctx > stack_ids32.size()) {
                    heap_ids32.resize(model->ctx);
                    ids32_ptr = heap_ids32.data();
                }
                for (size_t k = 0; k < model->ctx; ++k) {
                    if (ids_ptr[k] < std::numeric_limits<int32_t>::min() || ids_ptr[k] > std::numeric_limits<int32_t>::max()) {
                        if (err) *err = L"Token ID out of int32 range";
                        return std::nullopt;
                    }
                    ids32_ptr[k] = static_cast<int32_t>(ids_ptr[k]);
                }
                input = Ort::Value::CreateTensor<int32_t>(mem, ids32_ptr, model->ctx, shape.data(), shape.size());
            } else {
                input = Ort::Value::CreateTensor<int64_t>(mem, ids_ptr, model->ctx, shape.data(), shape.size());
            }

            const std::array<const char*, 1> in_names = {model->in_name.c_str()};
            const std::array<const char*, 1> out_names = {model->out_name.c_str()};
            auto out = model->session->Run(Ort::RunOptions{nullptr}, in_names.data(), &input, 1, out_names.data(), 1);
            if (out.empty() || !out[0].IsTensor()) {
                if (err) *err = L"Text model returned a non-tensor output";
                return std::nullopt;
            }
            const auto info = out[0].GetTensorTypeAndShapeInfo();
            if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                if (err) *err = L"Text model output is not float32";
                return std::nullopt;
            }
            const size_t n = info.GetElementCount();
            const auto* p = out[0].GetTensorData<float>();
            if (n == 0 || !p || n > (std::numeric_limits<size_t>::max() / sizeof(float)) ||
                out[0].GetTensorSizeInBytes() < n * sizeof(float)) {
                if (err) *err = L"Text model returned an invalid embedding tensor";
                return std::nullopt;
            }
            for (size_t k = 0; k < n; ++k) {
                if (!std::isfinite(p[k])) {
                    if (err) *err = L"Text model returned a non-finite embedding";
                    return std::nullopt;
                }
            }
            std::vector<float> v(p, p + n);

            const std::scoped_lock lk{m_};
            if (model == model_) { // not replaced by another model in the meantime
                for (auto it = cache_.begin(); it != cache_.end(); ++it) {
                    if (it->first == text) {
                        it->second = v;
                        cache_.splice(cache_.begin(), cache_, it);
                        return v;
                    }
                }
                cache_.emplace_front(text, v);
                if (cache_.size() > 64) cache_.pop_back();
            }
            return v;
        } catch (const std::exception& e) {
            if (err) *err = to_wide(e.what());
            return std::nullopt;
        } catch (...) {
            if (err) *err = L"Unknown error encoding text query";
            return std::nullopt;
        }
    }

private:
    static constexpr size_t MAX_CONTEXT_LENGTH = 65536;

    struct Model {
        std::shared_ptr<Ort::Env> env; // declared before session so session is always destroyed first
        std::unique_ptr<Ort::Session> session;
        UsyTokenizer* tok = nullptr;
        size_t ctx = 0;
        ONNXTensorElementDataType in_elem_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
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
        fs::path weights, onnx, onnx_data, tokenizer, shared_tokenizer;
    };
    static ModelPaths model_paths(const fs::path& dir, const std::string& name) {
        const std::wstring w = to_wide(name);
        return {
            .weights = dir / ulp::str::concat_strings(w, L".safetensors"),
            .onnx = dir / ulp::str::concat_strings(w, L"-text.onnx"),
            .onnx_data = dir / ulp::str::concat_strings(w, L"-text.onnx.data"),
            .tokenizer = dir / ulp::str::concat_strings(w, L"-tokenizer.json"),
            .shared_tokenizer = dir / L"tokenizer.json",
        };
    }

    /// Size and modification time of every file a load depends on (0 = missing).
    using FileSignature = std::array<uint64_t, 10>;
    static FileSignature file_signature(const fs::path& dir, const std::string& name) {
        const ModelPaths p = model_paths(dir, name);
        FileSignature sig{};
        size_t i = 0;
        for (const fs::path* f : {&p.weights, &p.onnx, &p.onnx_data, &p.tokenizer, &p.shared_tokenizer}) {
            WIN32_FILE_ATTRIBUTE_DATA d{};
            if (GetFileAttributesExW(f->c_str(), GetFileExInfoStandard, &d)) {
                sig[i] = (static_cast<uint64_t>(d.nFileSizeHigh) << 32u) | d.nFileSizeLow;
                sig[i + 1] = (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32u) | d.ftLastWriteTime.dwLowDateTime;
            }
            i += 2;
        }
        return sig;
    }

    void notify() const {
        if (on_state_changed) on_state_changed();
    }

    void load_loop() {
        bool busy_cleared = false;
        const auto finish_busy = wil::scope_exit([&] {
            if (!busy_cleared) {
                {
                    const std::scoped_lock lk{m_};
                    busy_ = false;
                }
                cv_.notify_all();
            }
        });
        for (;;) {
            if (stopping_.load(std::memory_order_acquire)) break;
            fs::path dir;
            std::string name;
            uint64_t generation = 0;
            {
                const std::scoped_lock lk{m_};
                dir = dir_;
                name = name_;
                generation = wanted_;
                state_.store(State::Loading, std::memory_order_release);
            }
            notify();
            const FileSignature files = file_signature(dir, name);

            std::shared_ptr<const Model> model;
            std::wstring error;
            try {
                model = load_model(dir, name);
            } catch (const std::exception& e) {
                error = to_wide(e.what());
            } catch (...) {
                error = L"Unknown error while loading text model";
            }

            std::shared_ptr<const Model> previous; // released after the lock
            {
                const std::scoped_lock lk{m_};
                if (stopping_.load(std::memory_order_acquire)) {
                    busy_ = false;
                    busy_cleared = true;
                    break;
                }
                if (generation != wanted_) continue; // another model was requested while this one loaded
                previous = std::exchange(model_, std::move(model));
                cache_.clear();
                error_ = std::move(error);
                failed_files_ = seen_files_ = files;
                state_.store(model_ ? State::Ready : State::Failed, std::memory_order_release);
                busy_ = false;
                busy_cleared = true;
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
                throw std::runtime_error(concat_utf8("Model weights missing (", paths.weights.filename(), ") - download the model in Settings"));
            }
            state_.store(State::Exporting, std::memory_order_release);
            notify();

            const fs::path idx_exe = exe_dir() / L"usyseek-indexer.exe";
            std::wstring cmd = ulp::str::concat_strings(L"\"", idx_exe.wstring(), L"\" --export-text");
            STARTUPINFOW si{.cb = sizeof(si)};
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            const wil::unique_handle job = make_kill_on_close_job();
            wil::unique_process_information pi;
            bool timed_out = false;
            if (CreateProcessW(idx_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, exe_dir().c_str(), &si, &pi)) {
                if (job) AssignProcessToJobObject(job.get(), pi.hProcess);
                if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
                    TerminateProcess(pi.hProcess, 1);
                } else {
                    std::array<HANDLE, 2> waits{pi.hProcess, stop_evt_.get()};
                    const DWORD n = stop_evt_ ? 2u : 1u;
                    const DWORD wr = WaitForMultipleObjects(n, waits.data(), FALSE, 180'000);
                    if (wr != WAIT_OBJECT_0) {
                        timed_out = wr == WAIT_TIMEOUT;
                        TerminateProcess(pi.hProcess, 1);
                        WaitForSingleObject(pi.hProcess, 2000);
                    }
                }
            }
            if (stopping_.load(std::memory_order_acquire)) {
                throw std::runtime_error("Model loading cancelled");
            }
            state_.store(State::Loading, std::memory_order_release);
            notify();
            if (timed_out && !fs::exists(paths.onnx, ec)) {
                throw std::runtime_error("Text ONNX export timed out - check the Python environment in Settings");
            }
            if (!fs::exists(paths.onnx, ec)) {
                throw std::runtime_error("Text ONNX export failed - check the Python environment in Settings");
            }
        }
        if (!fs::exists(tok, ec)) {
            throw std::runtime_error(concat_utf8("Tokenizer missing (", tok.filename(), ") - download the model in Settings"));
        }

        if (!env_) env_ = std::make_shared<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "usyseek");
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(static_cast<int>(std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u)));
        so.SetInterOpNumThreads(1);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        so.DisableCpuMemArena();

        auto model = std::make_shared<Model>();
        model->env = env_;
        model->session = std::make_unique<Ort::Session>(*env_, paths.onnx.c_str(), so);
        if (model->session->GetInputCount() < 1 || model->session->GetOutputCount() < 1) {
            throw std::runtime_error("Text ONNX model must have at least one input and one output");
        }

        const Ort::TypeInfo in_type = model->session->GetInputTypeInfo(0);
        if (in_type.GetONNXType() != ONNX_TYPE_TENSOR) {
            throw std::runtime_error("Text ONNX model input 0 is not a tensor");
        }
        const auto in_info = in_type.GetTensorTypeAndShapeInfo();
        if (static_cast<const OrtTensorTypeAndShapeInfo*>(in_info) == nullptr) {
            throw std::runtime_error("Text ONNX model input 0 has no tensor info");
        }
        const ONNXTensorElementDataType in_elem = in_info.GetElementType();
        if (in_elem != ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 && in_elem != ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32) {
            throw std::runtime_error("Text ONNX model input 0 must have int64 or int32 element type");
        }
        model->in_elem_type = in_elem;

        model->ctx = 77;
        if (in_info.HasShape()) {
            const auto in_shape = in_info.GetShape();
            if (in_shape.size() != 2) {
                throw std::runtime_error("Text ONNX model input 0 must be a 2D tensor [batch, context]");
            }
            if (in_shape[0] > 0 && in_shape[0] != 1) {
                throw std::runtime_error("Text ONNX model input 0 requires unsupported fixed batch size");
            }
            if (in_shape[1] > 0) {
                if (std::cmp_greater(in_shape[1], MAX_CONTEXT_LENGTH)) {
                    throw std::runtime_error("Text ONNX model context length exceeds supported limit");
                }
                model->ctx = static_cast<size_t>(in_shape[1]);
            }
        }

        const Ort::TypeInfo out_type = model->session->GetOutputTypeInfo(0);
        if (out_type.GetONNXType() != ONNX_TYPE_TENSOR) {
            throw std::runtime_error("Text ONNX model output 0 is not a tensor");
        }
        const auto out_info = out_type.GetTensorTypeAndShapeInfo();
        if (static_cast<const OrtTensorTypeAndShapeInfo*>(out_info) == nullptr) {
            throw std::runtime_error("Text ONNX model output 0 has no tensor info");
        }
        if (out_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            throw std::runtime_error("Text ONNX model output 0 must have float32 element type");
        }

        const Ort::AllocatorWithDefaultOptions alloc;
        const auto in_name_ptr = model->session->GetInputNameAllocated(0, alloc);
        const auto out_name_ptr = model->session->GetOutputNameAllocated(0, alloc);
        if (!in_name_ptr || in_name_ptr.get()[0] == '\0' || !out_name_ptr || out_name_ptr.get()[0] == '\0') {
            throw std::runtime_error("Text ONNX model has missing input or output name");
        }
        model->in_name = in_name_ptr.get();
        model->out_name = out_name_ptr.get();

        RustBuf terr;
        const std::string tok_path = to_utf8(tok.wstring());
        model->tok = usy_tok_load(ustr(tok_path), model->ctx, &terr.b);
        if (!model->tok) throw std::runtime_error(ulp::str::concat_strings("Tokenizer failed to load: ", terr.str()));
        return model;
    }

    mutable std::mutex m_;
    std::mutex loader_mx_; // serializes loader_ creation and join in load_async/stop
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
    std::shared_ptr<Ort::Env> env_; // loader thread only
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
    Engine() = default;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;
    ~Engine() { stop(); }

    std::function<void(std::unique_ptr<SearchResult>)> on_result;
    std::function<void(std::function<void()>)> post_ui; // marshal a callback to the UI thread
    std::function<void(bool ready, std::string error)> on_db_state;
    std::function<void(std::string coll, std::string error)> on_db_cleared;
    std::function<void(std::string title, std::string detail)> on_job_dropped;

    void start(const Settings& s) {
        if (thread_.joinable()) {
            stopping_atomic_.store(true, std::memory_order_release);
            {
                const std::scoped_lock lk{m_};
                stopping_ = true;
            }
            cv_.notify_all();
            thread_ = {};
            disarm_db_want_wait();
        }
        stopping_atomic_.store(false, std::memory_order_release);
        {
            const std::scoped_lock lk{m_};
            stopping_ = false;
            settings_ = s;
        }
        db_want_event_.reset(CreateEventW(nullptr, TRUE, FALSE, INDEXER_DB_WANT_EVENT));
        thread_ = std::jthread([this] { run(); });
    }

    void stop() noexcept {
        stopping_atomic_.store(true, std::memory_order_release);
        {
            const std::scoped_lock lk{m_};
            stopping_ = true;
        }
        cv_.notify_all();
        encoder_.stop();
        thread_ = {};
        disarm_db_want_wait();
    }

    void notify_indexer_changed() noexcept {
        {
            const std::scoped_lock lk{m_};
        }
        cv_.notify_one();
    }

    void set_indexing_active(bool /*active*/) noexcept {
        notify_indexer_changed();
    }

    void search(SearchRequest r) noexcept {
        {
            const std::scoped_lock lk{m_};
            pending_search_ = std::move(r);
        }
        cv_.notify_one();
    }

    void post(std::function<void(Db&)> job, bool notify_if_dropped = true) {
        {
            const std::scoped_lock lk{m_};
            jobs_.push_back({.fn = std::move(job), .notify_if_dropped = notify_if_dropped});
        }
        cv_.notify_one();
    }

    /// Runs `job` as soon as the database is open (waiting through an indexing pause if needed).
    void post_when_ready(std::function<void(Db&)> job) {
        {
            const std::scoped_lock lk{m_};
            deferred_jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

    void set_settings(const Settings& s) {
        const std::scoped_lock lk{m_};
        settings_ = s;
    }

    enum class DbState : uint8_t { Loading, Ready, PausedForIndexing, Failed };

    TextEncoder& encoder() noexcept { return encoder_; }
    [[nodiscard]] DbState db_state() const noexcept { return db_state_.load(std::memory_order_acquire); }
    [[nodiscard]] bool db_ready() const noexcept { return db_state() == DbState::Ready; }
    [[nodiscard]] bool db_loading() const noexcept { return db_state() == DbState::Loading; }
    [[nodiscard]] bool db_paused_for_indexing() const noexcept { return db_state() == DbState::PausedForIndexing; }

    [[nodiscard]] std::string collection() const {
        const std::scoped_lock lk{m_};
        return settings_.model_name;
    }

private:
    static void CALLBACK on_db_want(PVOID ctx, BOOLEAN /*timed_out*/) noexcept {
        static_cast<Engine*>(ctx)->notify_indexer_changed();
    }

    void arm_db_want_wait() noexcept {
        if (!db_want_event_ || stopping_atomic_.load(std::memory_order_acquire)) return;
        const std::scoped_lock lk{wait_mx_};
        if (stopping_atomic_.load(std::memory_order_acquire)) return;
        if (wait_handle_) {
            (void)UnregisterWaitEx(wait_handle_, INVALID_HANDLE_VALUE);
            wait_handle_ = nullptr;
        }
        if (!RegisterWaitForSingleObject(&wait_handle_, db_want_event_.get(), &Engine::on_db_want, this, INFINITE, WT_EXECUTEONLYONCE)) {
            wait_handle_ = nullptr;
        }
    }

    void disarm_db_want_wait() noexcept {
        const std::scoped_lock lk{wait_mx_};
        if (wait_handle_) {
            (void)UnregisterWaitEx(wait_handle_, INVALID_HANDLE_VALUE);
            wait_handle_ = nullptr;
        }
    }

    void run() {
        const ulp::win::COMWrapper<false> com{COINIT_MULTITHREADED};
        const auto close_on_exit = wil::scope_exit([this] { (void)db_.close(); });
        Settings s;
        {
            const std::scoped_lock lk{m_};
            s = settings_;
        }

        enum class Health : uint8_t { Ok, PausedForIndexing, Failed };
        auto ensure_collection_healthy = [&](const std::string& coll, std::string& out_err) -> Health {
            out_err.clear();
            if (is_indexer_db_wanted()) return Health::PausedForIndexing;
            std::string load_err;
            for (int attempt = 0; attempt < 6; ++attempt) {
                load_err.clear();
                if (db_.check_collection(coll, &load_err)) return Health::Ok;
                if (stopping_atomic_.load(std::memory_order_acquire)) return Health::Failed;
                if (is_indexer_db_wanted()) return Health::PausedForIndexing;
                if (attempt < 5) Sleep(50);
            }
            if (is_indexer_db_wanted()) return Health::PausedForIndexing;
            // Never delete a collection when the error indicates file-lock / sharing contention.
            if (is_db_lock_error(load_err)) {
                out_err = std::move(load_err);
                return Health::Failed;
            }
            (void)db_.drop_collection(coll);
            if (on_db_cleared) on_db_cleared(coll, load_err);
            return Health::Ok;
        };

        const auto pause_db = [&] {
            const bool already_paused = db_paused_for_indexing() && !db_.opened();
            std::string err;
            (void)db_.close(&err);
            db_state_.store(DbState::PausedForIndexing, std::memory_order_release);
            if (!already_paused && on_db_state) on_db_state(false, {});
        };
        const auto open_db = [&] {
            if (is_indexer_db_wanted()) {
                pause_db();
                return;
            }
            if (db_want_event_) ResetEvent(db_want_event_.get());
            arm_db_want_wait();
            if (is_indexer_db_wanted()) {
                pause_db();
                return;
            }
            db_state_.store(DbState::Loading, std::memory_order_release);
            if (on_db_state) on_db_state(false, {});
            std::string err;
            if (!db_.open(edge_data_dir(), &err)) {
                db_state_.store(DbState::Failed, std::memory_order_release);
                if (on_db_state) on_db_state(false, err);
                return;
            }
            std::string health_err;
            const Health h = ensure_collection_healthy(s.model_name, health_err);
            if (h == Health::PausedForIndexing || is_indexer_db_wanted()) {
                pause_db();
                return;
            }
            if (h == Health::Failed) {
                (void)db_.close();
                db_state_.store(DbState::Failed, std::memory_order_release);
                if (on_db_state) on_db_state(false, health_err);
                return;
            }
            db_state_.store(DbState::Ready, std::memory_order_release);
            if (on_db_state) on_db_state(true, {});
        };

        if (is_indexer_db_wanted()) {
            pause_db();
        } else {
            open_db();
        }

        for (;;) {
            std::optional<SearchRequest> req;
            PostedJob job;
            bool need_pause = false;
            bool need_resume = false;
            {
                std::unique_lock lk{m_};
                const auto wake = [&] {
                    if (stopping_) return true;
                    const bool want = is_indexer_db_wanted();
                    const bool paused = db_paused_for_indexing();
                    if (want && !paused) return true;
                    if (!want && paused) return true;
                    if (db_ready() && !deferred_jobs_.empty()) return true;
                    return pending_search_.has_value() || !jobs_.empty();
                };
                if (db_paused_for_indexing()) {
                    cv_.wait_for(lk, std::chrono::milliseconds(250), wake);
                } else {
                    cv_.wait(lk, wake);
                }
                if (stopping_) return;

                const bool want = is_indexer_db_wanted();
                if (want) {
                    if (!db_paused_for_indexing() || db_.opened()) need_pause = true;
                } else if (db_paused_for_indexing()) {
                    need_resume = true;
                }

                if (!need_pause && !need_resume) {
                    if (!jobs_.empty()) {
                        job = std::move(jobs_.front());
                        jobs_.pop_front();
                    } else if (db_ready() && !deferred_jobs_.empty()) {
                        job = {.fn = std::move(deferred_jobs_.front()), .notify_if_dropped = true};
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
            if (job.fn) {
                if (db_ready()) {
                    job.fn(db_);
                } else if (job.notify_if_dropped && on_job_dropped) {
                    if (db_paused_for_indexing()) {
                        on_job_dropped("Database unavailable while indexing", "Database updates are paused while indexing is in progress.");
                    } else if (db_loading()) {
                        on_job_dropped("Database is loading", "Database update could not run because the database is still opening.");
                    } else {
                        on_job_dropped("Database unavailable", "Queued database update could not run because the database is closed.");
                    }
                }
                continue;
            }
            if (req && on_result) {
                if (db_ready()) {
                    std::string health_err;
                    const Health h = ensure_collection_healthy(s.model_name, health_err);
                    if (h == Health::PausedForIndexing) {
                        pause_db();
                    } else if (h == Health::Failed) {
                        auto res = std::make_unique<SearchResult>();
                        res->seq = req->seq;
                        res->append = req->offset > 0;
                        res->error = to_wide(health_err);
                        on_result(std::move(res));
                        continue;
                    }
                }
                on_result(execute(*req, s));
            }
        }
    }

    std::unique_ptr<SearchResult> execute(const SearchRequest& r, const Settings& s) {
        auto res = std::make_unique<SearchResult>();
        res->seq = r.seq;
        res->append = r.offset > 0;
        const auto t0 = std::chrono::steady_clock::now();
        if (!db_ready()) {
            res->error = db_paused_for_indexing()
                ? L"Database unavailable while indexing"
                : (db_loading() ? L"Database loading…" : L"Database unavailable");
            return res;
        }

        ParsedQuery q = parse_query(r.query);
        if (q.unknown_type) {
            res->error = ulp::str::concat_strings(L"Unknown type filter 'type:", *q.unknown_type, L"' (use image or video)");
            return res;
        }
        if (q.empty()) return res;

        DbFilter f;
        if (q.kind) {
            f.kind = *q.kind;
        } else if (r.kind >= 0) {
            f.kind = r.kind;
        }
        f.tags_all = q.tags;
        f.tags_none = q.not_tags;
        f.dirs = q.dirs;
        f.names = q.names;
        const bool has_filter = f.kind || !f.tags_all.empty() || !f.tags_none.empty() || !f.dirs.empty() || !f.names.empty();
        static constexpr std::array<std::string_view, 9> fields = {"p", "n", "k", "m", "s", "w", "h", "d", "t"};
        static constexpr std::array<std::string_view, 1> kind_field = {"k"};
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
            std::optional<uint64_t> self_id;
            if (q.similar) {
                const uint64_t target_id = path_id(*q.similar);
                if (db_.retrieve(coll, std::span{&target_id, 1}, kind_field).value_or(DbPoints{}).points.empty()) {
                    res->error = L"Not indexed yet";
                    return res;
                }
                self_id = target_id;
                qr.id = target_id;
                pts = db_.query(coll, {}, qr, &err);
            } else {
                std::wstring enc_err;
                auto v = encoder_.encode(q.text, &enc_err, [this] noexcept {
                    return stopping_atomic_.load(std::memory_order_acquire) || is_indexer_db_wanted();
                });
                if (!v) {
                    res->error = enc_err;
                    return res;
                }
                if (stopping_atomic_.load(std::memory_order_acquire)) {
                    res->error = L"Search cancelled";
                    return res;
                }
                if (is_indexer_db_wanted()) {
                    res->error = L"Database unavailable while indexing";
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
            res->next_offset = r.offset > (std::numeric_limits<uint64_t>::max() - raw_count)
                ? std::numeric_limits<uint64_t>::max()
                : (r.offset + raw_count);
            res->points = std::move(pts->points);
            if (self_id) std::erase_if(res->points, [&](const DbPoint& p) noexcept { return p.id == *self_id; });
        } else {
            // Filter-only queries: newest first
            if (r.offset > (std::numeric_limits<uint64_t>::max() - PAGE)) {
                return res;
            }
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

    struct PostedJob {
        std::function<void(Db&)> fn;
        bool notify_if_dropped = true;
    };

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::optional<SearchRequest> pending_search_;
    std::deque<PostedJob> jobs_;
    std::deque<std::function<void(Db&)>> deferred_jobs_;
    bool stopping_ = false;
    std::atomic<bool> stopping_atomic_{false};
    Settings settings_;
    Db db_;
    std::atomic<DbState> db_state_{DbState::Loading};
    std::mutex wait_mx_;
    wil::unique_event_nothrow db_want_event_;
    HANDLE wait_handle_ = nullptr;
    TextEncoder encoder_;
    std::jthread thread_;
};

} // namespace usyseek::ui
