#pragma once

#include "canvas.hpp"
#include "search.hpp"
#include "thumbs.hpp"

#include <usyseek/status.hpp>

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

namespace usyseek::ui {

inline constexpr UINT WM_APP_THUMBS = WM_APP + 10;
inline constexpr UINT WM_APP_RESULT = WM_APP + 11;
inline constexpr UINT WM_APP_CALL = WM_APP + 12;

/// State and services shared by the pill and the settings window (UI thread only unless noted).
struct Shared {
    Gfx gfx;
    Fonts fonts;
    Settings settings;
    Engine engine;
    Thumbs thumbs;

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

    bool poll_indexer(bool force = false) {
        const bool was_running = indexer_running;
        const bool prev_stopping = indexer_stopping;
        const uint64_t prev_embedded = indexer.embedded;
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
        const bool failed_changed = indexer_running && indexer.failed != prev_failed;
        if (force || finished || failed_changed || !polled_once_) {
            polled_once_ = true;
            index_state = load_json_file<IndexState>(state_path());
            failed_files = load_failed_files(settings.model_name);
        }

        return force || finished || indexer_running != was_running || indexer_stopping != prev_stopping ||
               (indexer_running && (indexer.embedded != prev_embedded || indexer.failed != prev_failed || indexer.phase != prev_phase));
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
