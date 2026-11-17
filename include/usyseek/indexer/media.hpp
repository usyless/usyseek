#pragma once

#include <usyseek/common.hpp>
#include <usyseek/svg.hpp>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <wil/com.h>

#include <cmath>

namespace usyseek::media {

enum class ResizeMode : uint8_t { Squash, Shortest, Longest };

inline ResizeMode parse_resize_mode(std::string_view s) noexcept {
    if (s == "shortest") return ResizeMode::Shortest;
    if (s == "longest") return ResizeMode::Longest;
    return ResizeMode::Squash;
}

inline constexpr std::array<std::wstring_view, 17> IMAGE_EXTS = {
    L"jpg", L"jpeg", L"jpe", L"jfif", L"png", L"bmp", L"gif", L"tif", L"tiff", L"webp", L"heic", L"heif", L"avif", L"jxl", L"dng",
    L"svg", L"svgz",
};
inline constexpr std::array<std::wstring_view, 12> VIDEO_EXTS = {
    L"mp4", L"m4v", L"mov", L"mkv", L"webm", L"avi", L"wmv", L"mpg", L"mpeg", L"ts", L"m2ts", L"3gp",
};

inline int kind_of(std::wstring_view path) {
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos) return -1;
    const std::wstring ext = lower(path.substr(dot + 1));
    for (auto e : IMAGE_EXTS) if (ext == e) return 0;
    for (auto e : VIDEO_EXTS) if (ext == e) return 1;
    return -1;
}

/// One network-ready input: size x size x 3 RGB bytes.
struct Frame {
    std::vector<uint8_t> rgb;
};

struct Decoded {
    std::vector<Frame> frames;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t duration_ms = 0;
};

/// Per-thread decoder (owns its WIC factory; call from a COM MTA thread).
class Decoder {
public:
    Decoder(uint32_t size, ResizeMode mode) noexcept : size_{size}, mode_{mode} {
        CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    }

    std::optional<Decoded> image(const std::wstring& path, uint32_t frame_count = 1, std::wstring* reason = nullptr) {
        auto fail = [&](std::wstring_view msg) -> std::optional<Decoded> {
            if (reason) *reason = std::wstring{msg};
            return std::nullopt;
        };
        if (svg::is_svg(path)) return vector_image(path, reason);
        if (!wic_) return fail(L"WIC imaging factory unavailable");
        wil::com_ptr<IWICBitmapDecoder> dec;
        if (FAILED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) {
            return fail(L"Unsupported image codec or unreadable file");
        }

        if (frame_count > 1) {
            GUID container{};
            UINT count = 0;
            if (SUCCEEDED(dec->GetContainerFormat(&container)) && container == GUID_ContainerFormatGif &&
                SUCCEEDED(dec->GetFrameCount(&count)) && count > 1) {
                auto gif = animated_gif(dec.get(), count, frame_count);
                if (!gif && reason) *reason = L"GIF frames could not be decoded";
                return gif;
            }
        }

        wil::com_ptr<IWICBitmapFrameDecode> frame;
        if (FAILED(dec->GetFrame(0, &frame))) return fail(L"Failed to read image frame");

        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h))) return fail(L"Failed to read image dimensions");
        if (w < 32 || h < 32) return fail(L"Image smaller than 32x32 minimum");

        const WICBitmapTransformOptions orient = orientation(frame.get());
        const bool swapped = (orient & WICBitmapTransformRotate90) != 0;

        Decoded out;
        out.width = swapped ? h : w;
        out.height = swapped ? w : h;
        Frame f;
        if (!resize(frame.get(), w, h, orient, f)) return fail(L"Image pixel conversion/resize failed");
        out.frames.push_back(std::move(f));
        return out;
    }

    std::optional<Decoded> video(const std::wstring& path, uint32_t frame_count, std::wstring* reason = nullptr) {
        auto fail = [&](std::wstring_view msg) -> std::optional<Decoded> {
            if (reason) *reason = std::wstring{msg};
            return std::nullopt;
        };
        if (!wic_) return fail(L"WIC imaging factory unavailable");
        wil::com_ptr<IMFAttributes> attr;
        if (FAILED(MFCreateAttributes(&attr, 2))) return fail(L"Media Foundation attributes failed");
        attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        wil::com_ptr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attr.get(), &reader))) {
            return fail(L"Unsupported video container or unreadable file");
        }

        const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        if (FAILED(reader->SetStreamSelection(stream, TRUE))) return fail(L"No video stream found");

        UINT32 rotation = 0;
        {
            wil::com_ptr<IMFMediaType> native;
            if (SUCCEEDED(reader->GetNativeMediaType(stream, 0, &native))) {
                rotation = MFGetAttributeUINT32(native.get(), MF_MT_VIDEO_ROTATION, 0);
            }
        }

        wil::com_ptr<IMFMediaType> type;
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (FAILED(reader->SetCurrentMediaType(stream, nullptr, type.get()))) return fail(L"Unsupported video codec (missing decoder)");

        wil::com_ptr<IMFMediaType> current;
        if (FAILED(reader->GetCurrentMediaType(stream, &current))) return fail(L"Could not query video stream format");
        UINT32 w = 0, h = 0;
        if (FAILED(MFGetAttributeSize(current.get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0) {
            return fail(L"Invalid video frame dimensions");
        }

        PROPVARIANT dur;
        PropVariantInit(&dur);
        uint64_t duration = 0;
        if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &dur))) {
            duration = dur.uhVal.QuadPart;
        }
        PropVariantClear(&dur);

        WICBitmapTransformOptions orient = WICBitmapTransformRotate0;
        if (rotation == 90) orient = WICBitmapTransformRotate90;
        else if (rotation == 180) orient = WICBitmapTransformRotate180;
        else if (rotation == 270) orient = WICBitmapTransformRotate270;
        const bool swapped = rotation == 90 || rotation == 270;

        Decoded out;
        out.width = swapped ? h : w;
        out.height = swapped ? w : h;
        out.duration_ms = duration / 10'000;

        const uint32_t n = duration > 0 ? std::max(1u, frame_count) : 1;
        bgrx_buf_.resize(static_cast<size_t>(w) * h * 4);
        bool saw_frame = false;
        bool seek_failed = false;
        for (uint32_t k = 0; k < n; ++k) {
            if (duration > 0) {
                PROPVARIANT pos;
                InitPropVariantFromInt64(static_cast<LONGLONG>(duration * (2ull * k + 1) / (2ull * n)), &pos);
                const HRESULT hr = reader->SetCurrentPosition(GUID_NULL, pos);
                PropVariantClear(&pos);
                if (FAILED(hr)) {
                    seek_failed = true;
                    break;
                }
            }
            wil::com_ptr<IMFSample> sample;
            for (int tries = 0; tries < 16 && !sample; ++tries) {
                DWORD flags = 0;
                LONGLONG ts = 0;
                if (FAILED(reader->ReadSample(stream, 0, nullptr, &flags, &ts, &sample))) break;
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            }
            if (!sample || !copy_sample(sample.get(), w, h, bgrx_buf_)) continue;

            wil::com_ptr<IWICBitmap> bmp;
            if (FAILED(wic_->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGR, w * 4, static_cast<UINT>(bgrx_buf_.size()), bgrx_buf_.data(), &bmp))) continue;
            Frame f;
            if (!resize(bmp.get(), w, h, orient, f)) continue;
            saw_frame = true;
            if (mean_luma(f.rgb) < 5.0) continue; // black frames (fades, intros) poison the average
            out.frames.push_back(std::move(f));
        }
        if (out.frames.empty()) {
            if (seek_failed) return fail(L"Video seek failed while sampling frames");
            if (saw_frame) return fail(L"All sampled video frames were blank/black");
            return fail(L"No decodable video frames found");
        }
        return out;
    }

private:
    static bool query_u16(IWICMetadataQueryReader* q, const wchar_t* name, UINT& out) noexcept {
        if (!q) return false;
        PROPVARIANT v;
        PropVariantInit(&v);
        const bool ok = SUCCEEDED(q->GetMetadataByName(name, &v)) && (v.vt == VT_UI2 || v.vt == VT_UI1);
        if (ok) out = (v.vt == VT_UI2) ? v.uiVal : v.bVal;
        PropVariantClear(&v);
        return ok;
    }

    std::optional<Decoded> animated_gif(IWICBitmapDecoder* dec, UINT total, uint32_t frame_count) {
        UINT cw = 0, ch = 0;
        {
            wil::com_ptr<IWICMetadataQueryReader> gmeta;
            if (SUCCEEDED(dec->GetMetadataQueryReader(&gmeta))) {
                query_u16(gmeta.get(), L"/logscrdesc/Width", cw);
                query_u16(gmeta.get(), L"/logscrdesc/Height", ch);
            }
        }
        if (cw == 0 || ch == 0) {
            wil::com_ptr<IWICBitmapFrameDecode> f0;
            if (FAILED(dec->GetFrame(0, &f0)) || FAILED(f0->GetSize(&cw, &ch))) return std::nullopt;
        }
        if (cw < 32 || ch < 32 || static_cast<uint64_t>(cw) * ch * 4 > (512ull << 20)) return std::nullopt;

        const UINT n = std::min<UINT>(total, std::max(1u, frame_count));
        std::vector<UINT> targets;
        targets.reserve(n);
        if (n == total) {
            for (UINT i = 0; i < total; ++i) targets.push_back(i);
        } else {
            for (uint32_t k = 0; k < n; ++k) {
                const UINT idx = static_cast<UINT>((static_cast<uint64_t>(total) * (2ull * k + 1)) / (2ull * n));
                if (targets.empty() || idx > targets.back()) targets.push_back(std::min(idx, total - 1));
            }
        }
        if (targets.empty()) return std::nullopt;

        gif_canvas_.assign(static_cast<size_t>(cw) * ch * 4, 0);
        std::vector<Frame> sampled;
        std::vector<double> lumas;
        sampled.reserve(targets.size());
        lumas.reserve(targets.size());

        size_t next_target = 0;
        for (UINT i = 0; i <= targets.back(); ++i) {
            while (next_target < targets.size() && targets[next_target] < i) ++next_target;
            wil::com_ptr<IWICBitmapFrameDecode> frame;
            if (FAILED(dec->GetFrame(i, &frame))) continue;
            UINT fw = 0, fh = 0;
            if (FAILED(frame->GetSize(&fw, &fh)) || fw == 0 || fh == 0) continue;

            UINT left = 0, top = 0, disposal = 0;
            {
                wil::com_ptr<IWICMetadataQueryReader> fmeta;
                if (SUCCEEDED(frame->GetMetadataQueryReader(&fmeta))) {
                    query_u16(fmeta.get(), L"/imgdesc/Left", left);
                    query_u16(fmeta.get(), L"/imgdesc/Top", top);
                    query_u16(fmeta.get(), L"/grctlext/Disposal", disposal);
                }
            }

            if (disposal == 3) gif_prev_canvas_ = gif_canvas_;

            wil::com_ptr<IWICFormatConverter> conv;
            if (FAILED(wic_->CreateFormatConverter(&conv))) continue;
            if (FAILED(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) continue;
            const size_t fstride = static_cast<size_t>(fw) * 4;
            if (static_cast<uint64_t>(fstride) * fh > (512ull << 20)) continue;
            frame_buf_.resize(fstride * fh);
            if (FAILED(conv->CopyPixels(nullptr, static_cast<UINT>(fstride), static_cast<UINT>(frame_buf_.size()), frame_buf_.data()))) continue;

            const UINT copy_w = left < cw ? std::min(fw, cw - left) : 0;
            const UINT copy_h = top < ch ? std::min(fh, ch - top) : 0;
            for (UINT y = 0; y < copy_h; ++y) {
                const uint8_t* src = frame_buf_.data() + static_cast<size_t>(y) * fstride;
                uint8_t* dst = gif_canvas_.data() + (static_cast<size_t>(top + y) * cw + left) * 4;
                for (UINT x = 0; x < copy_w; ++x, src += 4, dst += 4) {
                    const uint8_t a = src[3];
                    if (a == 255) {
                        std::memcpy(dst, src, 4);
                    } else if (a > 0) {
                        const uint32_t inv = 255u - a;
                        dst[0] = static_cast<uint8_t>(std::min<uint32_t>(255, src[0] + (dst[0] * inv + 127) / 255));
                        dst[1] = static_cast<uint8_t>(std::min<uint32_t>(255, src[1] + (dst[1] * inv + 127) / 255));
                        dst[2] = static_cast<uint8_t>(std::min<uint32_t>(255, src[2] + (dst[2] * inv + 127) / 255));
                        dst[3] = static_cast<uint8_t>(std::min<uint32_t>(255, a + (dst[3] * inv + 127) / 255));
                    }
                }
            }

            if (next_target < targets.size() && i == targets[next_target]) {
                ++next_target;
                wil::com_ptr<IWICBitmap> bmp;
                if (SUCCEEDED(wic_->CreateBitmapFromMemory(cw, ch, GUID_WICPixelFormat32bppPBGRA, cw * 4, static_cast<UINT>(gif_canvas_.size()), gif_canvas_.data(), &bmp))) {
                    Frame f;
                    if (resize(bmp.get(), cw, ch, WICBitmapTransformRotate0, f)) {
                        lumas.push_back(mean_luma(f.rgb));
                        sampled.push_back(std::move(f));
                    }
                }
            }

            if (disposal == 2) {
                for (UINT y = 0; y < copy_h; ++y) {
                    std::memset(gif_canvas_.data() + (static_cast<size_t>(top + y) * cw + left) * 4, 0, static_cast<size_t>(copy_w) * 4);
                }
            } else if (disposal == 3) {
                gif_canvas_.swap(gif_prev_canvas_);
            }
        }

        if (sampled.empty()) return std::nullopt;
        Decoded out;
        out.width = cw;
        out.height = ch;
        if (!std::ranges::any_of(lumas, [](double l) { return l >= 5.0; })) {
            out.frames = std::move(sampled);
        } else {
            for (size_t k = 0; k < sampled.size(); ++k) {
                if (lumas[k] >= 5.0) out.frames.push_back(std::move(sampled[k]));
            }
        }
        return out;
    }

    /// SVG has no pixels of its own, so resvg rasterises it straight at the network input size.
    [[nodiscard]] std::optional<Decoded> vector_image(const std::wstring& path, std::wstring* reason = nullptr) const {
        const svg::Fit fit = mode_ == ResizeMode::Squash ? svg::Fit::Stretch
                           : mode_ == ResizeMode::Shortest ? svg::Fit::Cover : svg::Fit::Contain;
        std::string svg_err;
        const auto raster = svg::render(path, size_, size_, fit, svg::WHITE, &svg_err);
        if (!raster || raster->width > size_ || raster->height > size_) {
            if (reason) *reason = svg_err.empty() ? L"SVG rasterization failed" : to_wide(svg_err);
            return std::nullopt;
        }

        Decoded out;
        out.width = raster->src_width;
        out.height = raster->src_height;
        Frame f;
        f.rgb.assign(static_cast<size_t>(size_) * size_ * 3, 0);
        // Contain leaves the bitmap smaller than the input: centre it, like raster images in Longest mode
        const size_t x0 = (size_ - raster->width) / 2, y0 = (size_ - raster->height) / 2;
        const std::span<const uint8_t> src = raster->rgba();
        for (size_t y = 0; y < raster->height; ++y) {
            const uint8_t* s = src.data() + y * raster->width * 4;
            uint8_t* d = f.rgb.data() + ((y0 + y) * size_ + x0) * 3;
            for (size_t x = 0; x < raster->width; ++x, s += 4, d += 3) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            }
        }
        out.frames.push_back(std::move(f));
        return out;
    }

    static bool copy_sample(IMFSample* sample, UINT32 w, UINT32 h, std::vector<uint8_t>& dst) noexcept {
        wil::com_ptr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return false;
        const size_t row = static_cast<size_t>(w) * 4;
        if (auto b2d = buf.try_query<IMF2DBuffer>()) {
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(b2d->Lock2D(&scan0, &pitch))) {
                for (UINT32 y = 0; y < h; ++y) std::memcpy(dst.data() + y * row, scan0 + static_cast<ptrdiff_t>(y) * pitch, row);
                b2d->Unlock2D();
                return true;
            }
        }
        BYTE* data = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&data, nullptr, &len))) return false;
        const bool ok = len >= row * h;
        if (ok) {
            for (UINT32 y = 0; y < h; ++y) std::memcpy(dst.data() + y * row, data + y * row, row);
        }
        buf->Unlock();
        return ok;
    }

    static double mean_luma(std::span<const uint8_t> rgb) noexcept {
        constexpr size_t step = 21; // every 7th RGB pixel is plenty
        uint64_t sum = 0;
        for (size_t i = 0; i + 2 < rgb.size(); i += step) sum += (rgb[i] * 77u + rgb[i + 1] * 150u + rgb[i + 2] * 29u) >> 8;
        const size_t n = rgb.size() / step;
        return n ? static_cast<double>(sum) / static_cast<double>(n) : 0.0;
    }

    static WICBitmapTransformOptions orientation(IWICBitmapFrameDecode* frame) noexcept {
        wil::com_ptr<IWICMetadataQueryReader> q;
        if (FAILED(frame->GetMetadataQueryReader(&q))) return WICBitmapTransformRotate0;
        PROPVARIANT v;
        PropVariantInit(&v);
        UINT o = 1;
        if (SUCCEEDED(q->GetMetadataByName(L"System.Photo.Orientation", &v)) && v.vt == VT_UI2) o = v.uiVal;
        PropVariantClear(&v);
        switch (o) {
            case 2: return WICBitmapTransformFlipHorizontal;
            case 3: return WICBitmapTransformRotate180;
            case 4: return WICBitmapTransformFlipVertical;
            case 5: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
            case 6: return WICBitmapTransformRotate90;
            case 7: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
            case 8: return WICBitmapTransformRotate270;
            default: return WICBitmapTransformRotate0;
        }
    }

    /// Scales (with the JPEG DCT fast path when the source supports it), orients, converts to RGB and crops/pads.
    bool resize(IWICBitmapSource* src, UINT w, UINT h, WICBitmapTransformOptions orient, Frame& out) {
        const bool swapped = (orient & WICBitmapTransformRotate90) != 0;
        const double ow = swapped ? h : w;
        const double oh = swapped ? w : h;
        const UINT S = size_;

        UINT sw = S, sh = S; // oriented scaled size
        if (mode_ == ResizeMode::Shortest || mode_ == ResizeMode::Longest) {
            const double scale = (mode_ == ResizeMode::Shortest) ? S / std::min(ow, oh) : S / std::max(ow, oh);
            sw = std::max(1u, static_cast<UINT>(std::lround(ow * scale)));
            sh = std::max(1u, static_cast<UINT>(std::lround(oh * scale)));
            if (mode_ == ResizeMode::Shortest) {
                sw = std::max(sw, S);
                sh = std::max(sh, S);
            } else {
                sw = std::min(sw, S);
                sh = std::min(sh, S);
            }
        }

        wil::com_ptr<IWICBitmapScaler> scaler;
        if (FAILED(wic_->CreateBitmapScaler(&scaler))) return false;
        HRESULT hr = scaler->Initialize(src, swapped ? sh : sw, swapped ? sw : sh, WICBitmapInterpolationModeHighQualityCubic);
        wil::com_ptr<IWICFormatConverter> pre;
        if (FAILED(hr)) {
            // Some pixel formats cannot be scaled directly
            if (FAILED(wic_->CreateFormatConverter(&pre))) return false;
            if (FAILED(pre->Initialize(src, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
            if (FAILED(scaler->Initialize(pre.get(), swapped ? sh : sw, swapped ? sw : sh, WICBitmapInterpolationModeHighQualityCubic))) return false;
        }

        wil::com_ptr<IWICFormatConverter> conv;
        if (FAILED(wic_->CreateFormatConverter(&conv))) return false;
        if (FAILED(conv->Initialize(scaler.get(), GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;

        IWICBitmapSource* final_src = conv.get();
        wil::com_ptr<IWICBitmapFlipRotator> rot;
        if (orient != WICBitmapTransformRotate0) {
            if (FAILED(wic_->CreateBitmapFlipRotator(&rot)) || FAILED(rot->Initialize(conv.get(), orient))) return false;
            final_src = rot.get();
        }

        const UINT stride = S * 3;
        if (mode_ == ResizeMode::Longest) {
            out.rgb.assign(static_cast<size_t>(S) * S * 3, 0);
            const size_t x0 = (S - sw) / 2, y0 = (S - sh) / 2;
            const size_t row = static_cast<size_t>(sw) * 3;
            resize_tmp_.resize(row * sh);
            if (FAILED(final_src->CopyPixels(nullptr, sw * 3, static_cast<UINT>(resize_tmp_.size()), resize_tmp_.data()))) return false;
            for (size_t y = 0; y < sh; ++y) std::memcpy(out.rgb.data() + (y0 + y) * stride + x0 * 3, resize_tmp_.data() + y * row, row);
            return true;
        }
        out.rgb.resize(static_cast<size_t>(S) * S * 3);
        const WICRect rc{static_cast<INT>((sw - S) / 2), static_cast<INT>((sh - S) / 2), static_cast<INT>(S), static_cast<INT>(S)};
        return SUCCEEDED(final_src->CopyPixels(&rc, stride, static_cast<UINT>(out.rgb.size()), out.rgb.data()));
    }

    wil::com_ptr<IWICImagingFactory> wic_;
    uint32_t size_;
    ResizeMode mode_;
    std::vector<uint8_t> bgrx_buf_;
    std::vector<uint8_t> gif_canvas_;
    std::vector<uint8_t> gif_prev_canvas_;
    std::vector<uint8_t> frame_buf_;
    std::vector<uint8_t> resize_tmp_;
};

} // namespace usyseek::media
