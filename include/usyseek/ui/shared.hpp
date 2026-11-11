#pragma once

#include "canvas.hpp"
#include "search.hpp"
#include "thumbs.hpp"

#include <usyseek/progress.hpp>
#include <usyseek/status.hpp>

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

namespace usyseek::ui {

inline constexpr UINT WM_APP_THUMBS = WM_APP + 10;
inline constexpr UINT WM_APP_RESULT = WM_APP + 11;
inline constexpr UINT WM_APP_CALL = WM_APP + 12;
inline constexpr UINT WM_APP_REDRAW = WM_APP + 13; // coalesced "something changed" from a worker thread

inline std::wstring format_hotkey(uint32_t mods, uint32_t vk) {
    std::wstring t;
    if (mods & MOD_CONTROL) t += L"Ctrl + ";
    if (mods & MOD_ALT) t += L"Alt + ";
    if (mods & MOD_SHIFT) t += L"Shift + ";
    if (mods & MOD_WIN) t += L"Win + ";
    wchar_t name[64]{};
    if (vk == VK_SPACE) wcscpy_s(name, L"Space");
    else GetKeyNameTextW(static_cast<LONG>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16), name, 64);
    return t + (name[0] ? name : L"Key");
}

/// State and services shared by the pill and the settings window (UI thread only unless noted).
struct Shared {
    Gfx gfx;
    Fonts fonts;
    Settings settings;
    Engine engine;
    Thumbs thumbs;
    Thumbs previews{4, true}; // full-size viewer images: the shown one and its neighbours

    std::vector<TagCount> tags;      // facet counts
    std::optional<uint64_t> db_count;
    IndexState index_state;
    std::vector<FailedFile> failed_files;
    bool indexer_running = false;
    bool indexer_stopping = false;
    StatusData indexer{};
    wil::unique_event_nothrow stop_event_;

    HWND call_hwnd = nullptr;        // window that runs marshalled callbacks
    bool test_mode = false;
    HotkeyState hotkey_state = HotkeyState::Unknown;
    int background_jobs = 0;         // downloads / installs in flight: the process must not idle-exit under them

    std::function<void()> open_settings;
    std::function<void()> visibility_changed;
    std::function<void()> settings_changed; // repaint dependants
    std::function<void()> tags_refreshed;
    std::function<void()> redraw_all;

    /// Runs `fn` on the UI thread (safe from any thread).
    void call(std::function<void()> fn) {
        if (!call_hwnd) return;
        PostMessageW(call_hwnd, WM_APP_CALL, 0, reinterpret_cast<LPARAM>(new std::function<void()>(std::move(fn))));
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

        return force || finished || indexer_running != was_running || indexer_stopping != prev_stopping ||
               (indexer_running && (indexer.embedded != prev_embedded || indexer.found != prev_found || indexer.failed != prev_failed || indexer.phase != prev_phase));
    }

    bool polled_once_ = false;

    void save() {
        if (!test_mode) save_settings(settings);
        engine.set_settings(settings);
    }
};

/// Applies the dark immersive frame + backdrop attributes used by both windows.
inline void style_window(HWND hwnd, int backdrop, bool round = true) {
    ulp::win::dark::allow_dark_mode_for_window(hwnd, true);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corner = round ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    const MARGINS m{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &m);
}

inline void open_file(const std::wstring& path) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

inline void reveal_in_explorer(const std::wstring& path) {
    if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str())) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
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

/// Drags files out to other applications (copy or link only: a drop never moves the user's files away).
inline DWORD drag_media(HWND hwnd, std::span<const std::wstring> paths) {
    const auto obj = media_data_object(paths);
    DWORD effect = DROPEFFECT_NONE;
    if (obj) SHDoDragDrop(hwnd, obj.get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
    return effect;
}

/// Puts files on the clipboard. `image` (unpremultiplied BGRA) adds a bitmap for apps that paste pixels
/// (Paint, Office, image editors); `png_file` adds that file's bytes as "PNG" so alpha survives.
inline bool copy_media_to_clipboard(std::span<const std::wstring> paths, const Raster* image, const std::wstring* png_file) {
    const auto obj = media_data_object(paths);
    if (!obj) return false;
    if (image && image->w && image->h) {
        const size_t stride = static_cast<size_t>(image->w) * 4;
        std::vector<uint8_t> dib(sizeof(BITMAPINFOHEADER) + stride * image->h);
        auto* bih = reinterpret_cast<BITMAPINFOHEADER*>(dib.data());
        bih->biSize = sizeof(BITMAPINFOHEADER);
        bih->biWidth = static_cast<LONG>(image->w);
        bih->biHeight = static_cast<LONG>(image->h); // bottom-up: the form every consumer reads correctly
        bih->biPlanes = 1;
        bih->biBitCount = 32;
        bih->biCompression = BI_RGB;
        bih->biSizeImage = static_cast<DWORD>(stride * image->h);
        for (UINT y = 0; y < image->h; ++y) {
            std::memcpy(dib.data() + sizeof(BITMAPINFOHEADER) + stride * (image->h - 1 - y), image->pixels.data() + stride * y, stride);
        }
        set_data_bytes(obj.get(), CF_DIB, dib.data(), dib.size());
    }
    if (png_file) {
        std::vector<uint8_t> bytes;
        wil::unique_hfile f{CreateFileW(png_file->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
        LARGE_INTEGER size{};
        if (f && GetFileSizeEx(f.get(), &size) && size.QuadPart > 0 && size.QuadPart < (256ll << 20)) {
            bytes.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            if (ReadFile(f.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size()) {
                set_data_bytes(obj.get(), static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"PNG")), bytes.data(), bytes.size());
            }
        }
    }
    if (FAILED(OleSetClipboard(obj.get()))) return false;
    // Render now: this process exits when idle and the clipboard must outlive it
    OleFlushClipboard();
    return true;
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
