#pragma once

#include <usyseek/common.hpp>

#include <d2d1_3.h>
#include <d2d1_3helper.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite_3.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <wil/com.h>

#include <functional>
#include <unordered_map>

namespace usyseek::ui {

/// Process-wide graphics device shared by every window. Recreated wholesale on device loss or when
/// the adapter set changes (eGPU unplug, driver update, TDR); `epoch()` bumps on every recreation so
/// owners of device-bound resources (bitmaps) know to drop them.
class Gfx {
public:
    bool init() {
        D2D1_FACTORY_OPTIONS opts{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory7), &opts, reinterpret_cast<void**>(&d2d_factory_)))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3), reinterpret_cast<IUnknown**>(&dwrite_)))) return false;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_)))) return false;
        return create_device(nullptr);
    }

    [[nodiscard]] ID2D1Factory7* d2d_factory() const noexcept { return d2d_factory_.get(); }
    [[nodiscard]] IDWriteFactory3* dwrite() const noexcept { return dwrite_.get(); }
    [[nodiscard]] IWICImagingFactory* wic() const noexcept { return wic_.get(); }
    [[nodiscard]] ID3D11Device* d3d() const noexcept { return d3d_.get(); }
    [[nodiscard]] ID3D11DeviceContext* d3d_context() const noexcept { return d3d_ctx_.get(); }
    [[nodiscard]] IDXGIFactory2* dxgi_factory() const noexcept { return dxgi_factory_.get(); }
    [[nodiscard]] IDXGIDevice* dxgi_device() const noexcept { return dxgi_device_.get(); }
    [[nodiscard]] ID2D1Device6* d2d_device() const noexcept { return d2d_device_.get(); }
    [[nodiscard]] uint64_t epoch() const noexcept { return epoch_; }
    [[nodiscard]] bool ready() const noexcept { return d2d_device_ != nullptr; }

    /// Called by windows when presenting fails or the adapter may have changed.
    bool recover(HWND hint) {
        return create_device(hint);
    }

    [[nodiscard]] bool device_lost() const noexcept {
        return !d3d_ || FAILED(d3d_->GetDeviceRemovedReason());
    }

    /// True when the DXGI adapter list changed or `hwnd` now sits on another GPU's monitor.
    [[nodiscard]] bool adapter_stale(HWND hwnd) const {
        if (!dxgi_factory_ || !dxgi_factory_->IsCurrent()) return true;
        if (warp_) return false;
        if (auto a = window_adapter(dxgi_factory_.get(), hwnd)) {
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(a->GetDesc1(&d))) return d.AdapterLuid.LowPart != luid_.LowPart || d.AdapterLuid.HighPart != luid_.HighPart;
        }
        return false;
    }

    std::function<void()> on_device_changed;

private:
    static wil::com_ptr<IDXGIAdapter1> window_adapter(IDXGIFactory2* f, HWND hwnd) {
        if (!f || !hwnd) return nullptr;
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        for (UINT i = 0;; ++i) {
            wil::com_ptr<IDXGIAdapter1> a;
            if (f->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            for (UINT j = 0;; ++j) {
                wil::com_ptr<IDXGIOutput> o;
                if (a->EnumOutputs(j, &o) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC od{};
                if (SUCCEEDED(o->GetDesc(&od)) && od.Monitor == mon) return a;
            }
        }
        return nullptr;
    }

    bool create_device(HWND hint) {
        d2d_device_.reset();
        dxgi_device_.reset();
        if (d3d_ctx_) {
            d3d_ctx_->ClearState();
            d3d_ctx_->Flush();
        }
        d3d_ctx_.reset();
        d3d_.reset();
        dxgi_factory_.reset();

        wil::com_ptr<IDXGIFactory2> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        wil::com_ptr<IDXGIAdapter1> adapter = window_adapter(factory.get(), hint);
        if (!adapter) factory->EnumAdapters1(0, &adapter);

        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = E_FAIL;
        warp_ = false;
        if (adapter) hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 4, D3D11_SDK_VERSION, &d3d_, nullptr, &d3d_ctx_);
        if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 4, D3D11_SDK_VERSION, &d3d_, nullptr, &d3d_ctx_);
        if (FAILED(hr)) {
            warp_ = true;
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 4, D3D11_SDK_VERSION, &d3d_, nullptr, &d3d_ctx_);
        }
        if (FAILED(hr)) return false;

        if (FAILED(d3d_->QueryInterface(IID_PPV_ARGS(&dxgi_device_)))) return false;
        if (auto d1 = dxgi_device_.try_query<IDXGIDevice1>()) d1->SetMaximumFrameLatency(1);

        wil::com_ptr<IDXGIAdapter> used;
        if (SUCCEEDED(dxgi_device_->GetAdapter(&used))) {
            DXGI_ADAPTER_DESC d{};
            if (SUCCEEDED(used->GetDesc(&d))) luid_ = d.AdapterLuid;
        }
        dxgi_factory_ = std::move(factory);

        if (FAILED(d2d_factory_->CreateDevice(dxgi_device_.get(), &d2d_device_))) return false;
        // Keep the D2D glyph/bitmap caches small - this process should stay light
        d2d_device_->SetMaximumTextureMemory(32ull << 20);
        ++epoch_;
        if (on_device_changed) on_device_changed();
        return true;
    }

    wil::com_ptr<ID2D1Factory7> d2d_factory_;
    wil::com_ptr<IDWriteFactory3> dwrite_;
    wil::com_ptr<IWICImagingFactory> wic_;
    wil::com_ptr<ID3D11Device> d3d_;
    wil::com_ptr<ID3D11DeviceContext> d3d_ctx_;
    wil::com_ptr<IDXGIFactory2> dxgi_factory_;
    wil::com_ptr<IDXGIDevice> dxgi_device_;
    wil::com_ptr<ID2D1Device6> d2d_device_;
    LUID luid_{};
    bool warp_ = false;
    uint64_t epoch_ = 0;
};

/// Per-window swapchain composed through DirectComposition (premultiplied alpha so DWM backdrops show through).
class WindowTarget {
public:
    bool attach(Gfx& gfx, HWND hwnd) {
        gfx_ = &gfx;
        hwnd_ = hwnd;
        return rebuild();
    }

    bool rebuild() {
        release();
        if (!gfx_ || !gfx_->ready()) return false;
        if (FAILED(gfx_->d2d_device()->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS, &dc_))) return false;

        RECT rc{};
        GetClientRect(hwnd_, &rc);
        w_ = static_cast<UINT>(std::max(1L, rc.right - rc.left));
        h_ = static_cast<UINT>(std::max(1L, rc.bottom - rc.top));

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = w_;
        desc.Height = h_;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(gfx_->dxgi_factory()->CreateSwapChainForComposition(gfx_->d3d(), &desc, nullptr, &swap_))) return false;
        if (FAILED(DCompositionCreateDevice(gfx_->dxgi_device(), IID_PPV_ARGS(&dcomp_)))) return false;
        if (FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dcomp_target_))) return false;
        if (FAILED(dcomp_->CreateVisual(&visual_))) return false;
        visual_->SetContent(swap_.get());
        dcomp_target_->SetRoot(visual_.get());
        dcomp_->Commit();

        dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));
        if (dpi_ <= 0) dpi_ = 96.0f;
        if (!create_target()) return false;
        epoch_ = gfx_->epoch();
        return true;
    }

    void release() {
        if (dc_) dc_->SetTarget(nullptr);
        target_.reset();
        visual_.reset();
        dcomp_target_.reset();
        dcomp_.reset();
        swap_.reset();
        dc_.reset();
    }

    [[nodiscard]] bool ready() const noexcept { return dc_ && target_ && gfx_ && epoch_ == gfx_->epoch(); }
    [[nodiscard]] ID2D1DeviceContext6* dc() const noexcept { return dc_.get(); }
    [[nodiscard]] float scale() const noexcept { return dpi_ / 96.0f; }
    [[nodiscard]] D2D1_SIZE_F size() const noexcept { return {static_cast<float>(w_) / scale(), static_cast<float>(h_) / scale()}; }

    bool resize(UINT w, UINT h) {
        if (w == 0 || h == 0) return ready();
        const float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
        if (!ready()) return rebuild();
        if (w == w_ && h == h_ && dpi == dpi_) return true;
        dpi_ = dpi > 0 ? dpi : 96.0f;
        dc_->SetTarget(nullptr);
        target_.reset();
        if (FAILED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return rebuild();
        w_ = w;
        h_ = h;
        return create_target();
    }

    /// Ends the frame and presents. Returns false if the device must be recovered.
    bool present(bool vsync = true) {
        HRESULT hr = dc_->EndDraw();
        if (SUCCEEDED(hr) && capture_) {
            capture_ok_ = write_png(*capture_);
            capture_.reset();
        }
        if (SUCCEEDED(hr)) hr = swap_->Present(vsync ? 1 : 0, 0);
        return SUCCEEDED(hr) || hr == DXGI_STATUS_OCCLUDED;
    }

    void request_capture(fs::path p) {
        capture_ = std::move(p);
        capture_ok_ = false;
    }
    [[nodiscard]] bool capture_ok() const noexcept { return capture_ok_; }

private:
    bool create_target() {
        wil::com_ptr<IDXGISurface> surface;
        if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi_, dpi_);
        if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.get(), &props, &target_))) return false;
        dc_->SetTarget(target_.get());
        dc_->SetDpi(dpi_, dpi_);
        dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        return true;
    }

    bool write_png(const fs::path& path) {
        wil::com_ptr<ID3D11Texture2D> back;
        if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
        D3D11_TEXTURE2D_DESC desc{};
        back->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        wil::com_ptr<ID3D11Texture2D> staging;
        if (FAILED(gfx_->d3d()->CreateTexture2D(&desc, nullptr, &staging))) return false;
        gfx_->d3d_context()->CopyResource(staging.get(), back.get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(gfx_->d3d_context()->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) return false;

        // Composite over a dark backdrop so translucent areas read correctly in the PNG
        std::vector<uint8_t> px(static_cast<size_t>(desc.Width) * desc.Height * 4);
        for (UINT y = 0; y < desc.Height; ++y) {
            const uint8_t* src = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
            uint8_t* dst = px.data() + static_cast<size_t>(y) * desc.Width * 4;
            for (UINT x = 0; x < desc.Width; ++x) {
                const uint8_t a = src[x * 4 + 3];
                const uint8_t bg[3] = {0x1a, 0x18, 0x1c};
                for (int c = 0; c < 3; ++c) dst[x * 4 + c] = static_cast<uint8_t>(src[x * 4 + c] + bg[c] * (255 - a) / 255);
                dst[x * 4 + 3] = 255;
            }
        }
        gfx_->d3d_context()->Unmap(staging.get(), 0);

        IWICImagingFactory* wic = gfx_->wic();
        wil::com_ptr<IWICStream> stream;
        if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
        wil::com_ptr<IWICBitmapEncoder> enc;
        if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;
        wil::com_ptr<IWICBitmapFrameEncode> frame;
        if (FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return false;
        frame->SetSize(desc.Width, desc.Height);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        frame->SetPixelFormat(&fmt);
        if (FAILED(frame->WritePixels(desc.Height, desc.Width * 4, static_cast<UINT>(px.size()), px.data()))) return false;
        return SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    }

    Gfx* gfx_ = nullptr;
    HWND hwnd_ = nullptr;
    wil::com_ptr<ID2D1DeviceContext6> dc_;
    wil::com_ptr<ID2D1Bitmap1> target_;
    wil::com_ptr<IDXGISwapChain1> swap_;
    wil::com_ptr<IDCompositionDevice> dcomp_;
    wil::com_ptr<IDCompositionTarget> dcomp_target_;
    wil::com_ptr<IDCompositionVisual> visual_;
    UINT w_ = 0, h_ = 0;
    float dpi_ = 96.0f;
    uint64_t epoch_ = 0;
    std::optional<fs::path> capture_;
    bool capture_ok_ = false;
};

} // namespace usyseek::ui
