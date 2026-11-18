#pragma once

#include "gfx.hpp"
#include "preview_math.hpp"

#include <shlwapi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace usyseek::ui {

enum class MediaTickStatus : uint8_t { NoChange, FrameReady, DeviceLost };

/// Decodes and plays animated GIFs in the preview viewer without locking files or exhausting GPU/RAM.
/// Fits small/medium GIFs into a 96 MB frame cache; streams huge GIFs via a bounded sliding lookahead queue.
/// GPU bitmap creation and drawing occur strictly on the UI thread and survive D3D/D2D device resets cleanly.
class GifPlayer {
public:
    static constexpr uint64_t MAX_FILE_MEM_BYTES = 64ull << 20u;
    static constexpr uint64_t MAX_RAM_BYTES = 96ull << 20u;
    static constexpr uint32_t MAX_CANVAS_DIM = 16384u;
    static constexpr uint64_t MAX_CANVAS_BYTES = 256ull << 20u;

    GifPlayer() = default;
    GifPlayer(const GifPlayer&) = delete;
    GifPlayer& operator=(const GifPlayer&) = delete;
    GifPlayer(GifPlayer&&) = delete;
    GifPlayer& operator=(GifPlayer&&) = delete;
    ~GifPlayer() { shutdown(); }

    void set_notify(HWND hwnd, UINT msg) noexcept {
        const std::scoped_lock lk{notify_mtx_};
        notify_hwnd_ = hwnd;
        notify_msg_ = msg;
    }

    void shutdown() noexcept {
        {
            const std::scoped_lock lk{notify_mtx_};
            notify_hwnd_ = nullptr;
        }
        {
            const std::scoped_lock lk{mtx_};
            ++generation_;
            stopping_ = true;
            has_pending_job_ = false;
            clear_session_locked();
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
        reset_gpu();
        ui_gen_ = 0;
        ui_reset_state();
    }

    void open(uint64_t item_id, const std::wstring& path, UINT max_px) {
        if (item_id == ui_item_id_ && path == ui_path_ && !ui_path_.empty() && ui_active_) {
            return;
        }
        close();
        if (path.empty() || !is_gif_path(path)) return;

        ensure_worker_started();
        uint64_t gen = 0;
        {
            const std::scoped_lock lk{mtx_};
            gen = ++generation_;
            pending_job_ = Job{.gen = gen, .path = path, .max_px = max_px};
            has_pending_job_ = true;
            clear_session_locked();
            session_.gen = gen;
        }
        ui_gen_ = gen;
        ui_item_id_ = item_id;
        ui_path_ = path;
        ui_max_px_ = max_px;
        ui_active_ = true;
        ui_playing_ = true;
        cv_.notify_all();
    }

    void close() noexcept {
        {
            const std::scoped_lock lk{mtx_};
            ++generation_;
            has_pending_job_ = false;
            clear_session_locked();
        }
        cv_.notify_all();
        reset_gpu();
        ui_reset_state();
    }

    /// Drops the D2D bitmap while keeping decoded CPU pixels intact for seamless post-reset re-upload.
    void reset_gpu() noexcept {
        gpu_bmp_.reset();
        gpu_bmp_w_ = 0;
        gpu_bmp_h_ = 0;
        uploaded_pixel_seq_ = 0;
    }

    /// Absorbs newly decoded header/frames posted from the background worker.
    /// Returns true if visual preview state changed and should be redrawn.
    bool process_worker_signal() noexcept {
        bool changed = false;
        const auto now = std::chrono::steady_clock::now();
        const std::scoped_lock lk{mtx_};
        if (!ui_active_ || session_.gen != ui_gen_) return false;

        if (session_.settled_static && !ui_settled_static_) {
            ui_settled_static_ = true;
            ui_static_fallback_error_ = session_.static_fallback_error;
            ui_total_frames_ = 0;
            changed = true;
            return changed;
        }

        if (session_.has_header && ui_total_frames_ == 0) {
            ui_native_w_ = session_.native_w;
            ui_native_h_ = session_.native_h;
            ui_out_w_ = session_.out_w;
            ui_out_h_ = session_.out_h;
            ui_total_frames_ = session_.total_frames;
            ui_streaming_ = session_.streaming;
            changed = true;
        }

        if (!ui_current_pixels_) {
            if (!session_.streaming && !session_.cached_frames.empty()) {
                adopt_frame_locked(session_.cached_frames[0], now, 0);
                changed = true;
            } else if (session_.streaming && !session_.stream_queue_.empty()) {
                const Frame f = std::move(session_.stream_queue_.front());
                session_.stream_queue_.pop_front();
                cv_.notify_one();
                adopt_frame_locked(f, now);
                changed = true;
            }
        } else if (ui_waiting_stream_frame_ && session_.streaming && !session_.stream_queue_.empty()) {
            const Frame f = std::move(session_.stream_queue_.front());
            session_.stream_queue_.pop_front();
            cv_.notify_one();
            adopt_frame_locked(f, now);
            ui_waiting_stream_frame_ = false;
            changed = true;
        }
        return changed || (ui_total_frames_ > 1 && (!session_.streaming ? session_.cached_frames.size() > 1 : !session_.stream_queue_.empty()));
    }

    /// Advances playback if the current frame's delay has elapsed.
    /// Returns `{advanced, next_wait_ms}` (`next_wait_ms == 0` means no timer needed right now).
    [[nodiscard]] std::pair<bool, UINT> tick(std::chrono::steady_clock::time_point now) noexcept {
        if (!ui_active_ || ui_settled_static_ || !ui_playing_ || ui_total_frames_ <= 1 || !ui_current_pixels_) {
            return {false, 0u};
        }
        if (now < ui_frame_deadline_) {
            const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(ui_frame_deadline_ - now).count();
            return {false, static_cast<UINT>(std::clamp<int64_t>(rem, 10, 60000))};
        }

        const std::scoped_lock lk{mtx_};
        if (session_.gen != ui_gen_) return {false, 0u};

        bool advanced = false;
        if (!session_.streaming) {
            const size_t avail = session_.cached_frames.size();
            if (avail > 1) {
                const size_t next = (ui_cached_pos_ + 1) % avail;
                // Wait cleanly if decode of upcoming frame hasn't finished yet (unless decode is complete)
                if (next == 0 && !session_.decode_complete && avail < ui_total_frames_) {
                    // Hold current frame until remainder finishes decoding
                } else {
                    adopt_frame_locked(session_.cached_frames[next], now, next);
                    advanced = true;
                }
            } else if (avail == 1 && !session_.decode_complete) {
                // Frame 1+ still decoding
                return {false, 25u};
            }
        } else {
            if (!session_.stream_queue_.empty()) {
                const Frame f = std::move(session_.stream_queue_.front());
                session_.stream_queue_.pop_front();
                cv_.notify_one();
                adopt_frame_locked(f, now);
                ui_waiting_stream_frame_ = false;
                advanced = true;
            } else {
                ui_waiting_stream_frame_ = true;
                return {false, 0u};
            }
        }

        const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(ui_frame_deadline_ - now).count();
        const UINT wait_ms = static_cast<UINT>(std::clamp<int64_t>(rem, 10, 60000));
        return {advanced, wait_ms};
    }

    /// Ensures `gpu_bmp_` matches `ui_current_pixels_` on `dc`. Safe across GPU resets.
    [[nodiscard]] MediaTickStatus sync_bitmap(ID2D1DeviceContext* dc) noexcept {
        if (!dc || !ui_current_pixels_ || ui_out_w_ == 0 || ui_out_h_ == 0) return MediaTickStatus::NoChange;
        const size_t expected = static_cast<size_t>(ui_out_w_) * ui_out_h_ * 4u;
        if (ui_current_pixels_->size() < expected) return MediaTickStatus::NoChange;

        if (!gpu_bmp_ || gpu_bmp_w_ != ui_out_w_ || gpu_bmp_h_ != ui_out_h_) {
            gpu_bmp_.reset();
            const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            const HRESULT hr = dc->CreateBitmap(
                D2D1::SizeU(ui_out_w_, ui_out_h_),
                ui_current_pixels_->data(),
                ui_out_w_ * 4u,
                &props,
                &gpu_bmp_);
            if (is_device_lost_hr(hr)) {
                reset_gpu();
                return MediaTickStatus::DeviceLost;
            }
            if (FAILED(hr) || !gpu_bmp_) {
                gpu_bmp_.reset();
                return MediaTickStatus::NoChange;
            }
            gpu_bmp_w_ = ui_out_w_;
            gpu_bmp_h_ = ui_out_h_;
            uploaded_pixel_seq_ = ui_pixel_seq_;
            return MediaTickStatus::FrameReady;
        }

        if (uploaded_pixel_seq_ != ui_pixel_seq_) {
            const D2D1_RECT_U dst{.left = 0u, .top = 0u, .right = ui_out_w_, .bottom = ui_out_h_};
            const HRESULT hr = gpu_bmp_->CopyFromMemory(&dst, ui_current_pixels_->data(), ui_out_w_ * 4u);
            if (is_device_lost_hr(hr)) {
                reset_gpu();
                return MediaTickStatus::DeviceLost;
            }
            if (FAILED(hr)) return MediaTickStatus::NoChange;
            uploaded_pixel_seq_ = ui_pixel_seq_;
            return MediaTickStatus::FrameReady;
        }
        return MediaTickStatus::NoChange;
    }

    void toggle_play() noexcept {
        if (!has_animation()) return;
        ui_playing_ = !ui_playing_;
        if (ui_playing_) {
            ui_frame_deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(ui_current_delay_ms_);
        }
    }

    void set_playing(bool on) noexcept {
        if (!ui_active_ || ui_settled_static_) return;
        if (on && !has_animation()) return;
        if (ui_playing_ == on) return;
        ui_playing_ = on;
        if (ui_playing_) {
            ui_frame_deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(ui_current_delay_ms_);
        }
    }

    void step_frame(int delta) noexcept {
        if (!has_animation()) return;
        const auto total = static_cast<int64_t>(ui_total_frames_);
        const int64_t wrapped = (((static_cast<int64_t>(ui_current_index_) + delta) % total) + total) % total;
        seek_to_frame(static_cast<size_t>(wrapped));
    }

    void seek_fraction(float ratio) noexcept {
        if (!has_animation()) return;
        const float r = std::clamp(ratio, 0.0f, 1.0f);
        const auto idx = static_cast<size_t>(std::lround(r * static_cast<float>(ui_total_frames_ - 1)));
        seek_to_frame(std::min(idx, ui_total_frames_ - 1));
    }

    void seek_to_frame(size_t target) noexcept {
        if (!has_animation()) return;
        target = std::min(target, ui_total_frames_ - 1);
        const auto now = std::chrono::steady_clock::now();
        const std::scoped_lock lk{mtx_};
        if (session_.gen != ui_gen_) return;
        if (!session_.streaming) {
            if (session_.cached_frames.empty()) return;
            size_t idx = std::min(target, session_.cached_frames.size() - 1);
            if (session_.cached_frames[idx].index != target) {
                for (size_t i = 0; i < session_.cached_frames.size(); ++i) {
                    if (session_.cached_frames[i].index >= target) {
                        idx = i;
                        break;
                    }
                }
            }
            adopt_frame_locked(session_.cached_frames[idx], now, idx);
        } else {
            session_.stream_queue_.clear();
            session_.seek_target = target;
            ++session_.seek_seq;
            ui_waiting_stream_frame_ = true;
            ui_current_index_ = target;
            cv_.notify_one();
        }
    }

    [[nodiscard]] bool active() const noexcept { return ui_active_; }
    [[nodiscard]] bool settled_static() const noexcept { return ui_settled_static_; }
    [[nodiscard]] bool static_fallback_error() const noexcept { return ui_static_fallback_error_; }
    [[nodiscard]] bool has_animation() const noexcept { return ui_active_ && !ui_settled_static_ && ui_total_frames_ > 1; }
    [[nodiscard]] bool has_frame() const noexcept { return ui_active_ && ui_current_pixels_ != nullptr && ui_out_w_ > 0 && ui_out_h_ > 0; }
    [[nodiscard]] bool playing() const noexcept { return ui_playing_; }
    [[nodiscard]] size_t current_frame() const noexcept { return ui_current_index_; }
    [[nodiscard]] size_t frame_count() const noexcept { return ui_total_frames_; }
    [[nodiscard]] uint32_t current_delay_ms() const noexcept { return ui_current_delay_ms_; }
    [[nodiscard]] ID2D1Bitmap1* bitmap() const noexcept { return gpu_bmp_.get(); }
    [[nodiscard]] uint32_t output_width() const noexcept { return ui_out_w_; }
    [[nodiscard]] uint32_t output_height() const noexcept { return ui_out_h_; }
    [[nodiscard]] uint32_t native_width() const noexcept { return ui_native_w_; }
    [[nodiscard]] uint32_t native_height() const noexcept { return ui_native_h_; }

private:
    struct Frame {
        std::shared_ptr<const std::vector<uint8_t>> pbgra;
        uint32_t delay_ms = 100;
        size_t index = 0;
    };

    struct Job {
        uint64_t gen = 0;
        std::wstring path;
        UINT max_px = 0;
    };

    struct SessionState {
        uint64_t gen = 0;
        bool has_header = false;
        bool settled_static = false;
        bool static_fallback_error = false;
        bool streaming = false;
        bool decode_complete = false;
        uint32_t native_w = 0, native_h = 0;
        uint32_t out_w = 0, out_h = 0;
        size_t total_frames = 0;
        std::vector<Frame> cached_frames;
        std::deque<Frame> stream_queue_;
        uint64_t seek_seq = 0;
        size_t seek_target = 0;
    };

    [[nodiscard]] static bool is_device_lost_hr(HRESULT hr) noexcept {
        return hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
    }

    void clear_session_locked() noexcept {
        session_.gen = 0;
        session_.has_header = false;
        session_.settled_static = false;
        session_.static_fallback_error = false;
        session_.streaming = false;
        session_.decode_complete = false;
        session_.native_w = session_.native_h = 0;
        session_.out_w = session_.out_h = 0;
        session_.total_frames = 0;
        session_.cached_frames.clear();
        session_.stream_queue_.clear();
        session_.seek_seq = 0;
        session_.seek_target = 0;
    }

    void ui_reset_state() noexcept {
        ui_item_id_ = 0;
        ui_path_.clear();
        ui_max_px_ = 0;
        ui_active_ = false;
        ui_settled_static_ = false;
        ui_static_fallback_error_ = false;
        ui_streaming_ = false;
        ui_playing_ = false;
        ui_waiting_stream_frame_ = false;
        ui_native_w_ = ui_native_h_ = 0;
        ui_out_w_ = ui_out_h_ = 0;
        ui_total_frames_ = 0;
        ui_current_index_ = 0;
        ui_cached_pos_ = 0;
        ui_current_delay_ms_ = 100;
        ui_current_pixels_.reset();
        ui_pixel_seq_ = 0;
        ui_frame_deadline_ = {};
    }

    void adopt_frame_locked(const Frame& f, std::chrono::steady_clock::time_point now, size_t cache_pos = 0) noexcept {
        ui_current_index_ = f.index;
        ui_cached_pos_ = cache_pos;
        ui_current_delay_ms_ = f.delay_ms;
        ui_current_pixels_ = f.pbgra;
        ++ui_pixel_seq_;
        ui_frame_deadline_ = now + std::chrono::milliseconds(f.delay_ms);
    }

    void post_signal() noexcept {
        HWND hwnd = nullptr;
        UINT msg = 0;
        {
            const std::scoped_lock lk{notify_mtx_};
            hwnd = notify_hwnd_;
            msg = notify_msg_;
        }
        if (hwnd && msg) PostMessageW(hwnd, msg, 0, 0);
    }

    void ensure_worker_started() {
        if (!worker_.joinable()) {
            worker_ = std::jthread([this](const std::stop_token& st) { worker_loop(st); });
        }
    }

    static bool query_u16(IWICMetadataQueryReader* q, const wchar_t* name, UINT& out) noexcept {
        if (!q) return false;
        PROPVARIANT v;
        PropVariantInit(&v);
        const bool ok = SUCCEEDED(q->GetMetadataByName(name, &v)) && (v.vt == VT_UI2 || v.vt == VT_UI1);
        if (ok) out = (v.vt == VT_UI2) ? v.uiVal : v.bVal;
        PropVariantClear(&v);
        return ok;
    }

    struct OpenedDecoder {
        wil::com_ptr<IWICBitmapDecoder> decoder;
        std::vector<uint8_t> mem_file;
        wil::com_ptr<IStream> mem_stream;
    };

    static std::optional<OpenedDecoder> open_gif_decoder(IWICImagingFactory* wic, const std::wstring& path) {
        if (!wic) return std::nullopt;
        OpenedDecoder res;
        wil::unique_hfile fh{CreateFileW(
            path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        if (!fh) return std::nullopt;

        LARGE_INTEGER fsize{};
        if (!GetFileSizeEx(fh.get(), &fsize) || fsize.QuadPart <= 0) return std::nullopt;

        const auto bytes = static_cast<uint64_t>(fsize.QuadPart);
        if (bytes <= MAX_FILE_MEM_BYTES) {
            res.mem_file.resize(static_cast<size_t>(bytes));
            size_t total_read = 0;
            while (total_read < res.mem_file.size()) {
                const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(res.mem_file.size() - total_read, 16ull << 20u));
                DWORD got = 0;
                if (!ReadFile(fh.get(), res.mem_file.data() + total_read, chunk, &got, nullptr) || got == 0) {
                    return std::nullopt;
                }
                total_read += got;
            }
            fh.reset(); // Release OS file lock immediately
            IStream* raw_stream = SHCreateMemStream(res.mem_file.data(), static_cast<UINT>(res.mem_file.size()));
            if (!raw_stream) return std::nullopt;
            res.mem_stream.attach(raw_stream);
            if (FAILED(wic->CreateDecoderFromStream(res.mem_stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, &res.decoder))) {
                return std::nullopt;
            }
        } else {
            fh.reset();
            if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &res.decoder))) {
                return std::nullopt;
            }
        }
        return res;
    }

    static std::shared_ptr<const std::vector<uint8_t>> scale_canvas_if_needed(
        IWICImagingFactory* wic, uint32_t cw, uint32_t ch, uint32_t out_w, uint32_t out_h,
        const std::vector<uint8_t>& canvas_pbgra) {
        if (cw == out_w && ch == out_h) {
            return std::make_shared<std::vector<uint8_t>>(canvas_pbgra);
        }
        wil::com_ptr<IWICBitmap> src_bmp;
        const UINT src_stride = cw * 4u;
        if (FAILED(wic->CreateBitmapFromMemory(
                cw, ch, GUID_WICPixelFormat32bppPBGRA, src_stride,
                static_cast<UINT>(canvas_pbgra.size()), const_cast<BYTE*>(canvas_pbgra.data()), &src_bmp))) {
            return nullptr;
        }
        wil::com_ptr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(src_bmp.get(), out_w, out_h, WICBitmapInterpolationModeLinear))) {
            return nullptr;
        }
        auto out = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(out_w) * out_h * 4u);
        const UINT dst_stride = out_w * 4u;
        if (FAILED(scaler->CopyPixels(nullptr, dst_stride, static_cast<UINT>(out->size()), out->data()))) {
            return nullptr;
        }
        return out;
    }

    void mark_static(uint64_t gen, bool fallback_error = false) noexcept {
        {
            const std::scoped_lock lk{mtx_};
            if (session_.gen != gen || stopping_) return;
            session_.settled_static = true;
            session_.static_fallback_error = fallback_error;
        }
        post_signal();
    }

    void worker_loop(const std::stop_token& st) {
        const ulp::win::COMWrapper<false> com{
            static_cast<DWORD>(COINIT_MULTITHREADED) | static_cast<DWORD>(COINIT_DISABLE_OLE1DDE)};
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        wil::com_ptr<IWICImagingFactory> wic;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
            return;
        }

        while (!st.stop_requested()) {
            Job job;
            {
                std::unique_lock lk{mtx_};
                cv_.wait(lk, [&] { return stopping_ || st.stop_requested() || has_pending_job_; });
                if (stopping_ || st.stop_requested()) return;
                job = std::move(pending_job_);
                has_pending_job_ = false;
            }
            run_job(st, wic.get(), job);
        }
    }

    void run_job(const std::stop_token& st, IWICImagingFactory* wic, const Job& job) {
        const auto aborted = [&] noexcept {
            return st.stop_requested() || stopping_ || generation_.load(std::memory_order_relaxed) != job.gen;
        };
        if (aborted()) return;

        auto opened = open_gif_decoder(wic, job.path);
        if (!opened || !opened->decoder) {
            mark_static(job.gen, true);
            return;
        }

        GUID container{};
        UINT total = 0;
        if (FAILED(opened->decoder->GetContainerFormat(&container)) || container != GUID_ContainerFormatGif ||
            FAILED(opened->decoder->GetFrameCount(&total)) || total <= 1) {
            mark_static(job.gen, false);
            return;
        }

        UINT cw = 0;
        UINT ch = 0;
        {
            wil::com_ptr<IWICMetadataQueryReader> gmeta;
            if (SUCCEEDED(opened->decoder->GetMetadataQueryReader(&gmeta))) {
                query_u16(gmeta.get(), L"/logscrdesc/Width", cw);
                query_u16(gmeta.get(), L"/logscrdesc/Height", ch);
            }
        }
        if (cw == 0 || ch == 0) {
            wil::com_ptr<IWICBitmapFrameDecode> f0;
            if (FAILED(opened->decoder->GetFrame(0, &f0)) || FAILED(f0->GetSize(&cw, &ch))) {
                mark_static(job.gen, true);
                return;
            }
        }
        if (cw == 0 || ch == 0 || cw > MAX_CANVAS_DIM || ch > MAX_CANVAS_DIM ||
            static_cast<uint64_t>(cw) * ch * 4u > MAX_CANVAS_BYTES) {
            mark_static(job.gen, true);
            return;
        }

        const auto [out_w, out_h] = fit_scaled_size(cw, ch, job.max_px);
        if (out_w == 0 || out_h == 0) {
            mark_static(job.gen, true);
            return;
        }

        const uint64_t frame_bytes = static_cast<uint64_t>(out_w) * out_h * 4u;
        const bool streaming = (frame_bytes * total) > MAX_RAM_BYTES;
        const size_t stream_window = streaming
            ? std::clamp<size_t>(static_cast<size_t>(MAX_RAM_BYTES / std::max<uint64_t>(1u, frame_bytes)), 4u, 12u)
            : 0u;

        {
            const std::scoped_lock lk{mtx_};
            if (session_.gen != job.gen || aborted()) return;
            session_.has_header = true;
            session_.native_w = cw;
            session_.native_h = ch;
            session_.out_w = out_w;
            session_.out_h = out_h;
            session_.total_frames = total;
            session_.streaming = streaming;
            if (!streaming) session_.cached_frames.reserve(total);
        }

        GifCompositor comp{cw, ch};
        std::vector<uint8_t> raw_frame;
        std::vector<uint8_t> composed;

        const auto decode_one = [&](UINT idx, bool produce_output, Frame& out_frame) -> bool {
            wil::com_ptr<IWICBitmapFrameDecode> frame;
            if (FAILED(opened->decoder->GetFrame(idx, &frame))) return false;

            UINT fw = 0;
            UINT fh = 0;
            if (FAILED(frame->GetSize(&fw, &fh)) || fw == 0 || fh == 0 || fw > MAX_CANVAS_DIM || fh > MAX_CANVAS_DIM) {
                return false;
            }

            UINT left = 0;
            UINT top = 0;
            UINT disposal_raw = 0;
            UINT delay_cs = 0;
            {
                wil::com_ptr<IWICMetadataQueryReader> fmeta;
                if (SUCCEEDED(frame->GetMetadataQueryReader(&fmeta))) {
                    query_u16(fmeta.get(), L"/imgdesc/Left", left);
                    query_u16(fmeta.get(), L"/imgdesc/Top", top);
                    query_u16(fmeta.get(), L"/grctlext/Disposal", disposal_raw);
                    query_u16(fmeta.get(), L"/grctlext/Delay", delay_cs);
                }
            }
            const auto disposal = static_cast<GifDisposal>(disposal_raw <= 3u ? disposal_raw : 0u);

            wil::com_ptr<IWICFormatConverter> conv;
            if (FAILED(wic->CreateFormatConverter(&conv)) ||
                FAILED(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                return false;
            }
            const uint64_t fbytes = static_cast<uint64_t>(fw) * fh * 4u;
            if (fbytes > MAX_CANVAS_BYTES) return false;
            raw_frame.resize(static_cast<size_t>(fbytes));
            if (FAILED(conv->CopyPixels(nullptr, fw * 4u, static_cast<UINT>(raw_frame.size()), raw_frame.data()))) {
                return false;
            }

            if (!comp.apply_frame(left, top, fw, fh, raw_frame, disposal, composed)) {
                return false;
            }

            if (produce_output) {
                auto scaled = scale_canvas_if_needed(wic, cw, ch, out_w, out_h, composed);
                if (!scaled) return false;
                out_frame.pbgra = std::move(scaled);
                out_frame.delay_ms = normalize_gif_delay_ms(delay_cs);
                out_frame.index = idx;
            }
            return true;
        };

        if (!streaming) {
            for (UINT i = 0; i < total; ++i) {
                if (aborted()) return;
                Frame f;
                if (!decode_one(i, true, f)) continue;

                bool notify = false;
                {
                    const std::scoped_lock lk{mtx_};
                    if (session_.gen != job.gen || aborted()) return;
                    session_.cached_frames.push_back(std::move(f));
                    const size_t n = session_.cached_frames.size();
                    notify = (n == 1 || n == 2);
                }
                if (notify) post_signal();
            }

            {
                const std::scoped_lock lk{mtx_};
                if (session_.gen != job.gen || aborted()) return;
                session_.decode_complete = true;
                if (session_.cached_frames.size() <= 1) {
                    session_.settled_static = true;
                    session_.static_fallback_error = true;
                }
            }
            post_signal();
            return;
        }

        // Streaming loop for giant multi-hundred-MB GIFs
        UINT next_idx = 0;
        uint64_t seen_seek_seq = 0;
        bool any_stream_frame = false;
        while (!aborted()) {
            std::optional<size_t> seek_to;
            {
                std::unique_lock lk{mtx_};
                cv_.wait(lk, [&] {
                    return aborted() || session_.gen != job.gen || session_.seek_seq != seen_seek_seq ||
                           session_.stream_queue_.size() < stream_window;
                });
                if (aborted() || session_.gen != job.gen) return;
                if (session_.seek_seq != seen_seek_seq) {
                    seen_seek_seq = session_.seek_seq;
                    seek_to = session_.seek_target;
                }
            }

            if (seek_to) {
                comp.reset();
                const UINT target = static_cast<UINT>(std::min<size_t>(*seek_to, total - 1));
                for (UINT i = 0; i < target; ++i) {
                    if (aborted()) return;
                    Frame dummy;
                    (void)decode_one(i, false, dummy);
                }
                next_idx = target;
            }

            if (next_idx == 0 && !seek_to) {
                comp.reset();
            }

            Frame f;
            const bool ok = decode_one(next_idx, true, f);
            next_idx = (next_idx + 1) % total;
            if (!ok) {
                if (next_idx == 0) {
                    if (!any_stream_frame && !aborted()) mark_static(job.gen, true);
                    return;
                }
                continue;
            }

            bool notify = false;
            {
                const std::scoped_lock lk{mtx_};
                if (session_.gen != job.gen || aborted()) return;
                if (session_.seek_seq != seen_seek_seq) continue;
                const bool was_empty = session_.stream_queue_.empty();
                session_.stream_queue_.push_back(std::move(f));
                any_stream_frame = true;
                notify = was_empty;
            }
            if (notify) post_signal();
        }
    }

    std::mutex notify_mtx_;
    HWND notify_hwnd_ = nullptr;
    UINT notify_msg_ = 0;

    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<uint64_t> generation_{0};
    std::atomic<bool> stopping_{false};
    bool has_pending_job_ = false;
    Job pending_job_;
    SessionState session_;
    std::jthread worker_;

    // UI-thread-only playback & GPU state
    uint64_t ui_gen_ = 0;
    uint64_t ui_item_id_ = 0;
    std::wstring ui_path_;
    UINT ui_max_px_ = 0;
    bool ui_active_ = false;
    bool ui_settled_static_ = false;
    bool ui_static_fallback_error_ = false;
    bool ui_streaming_ = false;
    bool ui_playing_ = false;
    bool ui_waiting_stream_frame_ = false;
    uint32_t ui_native_w_ = 0, ui_native_h_ = 0;
    uint32_t ui_out_w_ = 0, ui_out_h_ = 0;
    size_t ui_total_frames_ = 0;
    size_t ui_current_index_ = 0;
    size_t ui_cached_pos_ = 0;
    uint32_t ui_current_delay_ms_ = 100;
    std::shared_ptr<const std::vector<uint8_t>> ui_current_pixels_;
    uint64_t ui_pixel_seq_ = 0;
    std::chrono::steady_clock::time_point ui_frame_deadline_{};

    wil::com_ptr<ID2D1Bitmap1> gpu_bmp_;
    uint32_t gpu_bmp_w_ = 0, gpu_bmp_h_ = 0;
    uint64_t uploaded_pixel_seq_ = 0;
};

} // namespace usyseek::ui
