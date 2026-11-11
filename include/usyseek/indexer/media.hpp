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

enum class ResizeMode { Squash, Shortest, Longest };

inline ResizeMode parse_resize_mode(std::string_view s) {
    if (s == "shortest") return ResizeMode::Shortest;
    if (s == "longest") return ResizeMode::Longest;
    return ResizeMode::Squash;
}

inline constexpr std::wstring_view IMAGE_EXTS[] = {
    L"jpg", L"jpeg", L"jpe", L"jfif", L"png", L"bmp", L"gif", L"tif", L"tiff", L"webp", L"heic", L"heif", L"avif", L"jxl", L"dng",
    L"svg", L"svgz",
};
inline constexpr std::wstring_view VIDEO_EXTS[] = {
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
    Decoder(uint32_t size, ResizeMode mode) : size_{size}, mode_{mode} {
        CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    }

    std::optional<Decoded> image(const std::wstring& path) {
        if (svg::is_svg(path)) return vector_image(path);
        if (!wic_) return std::nullopt;
        wil::com_ptr<IWICBitmapDecoder> dec;
        if (FAILED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return std::nullopt;
        wil::com_ptr<IWICBitmapFrameDecode> frame;
        if (FAILED(dec->GetFrame(0, &frame))) return std::nullopt;

        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h)) || w < 32 || h < 32) return std::nullopt;

        const WICBitmapTransformOptions orient = orientation(frame.get());
        const bool swapped = (orient & WICBitmapTransformRotate90) != 0;

        Decoded out;
        out.width = swapped ? h : w;
        out.height = swapped ? w : h;
        Frame f;
        if (!resize(frame.get(), w, h, orient, f)) return std::nullopt;
        out.frames.push_back(std::move(f));
        return out;
    }

    std::optional<Decoded> video(const std::wstring& path, uint32_t frame_count) {
        wil::com_ptr<IMFAttributes> attr;
        if (FAILED(MFCreateAttributes(&attr, 2))) return std::nullopt;
        attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        wil::com_ptr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attr.get(), &reader))) return std::nullopt;

        const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        if (FAILED(reader->SetStreamSelection(stream, TRUE))) return std::nullopt;

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
        if (FAILED(reader->SetCurrentMediaType(stream, nullptr, type.get()))) return std::nullopt;

        wil::com_ptr<IMFMediaType> current;
        if (FAILED(reader->GetCurrentMediaType(stream, &current))) return std::nullopt;
        UINT32 w = 0, h = 0;
        if (FAILED(MFGetAttributeSize(current.get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0) return std::nullopt;

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
        std::vector<uint8_t> bgrx(static_cast<size_t>(w) * h * 4);
        for (uint32_t k = 0; k < n; ++k) {
            if (duration > 0) {
                PROPVARIANT pos;
                InitPropVariantFromInt64(static_cast<LONGLONG>(duration * (2ull * k + 1) / (2ull * n)), &pos);
                const HRESULT hr = reader->SetCurrentPosition(GUID_NULL, pos);
                PropVariantClear(&pos);
                if (FAILED(hr)) break;
            }
            wil::com_ptr<IMFSample> sample;
            for (int tries = 0; tries < 16 && !sample; ++tries) {
                DWORD flags = 0;
                LONGLONG ts = 0;
                if (FAILED(reader->ReadSample(stream, 0, nullptr, &flags, &ts, &sample))) break;
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            }
            if (!sample || !copy_sample(sample.get(), w, h, bgrx)) continue;

            wil::com_ptr<IWICBitmap> bmp;
            if (FAILED(wic_->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGR, w * 4, static_cast<UINT>(bgrx.size()), bgrx.data(), &bmp))) continue;
            Frame f;
            if (!resize(bmp.get(), w, h, orient, f)) continue;
            if (mean_luma(f.rgb) < 5.0) continue; // black frames (fades, intros) poison the average
            out.frames.push_back(std::move(f));
        }
        if (out.frames.empty()) return std::nullopt;
        return out;
    }

private:
    /// SVG has no pixels of its own, so resvg rasterises it straight at the network input size.
    [[nodiscard]] std::optional<Decoded> vector_image(const std::wstring& path) const {
        const svg::Fit fit = mode_ == ResizeMode::Squash ? svg::Fit::Stretch
                           : mode_ == ResizeMode::Shortest ? svg::Fit::Cover : svg::Fit::Contain;
        const auto raster = svg::render(path, size_, size_, fit);
        if (!raster || raster->width > size_ || raster->height > size_) return std::nullopt;

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

    static bool copy_sample(IMFSample* sample, UINT32 w, UINT32 h, std::vector<uint8_t>& dst) {
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

    static double mean_luma(const std::vector<uint8_t>& rgb) {
        constexpr size_t step = 21; // every 7th RGB pixel is plenty
        uint64_t sum = 0;
        for (size_t i = 0; i + 2 < rgb.size(); i += step) sum += (rgb[i] * 77u + rgb[i + 1] * 150u + rgb[i + 2] * 29u) >> 8;
        const size_t n = rgb.size() / step;
        return n ? static_cast<double>(sum) / static_cast<double>(n) : 0.0;
    }

    static WICBitmapTransformOptions orientation(IWICBitmapFrameDecode* frame) {
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

        out.rgb.assign(static_cast<size_t>(S) * S * 3, 0);
        const UINT stride = S * 3;
        if (mode_ == ResizeMode::Longest) {
            const size_t x0 = (S - sw) / 2, y0 = (S - sh) / 2;
            const size_t row = static_cast<size_t>(sw) * 3;
            std::vector<uint8_t> tmp(row * sh);
            if (FAILED(final_src->CopyPixels(nullptr, sw * 3, static_cast<UINT>(tmp.size()), tmp.data()))) return false;
            for (size_t y = 0; y < sh; ++y) std::memcpy(out.rgb.data() + (y0 + y) * stride + x0 * 3, tmp.data() + y * row, row);
            return true;
        }
        const WICRect rc{static_cast<INT>((sw - S) / 2), static_cast<INT>((sh - S) / 2), static_cast<INT>(S), static_cast<INT>(S)};
        return SUCCEEDED(final_src->CopyPixels(&rc, stride, static_cast<UINT>(out.rgb.size()), out.rgb.data()));
    }

    wil::com_ptr<IWICImagingFactory> wic_;
    uint32_t size_;
    ResizeMode mode_;
};

} // namespace usyseek::media
