// usyseek-ui.exe - the search pill and settings window process.
// Runs ONNX Runtime on CPU for the text tower, reads the embedded Qdrant Edge database, and exits after
// `ui_idle_timeout_s` of inactivity when hidden so idle memory drops back to zero.

#include <usyseek/ui/pill.hpp>
#include <usyseek/ui/settings_window.hpp>
#include <usyseek/ui/theme_editor_window.hpp>

#include <shellapi.h>

namespace {

using namespace usyseek;
using namespace usyseek::ui;

constexpr UINT_PTR TIMER_IDLE_EXIT = 900;

void pump_messages(DWORD ms) {
    const ULONGLONG deadline = GetTickCount64() + ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(deadline - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

void wait_search(const Pill& pill, DWORD timeout_ms = 15000) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        pump_messages(25);
        if (!pill.searching()) break;
    }
    pump_messages(180);
}

bool capture_window(WindowTarget& wt, const std::function<void()>& render_fn, const fs::path& png) {
    wt.request_capture(png);
    render_fn();
    return wt.capture_ok();
}

int run_automated_test(Shared& sh, Pill& pill, SettingsWindow& settings_win, const fs::path& out_dir) {
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    sh.test_mode = true;

    // Wait briefly for the text encoder and Qdrant connection to settle
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < 15000) {
        pump_messages(40);
        const auto st = sh.engine.encoder().state();
        if (sh.engine.db_ready() && (st == TextEncoder::State::Ready || st == TextEncoder::State::Failed)) break;
    }
    sh.refresh_tags();
    sh.poll_indexer();
    pump_messages(200);

    pill.show();
    pump_messages(100);
    capture_window(pill.target(), [&] { pill.render(); }, out_dir / L"01_pill_collapsed.png");

    // Semantic search
    pill.test_set_query(L"red circle");
    wait_search(pill);
    capture_window(pill.target(), [&] { pill.render(); }, out_dir / L"02_pill_semantic.png");

    // Context menu + tag assignment on first card
    if (pill.result_count() > 0) {
        pill.test_open_menu(0);
        pump_messages(80);
        capture_window(pill.target(), [&] { pill.render(); }, out_dir / L"03_pill_context_menu.png");
        SendMessageW(pill.hwnd(), WM_CHAR, L'f', 0);
        SendMessageW(pill.hwnd(), WM_CHAR, L'a', 0);
        SendMessageW(pill.hwnd(), WM_CHAR, L'v', 0);
        SendMessageW(pill.hwnd(), WM_KEYDOWN, VK_RETURN, 0);
        pump_messages(250);
        SendMessageW(pill.hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
        pump_messages(60);

        // Full-size viewer, then the next result
        pill.test_preview(0);
        for (int i = 0; i < 100 && pill.preview_loading(); ++i) pump_messages(50);
        pump_messages(80);
        capture_window(pill.target(), [&] { pill.render(); }, out_dir / L"03b_pill_preview.png");
        if (pill.result_count() > 1) {
            SendMessageW(pill.hwnd(), WM_KEYDOWN, VK_RIGHT, 0);
            for (int i = 0; i < 100 && pill.preview_loading(); ++i) pump_messages(50);
            pump_messages(80);
            capture_window(pill.target(), [&] { pill.render(); }, out_dir / L"03c_pill_preview_next.png");
        }
        SendMessageW(pill.hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
        pump_messages(60);
    }

    // Tag autocomplete dropdown
    pill.test_dropdown(L"tag:");
    pump_messages(80);
    capture_window(pill.target(), [&] { pill.render(); }, out_dir / L"04_pill_tag_dropdown.png");

    // Filter by tag:fav
    SendMessageW(pill.hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
    pill.test_set_query(L"tag:fav");
    wait_search(pill);
    capture_window(pill.target(), [&] { pill.render(); }, out_dir / L"05_pill_tag_search.png");

    // Clean up test tag from first result
    if (pill.result_count() > 0) {
        pill.test_open_menu(0);
        pump_messages(40);
        SendMessageW(pill.hwnd(), WM_CHAR, L'f', 0);
        SendMessageW(pill.hwnd(), WM_CHAR, L'a', 0);
        SendMessageW(pill.hwnd(), WM_CHAR, L'v', 0);
        SendMessageW(pill.hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
    }

    // Settings window: Tab 1 (Progress & Scope) and Tab 2 (Application & Venv)
    settings_win.show(0);
    pump_messages(120);
    capture_window(settings_win.target(), [&] { settings_win.render(); }, out_dir / L"06_settings_progress.png");

    settings_win.set_tab(1);
    pump_messages(120);
    capture_window(settings_win.target(), [&] { settings_win.render(); }, out_dir / L"07_settings_venv.png");

    settings_win.hide();
    pill.hide();
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const ulp::win::COMWrapper<false> com{static_cast<DWORD>(COINIT_APARTMENTTHREADED) | static_cast<DWORD>(COINIT_DISABLE_OLE1DDE)};
    // OLE proper (not just COM) for dragging media out and putting it on the clipboard.
    // Keep alive across Pill/SettingsWindow/Shared destruction.
    struct OleScope {
        HRESULT hr = OleInitialize(nullptr);
        OleScope() = default;
        OleScope(const OleScope&) = delete;
        OleScope& operator=(const OleScope&) = delete;
        OleScope(OleScope&&) = delete;
        OleScope& operator=(OleScope&&) = delete;
        ~OleScope() {
            if (SUCCEEDED(hr)) OleUninitialize();
        }
    };
    const OleScope ole;
    ulp::win::dark::init_dark_mode();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool show_pill = true;
    bool show_settings = false;
    int settings_tab = 0;
    bool show_notifications = false;
    std::optional<fs::path> test_dir;
    std::optional<std::string> theme_arg;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a{argv[i]};
        if (a == L"--settings") {
            show_settings = true;
            settings_tab = 0;
            show_pill = false;
        } else if (a == L"--settings-tab" && i + 1 < argc) {
            show_settings = true;
            settings_tab = std::clamp(_wtoi(argv[++i]), 0, 3);
            show_pill = false;
        } else if (a == L"--notifications") {
            show_notifications = true;
            show_pill = false;
        } else if (a == L"--show") {
            show_pill = true;
        } else if (a == L"--theme" && i + 1 < argc) {
            theme_arg = to_utf8(argv[++i]);
        } else if (a == L"--test" && i + 1 < argc) {
            test_dir = fs::path{argv[++i]};
        }
    }
    if (argv) LocalFree(static_cast<HLOCAL>(argv));

    // If an instance is already running, forward the request and exit
    if (!test_dir) {
        if (HWND existing = FindWindowW(PILL_CLASS, nullptr)) {
            AllowSetForegroundWindow( ASFW_ANY );
            const UINT msg = show_notifications ? msg_open_notifications() : (show_settings ? msg_open_settings() : msg_toggle_search());
            const WPARAM wp = show_settings ? static_cast<WPARAM>(settings_tab + 1) : 0;
            PostMessageW(existing, msg, wp, 0);
            return 0;
        }
    }

    Shared sh;
    sh.test_mode = test_dir.has_value();
    sh.theme_override = std::move(theme_arg);
    bool settings_corrupt = false;
    sh.settings = load_settings(&settings_corrupt);
    sh.refresh_theme(true, false);
    if (!sh.test_mode) {
        sh.reload_notifications(true);
        sh.update_state = load_update_state();
    }
    if (!sh.gfx.init()) {
        MessageBoxW(nullptr, L"Failed to initialize Direct3D / Direct2D graphics.", APP_NAME, MB_ICONERROR | MB_OK);
        return 1;
    }
    if (!sh.fonts.init(sh.gfx.dwrite())) {
        MessageBoxW(nullptr, L"Failed to initialize DirectWrite fonts.", APP_NAME, MB_ICONERROR | MB_OK);
        return 1;
    }

    Pill pill{sh};
    SettingsWindow settings_win{sh};
    ThemeEditorWindow theme_editor_win{sh};
    if (!pill.create(hInstance)) {
        MessageBoxW(nullptr, L"Failed to create the search window.", APP_NAME, MB_ICONERROR | MB_OK);
        return 1;
    }
    if (!settings_win.create(hInstance)) {
        MessageBoxW(nullptr, L"Failed to create the settings window.", APP_NAME, MB_ICONERROR | MB_OK);
        return 1;
    }
    if (!theme_editor_win.create(hInstance)) {
        MessageBoxW(nullptr, L"Failed to create the theme editor window.", APP_NAME, MB_ICONERROR | MB_OK);
        return 1;
    }
    settings_win.open_theme_editor = [&](const ThemeEntry* entry, bool edit_existing) {
        if (!entry) {
            theme_editor_win.open_new();
        } else if (edit_existing) {
            theme_editor_win.open_edit(*entry);
        } else {
            theme_editor_win.open_duplicate(*entry);
        }
    };

    sh.call_hwnd.store(pill.hwnd(), std::memory_order_release);
    sh.refresh_theme(true, true);
    if (settings_corrupt && !sh.test_mode) {
        sh.notify("warning", "Settings", "Corrupted settings.json reset to defaults",
                  "settings.json could not be parsed cleanly. Default settings are in use until saved.", "settings-corrupt");
    }
    if (sh.gfx.is_warp()) {
        sh.notify("warning", "Graphics", "Hardware GPU rendering unavailable",
                  "Direct3D fell back to WARP software rendering; animations and video playback may be slower.", "warp-fallback");
    }
    sh.gfx.on_device_changed = [&] {
        sh.desktop_dupl.reset();
        sh.thumbs.reset_gpu();
        sh.previews.reset_gpu();
        pill.reset_preview_gpu();
    };
    sh.thumbs.start(pill.hwnd(), WM_APP_THUMBS);
    sh.previews.start(pill.hwnd(), WM_APP_THUMBS, 2);

    sh.engine.on_result = [&](std::unique_ptr<SearchResult> r) {
        const HWND hwnd = sh.call_hwnd.load(std::memory_order_acquire);
        if (!hwnd || !r) return;
        const auto* const raw = r.release();
        if (!PostMessageW(hwnd, WM_APP_RESULT, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
    };
    sh.engine.on_db_state = [&](bool ready, std::string err) {
        sh.call([&, ready, err = std::move(err)] {
            if (ready) {
                sh.clear_notification_key("Database", "db-connect");
                sh.clear_notification_key("Database", "db-job-dropped");
                sh.refresh_tags(true);
            } else if (!err.empty()) {
                sh.notify("error", "Database", "Could not open Qdrant Edge database", err, "db-connect");
            }
            if (sh.redraw_all) sh.redraw_all();
        });
    };
    sh.engine.on_db_cleared = [&](std::string coll, std::string err) {
        sh.call([&, coll = std::move(coll), err = std::move(err)] {
            sh.notify("warning", "Database", "Database cleared — re-indexing required",
                      ulp::str::concat_strings("Could not load the Qdrant Edge index for '", coll, "' (", err,
                      "), so it was cleared. Please run indexing again to rebuild embeddings. Your manual tags are preserved."),
                      "db-cleared");
            sh.refresh_tags();
            if (sh.redraw_all) sh.redraw_all();
        });
    };
    sh.engine.on_job_dropped = [&](std::string title, std::string detail) {
        sh.notify("error", "Database", std::move(title), std::move(detail), "db-job-dropped");
    };
    sh.engine.encoder().on_state_changed = [&] {
        sh.call([&] {
            if (sh.engine.encoder().state() == TextEncoder::State::Failed) {
                sh.notify("error", "Search Model", "Text encoder failed to load",
                          to_utf8(sh.engine.encoder().error()), "encoder-failed");
            } else if (sh.engine.encoder().state() == TextEncoder::State::Ready) {
                sh.clear_notification_key("Search Model", "encoder-failed");
            }
            pill.on_encoder_changed();
            settings_win.invalidate();
        });
    };
    sh.engine.start(sh.settings);
    sh.engine.encoder().load_async(to_wide(sh.settings.model_dir), sh.settings.model_name);

    const auto open_settings_tab = [&](int tab) {
        settings_win.show(tab);
        if (sh.settings.hide_on_focus_loss && pill.visible()) pill.hide();
    };
    sh.open_settings = [&] { open_settings_tab(0); };
    sh.open_notifications = [&] { open_settings_tab(2); };
    sh.on_ui_opened = [&] { settings_win.maybe_auto_check_updates(); };
    sh.tags_refreshed = [&] { pill.on_tags_refreshed(); };
    sh.redraw_all = [&] {
        pill.invalidate();
        settings_win.invalidate();
        theme_editor_win.invalidate();
    };
    sh.theme_changed = [&] {
        pill.apply_theme();
        settings_win.apply_theme();
        theme_editor_win.apply_theme();
    };
    sh.settings_changed = [&] {
        sh.engine.encoder().load_async(to_wide(sh.settings.model_dir), sh.settings.model_name);
        sh.refresh_tags();
        sh.redraw_all();
    };

    const auto arm_idle_timer = [&] {
        KillTimer(pill.hwnd(), TIMER_IDLE_EXIT);
        if (!pill.visible() && !settings_win.visible() && !theme_editor_win.visible()) {
            sh.desktop_dupl.reset();
        }
        // A download or package install keeps the process alive even with both windows closed
        if (!pill.visible() && !settings_win.visible() && !theme_editor_win.visible() && !sh.test_mode && sh.background_jobs == 0) {
            SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
            if (sh.settings.ui_idle_timeout_s == 0) {
                PostQuitMessage(0);
            } else if (sh.settings.ui_idle_timeout_s > 0) {
                SetTimer(pill.hwnd(), TIMER_IDLE_EXIT, static_cast<UINT>(sh.settings.ui_idle_timeout_s) * 1000u, nullptr);
            }
        }
    };
    sh.visibility_changed = arm_idle_timer;

    const UINT m_toggle = msg_toggle_search();
    const UINT m_settings = msg_open_settings();
    const UINT m_open_notifs = msg_open_notifications();
    const UINT m_idx = msg_indexer_changed();
    const UINT m_notifs = msg_notifications_changed();
    const UINT m_quit = msg_quit_ui();

    pill.extra_handler = [&](UINT msg, WPARAM wp, LPARAM lp, bool& done) -> LRESULT {
        if (msg == m_quit && m_quit) {
            done = true;
            PostQuitMessage(0);
            return 0;
        }
        if (msg == m_toggle && m_toggle) {
            done = true;
            switch (static_cast<ToggleMode>(wp)) {
                case ToggleMode::Hide:
                    pill.tray_hide();
                    break;
                case ToggleMode::TraySelect:
                    pill.tray_select({.x = GET_X_LPARAM(lp), .y = GET_Y_LPARAM(lp)});
                    break;
                case ToggleMode::Toggle:
                default:
                    pill.toggle();
                    break;
            }
            return 0;
        }
        if (msg == m_settings && m_settings) {
            done = true;
            open_settings_tab(wp > 0 ? static_cast<int>(wp - 1) : -1);
            return 0;
        }
        if (msg == m_open_notifs && m_open_notifs) {
            done = true;
            if (sh.open_notifications) sh.open_notifications();
            return 0;
        }
        if (msg == m_idx && m_idx) {
            done = true;
            sh.engine.set_indexing_active(wp != 0);
            pill.on_indexer_changed();
            settings_win.invalidate();
            return 0;
        }
        if (msg == m_notifs && m_notifs) {
            done = true;
            sh.reload_notifications(true);
            if (sh.redraw_all) sh.redraw_all();
            return 0;
        }
        if (msg == WM_APP_RESULT) {
            done = true;
            std::unique_ptr<SearchResult> r{reinterpret_cast<SearchResult*>(lp)};
            if (r) pill.on_result(std::move(r));
            return 0;
        }
        if (msg == WM_TIMER && wp == TIMER_IDLE_EXIT) {
            done = true;
            KillTimer(pill.hwnd(), TIMER_IDLE_EXIT);
            if (!pill.visible() && !settings_win.visible() && !theme_editor_win.visible() && sh.background_jobs == 0) PostQuitMessage(0);
            return 0;
        }
        return 0;
    };

    if (!find_launcher() && !test_dir) {
        const fs::path launcher_exe = exe_dir() / L"usyseek.exe";
        std::error_code ec;
        if (fs::exists(launcher_exe, ec)) {
            std::wstring cmd = ulp::str::concat_strings(L"\"", launcher_exe.wstring(), L"\"");
            STARTUPINFOW si{.cb = sizeof(si)};
            wil::unique_process_information pi;
            if (CreateProcessW(launcher_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, exe_dir().c_str(), &si, &pi)) {
                for (int i = 0; i < 40 && !find_launcher(); ++i) Sleep(25);
            } else {
                sh.notify("warning", "Launcher", "Could not start background launcher",
                          ulp::str::concat_strings("Failed to launch usyseek.exe (", win32_error_utf8(GetLastError()), "). Indexing and tray controls require the launcher."), "launcher-missing");
            }
        } else {
            sh.notify("warning", "Launcher", "Background launcher not found",
                      "usyseek.exe was not found next to usyseek-ui.exe. Indexing, tray icon, and global hotkey are unavailable.", "launcher-missing");
        }
    }

    if (HWND launcher = find_launcher()) {
        send_cmd_value(launcher, Cmd::UiHello, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pill.hwnd())));
        sh.refresh_hotkey_state();
        sh.clear_notification_key("Launcher", "launcher-missing");
    } else if (!test_dir) {
        sh.notify("warning", "Launcher", "Background launcher did not respond",
                  "usyseek.exe did not become ready in time. Starting/stopping the indexer from Settings may fail.", "launcher-missing");
    }

    int code = 0;
    if (test_dir) {
        code = run_automated_test(sh, pill, settings_win, *test_dir);
    } else {
        if (show_notifications) {
            settings_win.show(2);
        } else if (show_settings) {
            settings_win.show(settings_tab);
        } else if (show_pill) {
            pill.show();
        }

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        code = static_cast<int>(msg.wParam);
    }

    sh.call_hwnd.store(nullptr, std::memory_order_release);
    sh.thumbs.stop();
    sh.previews.stop();
    sh.engine.stop();
    settings_win.stop();
    pill.stop_workers();

    MSG leftover;
    while (PeekMessageW(&leftover, pill.hwnd(), WM_APP_CALL, WM_APP_CALL, PM_REMOVE)) {
        if (leftover.wParam == 0 && leftover.lParam) {
            delete reinterpret_cast<std::function<void()>*>(leftover.lParam);
        }
    }
    while (PeekMessageW(&leftover, pill.hwnd(), WM_APP_RESULT, WM_APP_RESULT, PM_REMOVE)) {
        delete reinterpret_cast<SearchResult*>(leftover.lParam);
    }

    pill.release_com_objects();
    return code;
}
