#pragma once

#include "canvas.hpp"
#include "liquid_glass.hpp"
#include "search.hpp"
#include "theme_loader.hpp"
#include "thumbs.hpp"

#include <usyseek/notifications.hpp>
#include <usyseek/progress.hpp>
#include <usyseek/status.hpp>

#include <atomic>
#include <dwmapi.h>
#include <shellapi.h>
#include <shldisp.h>
#include <windowsx.h>

namespace usyseek::ui {

inline constexpr UINT WM_APP_THUMBS = WM_APP + 10;
inline constexpr UINT WM_APP_RESULT = WM_APP + 11;
inline constexpr UINT WM_APP_CALL = WM_APP + 12;
inline constexpr UINT WM_APP_REDRAW = WM_APP + 13; // coalesced "something changed" from a worker thread
inline constexpr UINT WM_APP_GIF = WM_APP + 14;    // decoded animated GIF frame ready
inline constexpr UINT WM_APP_VIDEO = WM_APP + 15;  // Media Foundation video event ready

inline std::wstring format_hotkey(uint32_t mods, uint32_t vk) {
    std::wstring t;
    if (mods & MOD_CONTROL) t += L"Ctrl + ";
    if (mods & MOD_ALT) t += L"Alt + ";
    if (mods & MOD_SHIFT) t += L"Shift + ";
    if (mods & MOD_WIN) t += L"Win + ";
    wchar_t name[64]{};
    if (vk == VK_SPACE) wcscpy_s(name, L"Space");
    else GetKeyNameTextW(static_cast<LONG>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16, name, 64);
    return t + (name[0] ? name : L"Key");
}

/// State and services shared by the pill and the settings window (UI thread only unless noted).
struct Shared {
    Gfx gfx;
    DesktopDuplicator desktop_dupl;
    Fonts fonts;
    Settings settings;
    ThemeCatalog themes;
    Theme theme = theme_dark();
    std::optional<std::string> theme_override;
    Engine engine;
    Thumbs thumbs;
    Thumbs previews{4, true}; // full-size viewer images: the shown one and its neighbours

    std::vector<TagCount> tags;      // facet counts
    std::optional<uint64_t> db_count;
    IndexState index_state;
    std::vector<FailedFile> failed_files;
    std::vector<Notification> notifications;
    std::pair<int64_t, uint64_t> notifications_stamp_{0, 0};
    bool indexer_running = false;
    bool indexer_stopping = false;
    StatusData indexer{};
    wil::unique_event_nothrow stop_event_;

    std::atomic<HWND> call_hwnd{nullptr}; // window that runs marshalled callbacks
    bool test_mode = false;
    HotkeyState hotkey_state = HotkeyState::Unknown;
    int background_jobs = 0;         // downloads / installs in flight: the process must not idle-exit under them

    std::function<void()> open_settings;
    std::function<void()> open_notifications;
    std::function<void()> visibility_changed;
    std::function<void()> settings_changed; // repaint dependants
    std::function<void()> theme_changed;    // re-apply DWM attributes and repaint windows
    std::function<void()> tags_refreshed;
    std::function<void()> redraw_all;

    /// Runs `fn` on the UI thread (safe from any thread).
    void call(std::function<void()> fn) {
        const HWND hwnd = call_hwnd.load(std::memory_order_acquire);
        if (!hwnd) return;
        auto p = std::make_unique<std::function<void()>>(std::move(fn));
        if (!PostMessageW(hwnd, WM_APP_CALL, 0, reinterpret_cast<LPARAM>(p.get()))) return;
        (void)p.release();
    }

    void refresh_tags() {
        engine.post([this](Db& db) {
            auto t = db.tags(engine.collection());
            auto c = db.count(engine.collection());
            call([this, t = std::move(t), c]() mutable {
                tags = std::move(t);
                db_count = c;
                if (tags_refreshed) tags_refreshed();
                if (redraw_all) redraw_all();
            });
        });
    }

    /// Asks the launcher whether it holds the global shortcut. Returns true when the answer changed.
    bool refresh_hotkey_state() {
        const HotkeyState before = hotkey_state;
        const auto reply = query_cmd(find_launcher(), Cmd::HotkeyStatus);
        hotkey_state = reply && *reply <= static_cast<DWORD_PTR>(HotkeyState::Unavailable) ? static_cast<HotkeyState>(*reply) : HotkeyState::Unknown;
        return hotkey_state != before;
    }

    /// True while the configured shortcut cannot open search because another application owns it.
    [[nodiscard]] bool hotkey_unavailable() const noexcept {
        return settings.hotkey_enabled && hotkey_state == HotkeyState::Unavailable;
    }

    bool poll_indexer(bool force = false) {
        // Model files dropped into the folder (or exported by another process) are picked up without a restart
        engine.encoder().retry_if_files_changed();
        const bool was_running = indexer_running;
        const bool prev_stopping = indexer_stopping;
        const uint64_t prev_embedded = indexer.embedded;
        const uint64_t prev_found = indexer.found;
        const uint64_t prev_failed = indexer.failed;
        const Phase prev_phase = indexer.phase;

        if (auto b = StatusBlock::open()) {
            indexer = b->read();
            indexer_running = indexer.pid != 0 && indexer.phase != Phase::Done && indexer.phase != Phase::Stopped && indexer.phase != Phase::Error;
        } else {
            indexer_running = false;
        }

        if (indexer_running) {
            if (!stop_event_) {
                stop_event_.reset(OpenEventW(SYNCHRONIZE, FALSE, INDEXER_STOP_EVENT));
            }
            if (stop_event_ && WaitForSingleObject(stop_event_.get(), 0) == WAIT_OBJECT_0) {
                indexer_stopping = true;
            }
        } else {
            indexer_stopping = false;
        }

        const bool finished = was_running && !indexer_running;
        if (finished) engine.encoder().retry(); // the indexer exports the text model when it was missing
        const bool failed_changed = indexer_running && indexer.failed != prev_failed;
        if (force || finished || failed_changed || !polled_once_) {
            polled_once_ = true;
            index_state = load_json_file<IndexState>(state_path());
            failed_files = load_failed_files(settings.model_name);
        }
        const bool notifs_changed = reload_notifications(force);

        return force || finished || notifs_changed || indexer_running != was_running || indexer_stopping != prev_stopping ||
               (indexer_running && (indexer.embedded != prev_embedded || indexer.found != prev_found || indexer.failed != prev_failed || indexer.phase != prev_phase));
    }

    bool reload_notifications(bool force = false) {
        if (test_mode) return false;
        const auto stamp = notifications_file_stamp();
        if (!force && stamp == notifications_stamp_) return false;
        notifications_stamp_ = stamp;
        notifications = load_notifications();
        return true;
    }

    void notify(std::string severity, std::string source, std::string title, std::string detail, std::string key = {}) {
        auto apply = [this, sev = std::move(severity), src = std::move(source), t = std::move(title), d = std::move(detail), k = std::move(key)]() mutable {
            if (test_mode) {
                NotificationStoreData store{test_next_notif_id_++, std::move(notifications)};
                apply_notification(store, sev, src, t, d, k, unix_now());
                notifications = std::move(store.items);
            } else {
                notifications = push_notification_to_disk(sev, src, t, d, k, false);
                notifications_stamp_ = notifications_file_stamp();
            }
            if (redraw_all) redraw_all();
        };
        if (call_hwnd && GetWindowThreadProcessId(call_hwnd, nullptr) != GetCurrentThreadId()) {
            call(std::move(apply));
        } else {
            apply();
        }
    }

    void notify(std::string severity, std::string source, std::wstring_view title, std::wstring_view detail, std::string key = {}) {
        notify(std::move(severity), std::move(source), to_utf8(title), to_utf8(detail), std::move(key));
    }

    void dismiss_notification(uint64_t id) {
        if (test_mode) {
            NotificationStoreData store{test_next_notif_id_, std::move(notifications)};
            ::usyseek::dismiss_notification(store, id);
            notifications = std::move(store.items);
        } else {
            notifications = dismiss_notification_on_disk(id, false);
            notifications_stamp_ = notifications_file_stamp();
        }
        if (redraw_all) redraw_all();
    }

    void clear_notifications() {
        if (test_mode) {
            notifications.clear();
        } else {
            notifications = clear_notifications_on_disk(false);
            notifications_stamp_ = notifications_file_stamp();
        }
        if (redraw_all) redraw_all();
    }

    void clear_notification_key(std::string_view source, std::string_view key) {
        if (test_mode) {
            NotificationStoreData store{test_next_notif_id_, std::move(notifications)};
            ::usyseek::clear_notification_key(store, source, key);
            notifications = std::move(store.items);
        } else {
            notifications = clear_notification_key_on_disk(source, key, false);
            notifications_stamp_ = notifications_file_stamp();
        }
        if (redraw_all) redraw_all();
    }

    [[nodiscard]] size_t error_notification_count() const noexcept {
        return static_cast<size_t>(std::ranges::count_if(notifications, [](const Notification& n) {
            return n.severity == "error";
        }));
    }

    bool polled_once_ = false;
    uint64_t test_next_notif_id_ = 1;

    void refresh_theme(bool reload_catalog = false, bool report_errors = false) {
        if (reload_catalog || themes.entries().empty()) {
            std::vector<std::string> errors;
            themes.reload(&errors);
            if (report_errors && !test_mode && !errors.empty()) {
                std::string detail;
                for (size_t i = 0; i < errors.size(); ++i) {
                    if (i > 0) detail += '\n';
                    detail += errors[i];
                }
                notify("warning", "Themes", "Failed to load one or more custom themes", detail, "theme-load");
            } else if (reload_catalog && errors.empty()) {
                clear_notification_key("Themes", "theme-load");
            }
        } else {
            themes.refresh_dynamic_swatches();
        }
        const std::string& wanted = theme_override ? *theme_override : settings.theme;
        theme = themes.resolve(wanted);
        if (theme_changed) {
            theme_changed();
        } else if (redraw_all) {
            redraw_all();
        }
    }

    void on_windows_theme_changed() {
        if (!theme.follows_system_mode && !theme.follows_windows_accent) return;
        refresh_theme(false, false);
    }

    void save() {
        if (!test_mode && !save_settings(settings)) {
            notify("error", "Settings", "Failed to save settings", "Could not write settings.json (" + win32_error_utf8(GetLastError()) + ").", "save-settings");
        }
        engine.set_settings(settings);
    }
};

/// Native Windows confirmation dialog (Yes / No, defaulting to No).
inline bool confirm_box(HWND owner, const wchar_t* title, const std::wstring& message) {
    return MessageBoxW(owner, message.c_str(), title, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
}

/// Applies theme-driven immersive dark/light mode, corner preference, backdrop, and border to a window.
inline void apply_window_theme(HWND hwnd, const Theme& theme, bool is_pill, bool round = true) {
    if (!hwnd) return;
    const DWORD want_affinity = theme.is_liquid_glass ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    DWORD cur_affinity = WDA_NONE;
    if (!GetWindowDisplayAffinity(hwnd, &cur_affinity) || cur_affinity != want_affinity) {
        SetWindowDisplayAffinity(hwnd, want_affinity);
    }
    ulp::win::dark::allow_dark_mode_for_window(hwnd, theme.dark_mode);
    const BOOL dark = theme.dark_mode ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corner = (round && theme.round_corners) ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const int backdrop = is_pill ? theme.pill_backdrop : theme.settings_backdrop;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    if (is_pill) {
        const COLORREF border = theme.is_liquid_glass ? DWMWA_COLOR_NONE : to_colorref(theme.pill_border);
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    }
    const MARGINS m{.cxLeftWidth = -1, .cxRightWidth = -1, .cyTopHeight = -1, .cyBottomHeight = -1};
    DwmExtendFrameIntoClientArea(hwnd, &m);
}

/// Applies the dark immersive frame + backdrop attributes used by both windows.
inline void style_window(HWND hwnd, int backdrop, bool round = true) {
    Theme t = theme_dark();
    t.pill_backdrop = backdrop;
    t.settings_backdrop = backdrop;
    apply_window_theme(hwnd, t, false, round);
}

inline bool open_file(const std::wstring& path, std::string* error = nullptr) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) return true;
    if (error) *error = win32_error_utf8(GetLastError());
    return false;
}

inline bool reveal_in_explorer(const std::wstring& path, std::string* error = nullptr) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (!pidl) {
        if (error) *error = "Path could not be resolved on disk (it may have been moved or deleted).";
        return false;
    }
    const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
    if (SUCCEEDED(hr)) return true;
    if (error) *error = win32_error_utf8(static_cast<DWORD>(hr));
    return false;
}

/// Hands `n` bytes to `obj` as an HGLOBAL of format `cf` (the data object takes ownership).
inline bool set_data_bytes(IDataObject* obj, CLIPFORMAT cf, const void* data, size_t n) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!h) return false;
    if (void* p = GlobalLock(h)) {
        std::memcpy(p, data, n);
        GlobalUnlock(h);
    }
    FORMATETC fe{cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm{};
    sm.tymed = TYMED_HGLOBAL;
    sm.hGlobal = h;
    if (SUCCEEDED(obj->SetData(&fe, &sm, TRUE))) return true;
    GlobalFree(h);
    return false;
}

/// A shell data object for files, as Explorer makes it: shell ID lists + CF_HDROP, so drops and pastes into
/// Explorer, browsers, chat apps and editors receive the files themselves. Pastes default to copying.
inline wil::com_ptr<IDataObject> media_data_object(std::span<const std::wstring> paths) {
    std::vector<PIDLIST_ABSOLUTE> pidls;
    std::vector<std::wstring> found;
    for (const auto& p : paths) {
        if (PIDLIST_ABSOLUTE id = ILCreateFromPathW(p.c_str())) {
            pidls.push_back(id);
            found.push_back(p);
        }
    }
    const auto free_pidls = wil::scope_exit([&] {
        for (auto* id : pidls) ILFree(id);
    });
    if (pidls.empty()) return nullptr;

    // Shell ID lists relative to the shared folder (Explorer's own shape), or absolute under the desktop
    PIDLIST_ABSOLUTE parent = ILCloneFull(pidls.front());
    if (!parent) return nullptr;
    const auto free_parent = wil::scope_exit([&] { ILFree(parent); });
    ILRemoveLastID(reinterpret_cast<PUIDLIST_RELATIVE>(parent));
    bool same_parent = true;
    for (auto* id : pidls) {
        PIDLIST_ABSOLUTE pp = ILCloneFull(id);
        if (!pp) return nullptr;
        ILRemoveLastID(reinterpret_cast<PUIDLIST_RELATIVE>(pp));
        const bool eq = ILIsEqual(pp, parent) != FALSE;
        ILFree(pp);
        if (!eq) {
            same_parent = false;
            break;
        }
    }
    std::vector<PCUITEMID_CHILD> children;
    children.reserve(pidls.size());
    for (auto* id : pidls) children.push_back(same_parent ? ILFindLastID(id) : reinterpret_cast<PCUITEMID_CHILD>(id));
    wil::com_ptr<IDataObject> obj;
    if (FAILED(SHCreateDataObject(same_parent ? parent : nullptr, static_cast<UINT>(children.size()), children.data(), nullptr, IID_PPV_ARGS(&obj)))) return nullptr;

    // Plain file list too: what most non-shell drop targets read, and what survives a clipboard flush
    std::vector<uint8_t> buf(sizeof(DROPFILES));
    for (const auto& p : found) {
        const auto* b = reinterpret_cast<const uint8_t*>(p.c_str());
        buf.insert(buf.end(), b, b + (p.size() + 1) * sizeof(wchar_t));
    }
    buf.insert(buf.end(), sizeof(wchar_t), 0);
    auto* df = reinterpret_cast<DROPFILES*>(buf.data());
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    set_data_bytes(obj.get(), CF_HDROP, buf.data(), buf.size());

    const DWORD effect = DROPEFFECT_COPY;
    set_data_bytes(obj.get(), static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)), &effect, sizeof(effect));
    return obj;
}

/// Wraps a shell data object to properly advertise and handle IDataObjectAsyncCapability (IAsyncOperation).
/// SHCreateDataObject's built-in implementation of IDataObjectAsyncCapability ignores SetAsyncMode and
/// reports FALSE from GetAsyncMode for local file system items, which causes Windows Explorer to run the
/// drop and its conflict resolution dialogs synchronously inside IDropTarget::Drop() on our UI thread.
class AsyncDataObject final : public IDataObject, public IDataObjectAsyncCapability {
public:
    explicit AsyncDataObject(wil::com_ptr<IDataObject> inner) : inner_(std::move(inner)) {}
    AsyncDataObject(const AsyncDataObject&) = delete;
    AsyncDataObject& operator=(const AsyncDataObject&) = delete;
    AsyncDataObject(AsyncDataObject&&) = delete;
    AsyncDataObject& operator=(AsyncDataObject&&) = delete;
    virtual ~AsyncDataObject() = default;

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDataObject)) {
            *ppv = static_cast<IDataObject*>(this);
        } else if (riid == __uuidof(IDataObjectAsyncCapability)) {
            *ppv = static_cast<IDataObjectAsyncCapability*>(this);
        }
        if (*ppv) {
            AddRef();
            return S_OK;
        }
        return inner_ ? inner_->QueryInterface(riid, ppv) : E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override {
        return ++ref_count_;
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG count = --ref_count_;
        if (count == 0) delete this;
        return count;
    }

    // IDataObjectAsyncCapability
    IFACEMETHODIMP SetAsyncMode(BOOL fDoOpAsync) override {
        async_mode_ = fDoOpAsync;
        return S_OK;
    }

    IFACEMETHODIMP GetAsyncMode(BOOL* pfIsOpAsync) override {
        if (!pfIsOpAsync) return E_POINTER;
        *pfIsOpAsync = async_mode_;
        return S_OK;
    }

    IFACEMETHODIMP StartOperation(IBindCtx* /*pbcReserved*/) override {
        in_operation_ = TRUE;
        return S_OK;
    }

    IFACEMETHODIMP InOperation(BOOL* pfInAsyncOp) override {
        if (!pfInAsyncOp) return E_POINTER;
        *pfInAsyncOp = in_operation_;
        return S_OK;
    }

    IFACEMETHODIMP EndOperation(HRESULT /*hResult*/, IBindCtx* /*pbcReserved*/, DWORD /*dwEffects*/) override {
        in_operation_ = FALSE;
        return S_OK;
    }

    // IDataObject
    IFACEMETHODIMP GetData(FORMATETC* pformatetcIn, STGMEDIUM* pmedium) override {
        return inner_ ? inner_->GetData(pformatetcIn, pmedium) : E_UNEXPECTED;
    }

    IFACEMETHODIMP GetDataHere(FORMATETC* pformatetc, STGMEDIUM* pmedium) override {
        return inner_ ? inner_->GetDataHere(pformatetc, pmedium) : E_UNEXPECTED;
    }

    IFACEMETHODIMP QueryGetData(FORMATETC* pformatetc) override {
        return inner_ ? inner_->QueryGetData(pformatetc) : E_UNEXPECTED;
    }

    IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC* pformatetcIn, FORMATETC* pformatetcOut) override {
        return inner_ ? inner_->GetCanonicalFormatEtc(pformatetcIn, pformatetcOut) : E_UNEXPECTED;
    }

    IFACEMETHODIMP SetData(FORMATETC* pformatetc, STGMEDIUM* pmedium, BOOL fRelease) override {
        return inner_ ? inner_->SetData(pformatetc, pmedium, fRelease) : E_UNEXPECTED;
    }

    IFACEMETHODIMP EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppenumFormatEtc) override {
        return inner_ ? inner_->EnumFormatEtc(dwDirection, ppenumFormatEtc) : E_UNEXPECTED;
    }

    IFACEMETHODIMP DAdvise(FORMATETC* pformatetc, DWORD advf, IAdviseSink* pAdvSink, DWORD* pdwConnection) override {
        return inner_ ? inner_->DAdvise(pformatetc, advf, pAdvSink, pdwConnection) : E_UNEXPECTED;
    }

    IFACEMETHODIMP DUnadvise(DWORD dwConnection) override {
        return inner_ ? inner_->DUnadvise(dwConnection) : E_UNEXPECTED;
    }

    IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA** ppenumAdvise) override {
        return inner_ ? inner_->EnumDAdvise(ppenumAdvise) : E_UNEXPECTED;
    }

private:
    wil::com_ptr<IDataObject> inner_;
    std::atomic<ULONG> ref_count_{1};
    std::atomic<BOOL> async_mode_{VARIANT_TRUE};
    std::atomic<BOOL> in_operation_{FALSE};
};

/// Drags files out to other applications (copy or link only: a drop never moves the user's files away).
/// The target is asked to finish the drop on its own thread, so its dialogs (such as "file already exists")
/// never block this one inside the drag loop. Returns the data object's async state when the target is still
/// working on the drop afterwards, null otherwise.
inline wil::com_ptr<IDataObjectAsyncCapability> drag_media(HWND hwnd, std::span<const std::wstring> paths, std::string* error = nullptr) {
    auto inner = media_data_object(paths);
    if (!inner) {
        if (error) *error = "None of the selected files could be found on disk for drag-and-drop.";
        return nullptr;
    }
    auto* wrapper = new (std::nothrow) AsyncDataObject(std::move(inner));
    if (!wrapper) {
        if (error) *error = "Out of memory starting drag operation.";
        return nullptr;
    }
    wil::com_ptr<IDataObject> obj;
    obj.attach(static_cast<IDataObject*>(wrapper));
    auto async = obj.try_query<IDataObjectAsyncCapability>();
    if (!async) {
        if (error) *error = "Could not initialize drag data object.";
        return nullptr;
    }
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT hr = SHDoDragDrop(hwnd, obj.get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
    if (FAILED(hr) && error) {
        *error = "Drag-and-drop failed (" + win32_error_utf8(static_cast<DWORD>(hr)) + ").";
    }
    BOOL in_operation = FALSE;
    if (SUCCEEDED(async->InOperation(&in_operation)) && in_operation) return async;
    return nullptr;
}

/// What a copy puts on the clipboard besides the files themselves, prepared away from the UI thread.
struct ClipboardPayload {
    std::vector<std::wstring> paths;
    std::vector<uint8_t> dib; // bitmap for apps that paste pixels (Paint, Office, image editors)
    std::vector<uint8_t> png; // the file's own bytes as "PNG" so alpha survives
};

/// Any thread: copies `image` (unpremultiplied BGRA) into a DIB and reads `png_file`.
inline ClipboardPayload build_clipboard_payload(std::vector<std::wstring> paths, const Raster* image, const std::wstring* png_file) {
    ClipboardPayload payload{std::move(paths)};
    if (image && image->w && image->h) {
        const size_t stride = static_cast<size_t>(image->w) * 4;
        payload.dib.resize(sizeof(BITMAPINFOHEADER) + stride * image->h);
        auto* bih = reinterpret_cast<BITMAPINFOHEADER*>(payload.dib.data());
        bih->biSize = sizeof(BITMAPINFOHEADER);
        bih->biWidth = static_cast<LONG>(image->w);
        bih->biHeight = static_cast<LONG>(image->h); // bottom-up: the form every consumer reads correctly
        bih->biPlanes = 1;
        bih->biBitCount = 32;
        bih->biCompression = BI_RGB;
        bih->biSizeImage = static_cast<DWORD>(stride * image->h);
        for (UINT y = 0; y < image->h; ++y) {
            std::memcpy(payload.dib.data() + sizeof(BITMAPINFOHEADER) + stride * (image->h - 1 - y), image->pixels.data() + stride * y, stride);
        }
    }
    if (png_file) {
        wil::unique_hfile f{CreateFileW(png_file->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
        LARGE_INTEGER size{};
        if (f && GetFileSizeEx(f.get(), &size) && size.QuadPart > 0 && size.QuadPart < (256ll << 20)) {
            payload.png.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            if (!ReadFile(f.get(), payload.png.data(), static_cast<DWORD>(payload.png.size()), &read, nullptr) || read != payload.png.size()) payload.png.clear();
        }
    }
    return payload;
}

/// UI thread: the data object to hand to OleSetClipboard.
inline wil::com_ptr<IDataObject> clipboard_data_object(const ClipboardPayload& payload) {
    auto obj = media_data_object(payload.paths);
    if (!obj) return nullptr;
    if (!payload.dib.empty()) set_data_bytes(obj.get(), CF_DIB, payload.dib.data(), payload.dib.size());
    if (!payload.png.empty()) set_data_bytes(obj.get(), static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"PNG")), payload.png.data(), payload.png.size());
    return obj;
}

inline std::wstring format_date(int64_t unix) {
    if (unix <= 0) return {};
    const uint64_t ft = (static_cast<uint64_t>(unix) + 11'644'473'600ull) * 10'000'000ull;
    FILETIME f{static_cast<DWORD>(ft), static_cast<DWORD>(ft >> 32)};
    FILETIME local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&f, &local);
    FileTimeToSystemTime(&local, &st);
    wchar_t buf[64];
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"d MMM yyyy", buf, 64, nullptr);
    return buf;
}

/// Tag chip; returns its width.
inline float draw_tag_chip(Canvas& cv, float x, float y, std::wstring_view label, D2D1_COLOR_F col, float max_w = 1e9f) {
    const float tw = std::min(cv.measure(label, Font::Caption), max_w - 24.0f);
    const float w = tw + 23.0f;
    const D2D1_RECT_F r = rect(x, y, w, 20.0f);
    cv.fill_round(r, 10.0f, with_alpha(col, 0.13f));
    cv.stroke_round(r, 10.0f, with_alpha(col, 0.38f));
    cv.fill_circle({x + 10.0f, y + 10.0f}, 2.5f, col);
    cv.text(label, rect(x + 16.0f, y, tw + 2.0f, 20.0f), Font::Caption, col);
    return w;
}

} // namespace usyseek::ui
