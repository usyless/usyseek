#pragma once

#include "gfx.hpp"

#include <usyseek/svg.hpp>

#include <shobjidl.h>

#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace usyseek::ui {

/// A decoded 32bpp BGRA raster.
struct Raster {
    UINT w = 0, h = 0;
    std::vector<uint8_t> pixels;
};

/// Decodes the first frame of an image with WIC, upright (EXIF orientation applied) and scaled down to fit
/// `max_px` on its longer side (0 = native size). Any thread with COM initialised.
inline std::optional<Raster> decode_image(const std::wstring& path, UINT max_px, bool premultiplied) {
    wil::com_ptr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return std::nullopt;
    wil::com_ptr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return std::nullopt;
    wil::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return std::nullopt;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || !w || !h) return std::nullopt;

    USHORT orientation = 1;
    wil::com_ptr<IWICMetadataQueryReader> meta;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&meta))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(meta->GetMetadataByName(L"System.Photo.Orientation", &v)) && v.vt == VT_UI2) orientation = v.uiVal;
        PropVariantClear(&v);
    }
    wil::com_ptr<IWICBitmapSource> src = frame;
    const UINT longest = std::max(w, h);
    if (max_px && longest > max_px) {
        const double s = static_cast<double>(max_px) / longest;
        const UINT sw = std::max(1u, static_cast<UINT>(std::lround(w * s))), shh = std::max(1u, static_cast<UINT>(std::lround(h * s)));
        wil::com_ptr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(src.get(), sw, shh, WICBitmapInterpolationModeHighQualityCubic))) return std::nullopt;
        src = scaler;
    }
    // EXIF orientation as a rotation followed by an optional horizontal mirror
    WICBitmapTransformOptions rot = WICBitmapTransformRotate0;
    bool mirror = false;
    switch (orientation) {
        case 2: mirror = true; break;
        case 3: rot = WICBitmapTransformRotate180; break;
        case 4: rot = WICBitmapTransformRotate180; mirror = true; break;
        case 5: rot = WICBitmapTransformRotate90; mirror = true; break;
        case 6: rot = WICBitmapTransformRotate90; break;
        case 7: rot = WICBitmapTransformRotate270; mirror = true; break;
        case 8: rot = WICBitmapTransformRotate270; break;
        default: break;
    }
    for (const auto opt : {rot, mirror ? WICBitmapTransformFlipHorizontal : WICBitmapTransformRotate0}) {
        if (opt == WICBitmapTransformRotate0) continue;
        wil::com_ptr<IWICBitmapFlipRotator> fr;
        if (FAILED(wic->CreateBitmapFlipRotator(&fr)) || FAILED(fr->Initialize(src.get(), opt))) return std::nullopt;
        src = fr;
    }
    wil::com_ptr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv))) return std::nullopt;
    const WICPixelFormatGUID fmt = premultiplied ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA;
    if (FAILED(conv->Initialize(src.get(), fmt, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return std::nullopt;
    Raster r;
    if (FAILED(conv->GetSize(&r.w, &r.h)) || !r.w || !r.h) return std::nullopt;
    const size_t stride = static_cast<size_t>(r.w) * 4;
    if (stride * r.h > (1ull << 31)) return std::nullopt;
    r.pixels.resize(stride * r.h);
    if (FAILED(conv->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(r.pixels.size()), r.pixels.data()))) return std::nullopt;
    return r;
}

/// Shell thumbnails (Windows thumbnail cache / extractors) decoded on worker threads.
/// Device bitmaps are created on the UI thread and dropped when the graphics device changes.
class Thumbs {
public:
    /// `full` decodes the image itself (WIC) at up to `px` before falling back to the shell thumbnail: used for previews.
    explicit Thumbs(size_t max_bitmaps = 240, bool full = false) : max_bitmaps_{max_bitmaps}, full_{full} {}
    Thumbs(const Thumbs&) = delete;
    Thumbs& operator=(const Thumbs&) = delete;
    Thumbs(Thumbs&&) = delete;
    Thumbs& operator=(Thumbs&&) = delete;
    ~Thumbs() { stop(); }

    void start(HWND notify, UINT msg, unsigned threads = 3) {
        notify_ = notify;
        msg_ = msg;
        for (unsigned i = 0; i < threads; ++i) workers_.emplace_back([this](const std::stop_token& st) { worker(st); });
    }

    void stop() noexcept {
        {
            std::lock_guard lk{m_};
            stopping_ = true;
        }
        cv_.notify_all();
        workers_.clear();
    }

    /// Newest requests are served first; the queue is bounded so scrolled-past cards are dropped.
    void request(uint64_t id, const std::wstring& path, UINT px) {
        if (failed_.contains(id)) return;
        if (const auto it = bitmaps_.find(id); it != bitmaps_.end() && it->second.px >= px) return;
        {
            std::lock_guard lk{m_};
            if (!pending_.insert(id).second) return;
            queue_.push_front({id, path, px});
            while (queue_.size() > (full_ ? 6u : 64u)) {
                pending_.erase(queue_.back().id);
                queue_.pop_back();
            }
        }
        cv_.notify_one();
    }

    ID2D1Bitmap1* get(uint64_t id, UINT* out_px = nullptr) {
        const auto it = bitmaps_.find(id);
        if (it == bitmaps_.end()) return nullptr;
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        if (out_px) *out_px = it->second.px;
        return it->second.bmp.get();
    }

    [[nodiscard]] bool failed(uint64_t id) const { return failed_.contains(id); }

    /// UI thread: turn finished decodes into device bitmaps.
    bool process(ID2D1DeviceContext* dc) {
        std::vector<Done> done;
        {
            std::lock_guard lk{m_};
            done.swap(done_);
        }
        for (auto& d : done) {
            if (d.pixels.empty()) {
                failed_.insert(d.id);
                continue;
            }
            wil::com_ptr<ID2D1Bitmap1> bmp;
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(dc->CreateBitmap(D2D1::SizeU(d.w, d.h), d.pixels.data(), d.w * 4, &props, &bmp))) continue;
            if (const auto existing = bitmaps_.find(d.id); existing != bitmaps_.end()) {
                if (d.px >= existing->second.px) {
                    existing->second.bmp = std::move(bmp);
                    existing->second.px = d.px;
                }
                lru_.splice(lru_.begin(), lru_, existing->second.lru);
            } else {
                lru_.push_front(d.id);
                bitmaps_[d.id] = {std::move(bmp), lru_.begin(), d.px};
                while (bitmaps_.size() > max_bitmaps_) {
                    bitmaps_.erase(lru_.back());
                    lru_.pop_back();
                }
            }
        }
        return !done.empty();
    }

    /// Device lost / recreated: every bitmap belongs to the old device.
    void reset_gpu() noexcept {
        bitmaps_.clear();
        lru_.clear();
    }

    /// Drops every bitmap, queued request and failure (frees preview memory once the viewer closes).
    void clear() {
        {
            std::lock_guard lk{m_};
            for (const auto& q : queue_) pending_.erase(q.id);
            queue_.clear();
        }
        reset_gpu();
        failed_.clear();
    }

    void clear_failed() noexcept { failed_.clear(); }

private:
    size_t max_bitmaps_;
    bool full_;

    struct Req {
        uint64_t id = 0;
        std::wstring path;
        UINT px = 0;
    };
    struct Done {
        uint64_t id = 0;
        UINT w = 0, h = 0;
        UINT px = 0;
        std::vector<uint8_t> pixels;
    };
    struct Entry {
        wil::com_ptr<ID2D1Bitmap1> bmp;
        std::list<uint64_t>::iterator lru;
        UINT px = 0;
    };

    void worker(const std::stop_token& st) {
        ulp::win::COMWrapper<false> com{COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE};
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        while (!st.stop_requested()) {
            Req r;
            {
                std::unique_lock lk{m_};
                cv_.wait(lk, [&] { return stopping_ || !queue_.empty(); });
                if (stopping_) return;
                r = std::move(queue_.front());
                queue_.pop_front();
            }
            Done d{r.id};
            d.px = r.px;
            if (!full_ || !load_full(r, d)) load(r, d);
            {
                std::lock_guard lk{m_};
                pending_.erase(r.id);
                done_.push_back(std::move(d));
            }
            if (notify_) PostMessageW(notify_, msg_, 0, 0);
        }
    }

    /// The shell has no SVG thumbnailer of its own, so those are rasterised with resvg.
    static void load_svg(const Req& r, Done& d) {
        const auto raster = svg::render(r.path, r.px, r.px, svg::Fit::Contain);
        if (!raster) return;
        const std::span<const uint8_t> src = raster->rgba();
        d.pixels.resize(src.size());
        // Opaque RGBA -> BGRA
        for (size_t i = 0; i + 3 < src.size(); i += 4) {
            d.pixels[i] = src[i + 2];
            d.pixels[i + 1] = src[i + 1];
            d.pixels[i + 2] = src[i];
            d.pixels[i + 3] = src[i + 3];
        }
        d.w = raster->width;
        d.h = raster->height;
    }

    static bool load_full(const Req& r, Done& d) {
        if (svg::is_svg(r.path)) return false;
        auto img = decode_image(r.path, r.px, true);
        if (!img) return false;
        d.w = img->w;
        d.h = img->h;
        d.pixels = std::move(img->pixels);
        return true;
    }

    static void load(const Req& r, Done& d) {
        if (svg::is_svg(r.path)) {
            load_svg(r, d);
            return;
        }
        wil::com_ptr<IShellItemImageFactory> factory;
        if (FAILED(SHCreateItemFromParsingName(r.path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return;
        HBITMAP raw = nullptr;
        if (FAILED(factory->GetImage(SIZE{static_cast<LONG>(r.px), static_cast<LONG>(r.px)}, SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK, &raw)) || !raw) return;
        wil::unique_hbitmap bmp{raw};

        BITMAP info{};
        if (!GetObjectW(bmp.get(), sizeof(info), &info) || info.bmWidth <= 0 || info.bmHeight == 0) return;
        const UINT w = static_cast<UINT>(info.bmWidth);
        const UINT h = static_cast<UINT>(std::abs(info.bmHeight));
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = static_cast<LONG>(w);
        bi.bmiHeader.biHeight = -static_cast<LONG>(h);
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        d.pixels.resize(static_cast<size_t>(w) * h * 4);
        wil::unique_hdc_window screen{GetDC(nullptr)};
        if (GetDIBits(screen.get(), bmp.get(), 0, h, d.pixels.data(), &bi, DIB_RGB_COLORS) != static_cast<int>(h)) {
            d.pixels.clear();
            return;
        }
        // Thumbnails of opaque formats come back with alpha = 0; extractor output with alpha is premultiplied
        bool any_alpha = false;
        for (size_t i = 3; i < d.pixels.size(); i += 4) {
            if (d.pixels[i]) {
                any_alpha = true;
                break;
            }
        }
        if (!any_alpha) {
            for (size_t i = 3; i < d.pixels.size(); i += 4) d.pixels[i] = 255;
        }
        d.w = w;
        d.h = h;
    }

    HWND notify_ = nullptr;
    UINT msg_ = 0;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Req> queue_;
    std::unordered_set<uint64_t> pending_;
    std::vector<Done> done_;
    bool stopping_ = false;
    std::vector<std::jthread> workers_;

    std::unordered_map<uint64_t, Entry> bitmaps_;
    std::list<uint64_t> lru_;
    std::unordered_set<uint64_t> failed_;
};

} // namespace usyseek::ui
