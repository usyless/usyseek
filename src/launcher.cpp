// usyseek.exe - the always-running launcher.
// Owns the tray icon and global hotkey, supervises the search UI and indexer processes.
// Deliberately does nothing else so its working set stays tiny.

#include <usyseek/settings.hpp>
#include <usyseek/notifications.hpp>
#include <usyseek/status.hpp>
#include <usyseek/db.hpp>

#include <shellapi.h>
#include <wil/resource.h>

#include <utility>

#include "resource.h"

namespace {

using namespace usyseek;

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT TRAY_ID = 1;
constexpr int HOTKEY_ID = 1;
constexpr UINT_PTR TIMER_STOP_INDEX = 1;
constexpr UINT_PTR TIMER_TRAY_PROGRESS = 2;

enum MenuId : uint8_t {
    IdSearch = 100,
    IdSettings,
    IdCloseUi,
    IdToggleIndex,
    IdQuit,
};

class Launcher {
public:
    bool init(HINSTANCE inst) {
        inst_ = inst;
        bool settings_corrupt = false;
        settings_ = load_settings(&settings_corrupt);
        if (settings_corrupt) {
            push_notification_to_disk(
                "warning", "Settings", "Corrupted settings.json reset to defaults",
                "settings.json could not be parsed cleanly. Default settings are in use until you save new ones.",
                "settings-corrupt", false);
        }
        job_ = make_kill_on_close_job();
        stop_event_.reset(CreateEventW(nullptr, TRUE, FALSE, INDEXER_STOP_EVENT));
        db_want_event_.reset(CreateEventW(nullptr, TRUE, FALSE, INDEXER_DB_WANT_EVENT));
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");

        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &Launcher::wndproc;
        wc.hInstance = inst;
        wc.lpszClassName = LAUNCHER_CLASS;
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, LAUNCHER_CLASS, APP_NAME, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, this);
        if (!hwnd_) return false;
        // Allow the (possibly elevated-aware) children to reach us
        ChangeWindowMessageFilterEx(hwnd_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);

        add_tray();
        register_hotkey();
        return true;
    }

    int run() {
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
        for (;;) {
            std::array<HANDLE, 2> handles; // NOLINT(cppcoreguidelines-pro-type-member-init)
            DWORD n = 0;
            if (ui_.hProcess) handles[n++] = ui_.hProcess;
            if (indexer_.hProcess) handles[n++] = indexer_.hProcess;

            const DWORD r = MsgWaitForMultipleObjectsEx(n, handles.data(), INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (r < WAIT_OBJECT_0 + n) {
                on_process_exit(handles[r - WAIT_OBJECT_0]);
                continue;
            }
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) return static_cast<int>(msg.wParam);
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    void open_about() { open_settings(3); }

    void shutdown() {
        remove_tray();
        if (indexer_.hProcess) {
            SetEvent(stop_event_.get());
            if (WaitForSingleObject(indexer_.hProcess, 3000) != WAIT_OBJECT_0) TerminateProcess(indexer_.hProcess, 1);
            close_everything_instance();
        }
        if (db_want_event_) ResetEvent(db_want_event_.get());
        if (ui_.hProcess) TerminateProcess(ui_.hProcess, 0);
    }

private:
    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        }
        if (auto* self = reinterpret_cast<Launcher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
            return self->handle(hwnd, msg, wp, lp);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == taskbar_created_ && taskbar_created_) {
            add_tray();
            return 0;
        }
        if (msg == msg_toggle_search()) {
            toggle_search();
            return 0;
        }
        switch (msg) {
            case WM_HOTKEY:
                if (wp == HOTKEY_ID) toggle_search();
                return 0;
            case WM_TRAY:
                switch (LOWORD(lp)) {
                    case WM_LBUTTONDOWN:
                    case WM_LBUTTONDBLCLK:
                        tray_hide_on_select_ = is_pill_visible();
                        break;
                    case NIN_SELECT:
                        on_tray_select();
                        break;
                    case NIN_KEYSELECT:
                        tray_hide_on_select_ = false;
                        toggle_search();
                        break;
                    case WM_CONTEXTMENU:
                    case WM_RBUTTONUP:
                        tray_hide_on_select_ = false;
                        show_menu();
                        break;
                    case NIN_BALLOONUSERCLICK:
                        if (balloon_target_ == BalloonTarget::Notifications) open_notifications();
                        else if (balloon_target_ == BalloonTarget::About) open_settings(3);
                        else open_settings();
                        break;
                }
                return 0;
            case WM_COPYDATA:
                return on_copydata(reinterpret_cast<const COPYDATASTRUCT*>(lp));
            case WM_TIMER:
                if (wp == TIMER_STOP_INDEX) {
                    KillTimer(hwnd, TIMER_STOP_INDEX);
                    if (indexer_.hProcess) {
                        TerminateProcess(indexer_.hProcess, 1);
                        push_notification_to_disk(
                            "warning", "Indexer", "Indexer had to be force-stopped",
                            "usyseek-indexer.exe did not exit within 6 seconds after requesting stop and was terminated.",
                            "indexer-force-stop", true);
                    }
                } else if (wp == TIMER_TRAY_PROGRESS) {
                    if (indexer_.hProcess) update_tip();
                    else KillTimer(hwnd, TIMER_TRAY_PROGRESS);
                }
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT on_copydata(const COPYDATASTRUCT* cds) {
        const auto payload = [cds]<typename T>(T fallback) {
            if (cds->cbData >= sizeof(T) && cds->lpData) {
                T v;
                std::memcpy(&v, cds->lpData, sizeof(T));
                return v;
            }
            return fallback;
        };
        switch (static_cast<Cmd>(cds->dwData)) {
            case Cmd::UiHello:
                ui_hwnd_ = reinterpret_cast<HWND>(static_cast<uintptr_t>(payload(uint64_t{0})));
                return TRUE;
            case Cmd::StartIndex:
                start_index();
                return TRUE;
            case Cmd::StopIndex:
                stop_index();
                return TRUE;
            case Cmd::ReloadSettings:
                settings_ = load_settings();
                register_hotkey();
                return TRUE;
            case Cmd::SuspendHotkey:
                // The UI is recording a new shortcut: release the key but keep reporting the real state
                if (payload(uint32_t{0})) UnregisterHotKey(hwnd_, HOTKEY_ID);
                else register_hotkey();
                return TRUE;
            case Cmd::HotkeyStatus:
                return static_cast<LRESULT>(hotkey_state_);
            case Cmd::TestNotification:
                show_balloon(APP_NAME, L"Test Windows notification from usyseek. Click to open Notifications.", NIIF_INFO, BalloonTarget::Notifications);
                return TRUE;
            case Cmd::ShowUpdateBalloon: {
                std::wstring ver;
                if (cds->cbData > 0 && cds->lpData) {
                    ver = to_wide(std::string_view{static_cast<const char*>(cds->lpData), cds->cbData});
                }
                const std::wstring body = ver.empty()
                    ? std::wstring{L"A new version of usyseek is available. Click to view in About."}
                    : ulp::str::concat_strings(L"usyseek ", ver, L" is available. Click to view in About.");
                show_balloon(APP_NAME, body, NIIF_INFO, BalloonTarget::About);
                return TRUE;
            }
            case Cmd::Quit:
                DestroyWindow(hwnd_);
                return TRUE;
        }
        return FALSE;
    }

    // ------------------------------------------------------------------------------------------
    // Processes
    // ------------------------------------------------------------------------------------------

    wil::unique_process_information spawn(const fs::path& exe, std::wstring_view args, bool no_window, HANDLE std_out = nullptr) {
        wil::unique_process_information pi;
        std::wstring cmd = ulp::str::concat_strings(L"\"", exe.wstring(), L"\" ", args);
        STARTUPINFOW si{.cb = sizeof(STARTUPINFOW)};
        si.dwFlags |= STARTF_USESHOWWINDOW;
        si.wShowWindow = no_window ? SW_HIDE : SW_SHOWNORMAL;
        BOOL inherit = FALSE;
        if (std_out && std_out != INVALID_HANDLE_VALUE) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdInput = nullptr;
            si.hStdOutput = std_out;
            si.hStdError = std_out;
            inherit = TRUE;
        }
        DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
        if (no_window) flags |= CREATE_NO_WINDOW;
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, inherit, flags,
                            nullptr, exe_dir().c_str(), &si, &pi)) {
            const DWORD err = GetLastError();
            const std::string name = to_utf8(exe.filename().wstring());
            push_notification_to_disk(
                "error", "Launcher", ulp::str::concat_strings("Failed to start ", name),
                win32_error_utf8(err), ulp::str::concat_strings("spawn-", name), true);
            show_balloon(APP_NAME, ulp::str::concat_strings(L"Failed to start ", exe.filename().wstring(), L": ", to_wide(win32_error_utf8(err))), NIIF_ERROR, BalloonTarget::Notifications);
            return {};
        }
        AssignProcessToJobObject(job_.get(), pi.hProcess);
        ResumeThread(pi.hThread);
        if (!no_window) AllowSetForegroundWindow(pi.dwProcessId);
        return pi;
    }

    void spawn_ui(std::wstring_view arg) {
        ui_ = spawn(exe_dir() / L"usyseek-ui.exe", arg, false);
    }

    bool is_pill_visible() const noexcept {
        return ui_.hProcess && ui_hwnd_ && IsWindow(ui_hwnd_) && IsWindowVisible(ui_hwnd_);
    }

    void toggle_search() {
        if (ui_.hProcess && ui_hwnd_ && IsWindow(ui_hwnd_)) {
            AllowSetForegroundWindow(ui_.dwProcessId);
            PostMessageW(ui_hwnd_, msg_toggle_search(), static_cast<WPARAM>(ToggleMode::Toggle), 0);
        } else if (!ui_.hProcess) {
            spawn_ui(L"--show");
        }
    }

    void on_tray_select() {
        const bool hide_only = std::exchange(tray_hide_on_select_, false);
        if (ui_.hProcess && ui_hwnd_ && IsWindow(ui_hwnd_)) {
            if (hide_only) {
                PostMessageW(ui_hwnd_, msg_toggle_search(), static_cast<WPARAM>(ToggleMode::Hide), 0);
                return;
            }
            AllowSetForegroundWindow(ui_.dwProcessId);
            POINT pt{};
            GetCursorPos(&pt);
            PostMessageW(ui_hwnd_, msg_toggle_search(), static_cast<WPARAM>(ToggleMode::TraySelect), MAKELPARAM(pt.x, pt.y));
        } else if (!ui_.hProcess && !hide_only) {
            spawn_ui(L"--show");
        }
    }

    void open_settings(int tab = -1) {
        if (ui_.hProcess && ui_hwnd_ && IsWindow(ui_hwnd_)) {
            AllowSetForegroundWindow(ui_.dwProcessId);
            SetForegroundWindow(ui_hwnd_);
            PostMessageW(ui_hwnd_, msg_open_settings(), tab >= 0 ? static_cast<WPARAM>(tab + 1) : 0, 0);
        } else if (!ui_.hProcess) {
            if (tab >= 0) {
                FixedWString<32> arg{L"--settings-tab "};
                arg.append(to_fixed_wstr(tab));
                spawn_ui(arg);
            } else {
                spawn_ui(L"--settings");
            }
        }
    }

    void open_notifications() {
        if (ui_.hProcess && ui_hwnd_ && IsWindow(ui_hwnd_)) {
            AllowSetForegroundWindow(ui_.dwProcessId);
            SetForegroundWindow(ui_hwnd_);
            PostMessageW(ui_hwnd_, msg_open_notifications(), 0, 0);
        } else if (!ui_.hProcess) {
            spawn_ui(L"--notifications");
        }
    }

    bool is_ui_running() const noexcept {
        if (ui_.hProcess && WaitForSingleObject(ui_.hProcess, 0) == WAIT_TIMEOUT) return true;
        if (ui_hwnd_ && IsWindow(ui_hwnd_)) return true;
        return FindWindowW(PILL_CLASS, nullptr) != nullptr;
    }

    void close_ui() {
        HWND h = (ui_hwnd_ && IsWindow(ui_hwnd_)) ? ui_hwnd_ : FindWindowW(PILL_CLASS, nullptr);
        DWORD ext_pid = 0;
        if (h) {
            GetWindowThreadProcessId(h, &ext_pid);
            PostMessageW(h, msg_quit_ui(), 0, 0);
        }
        if (ui_.hProcess) {
            if (WaitForSingleObject(ui_.hProcess, 400) != WAIT_OBJECT_0) {
                TerminateProcess(ui_.hProcess, 0);
                WaitForSingleObject(ui_.hProcess, 1000);
            }
            ui_.reset();
        } else if (ext_pid != 0 && ext_pid != GetCurrentProcessId()) {
            wil::unique_handle proc{OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, ext_pid)};
            if (proc) {
                if (WaitForSingleObject(proc.get(), 400) != WAIT_OBJECT_0) {
                    TerminateProcess(proc.get(), 0);
                }
            }
        }
        ui_hwnd_ = nullptr;
    }

    void start_index() {
        if (indexer_.hProcess) return;
        settings_ = load_settings();
        if (db_want_event_) SetEvent(db_want_event_.get());
        notify_ui(true);
        ResetEvent(stop_event_.get());

        std::error_code ec;
        fs::create_directories(data_dir(), ec);
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        wil::unique_hfile log_file{CreateFileW((data_dir() / L"indexer.log").c_str(), GENERIC_WRITE,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                                               FILE_ATTRIBUTE_NORMAL, nullptr)};
        indexer_ = spawn(exe_dir() / L"usyseek-indexer.exe", L"", true, log_file.get());
        stopping_index_ = false;
        indexer_status_.reset();
        if (indexer_.hProcess) {
            SetTimer(hwnd_, TIMER_TRAY_PROGRESS, 1000, nullptr);
        } else if (db_want_event_) {
            ResetEvent(db_want_event_.get());
        }
        notify_ui();
        update_tip();
    }

    void stop_index() {
        if (!indexer_.hProcess) return;
        stopping_index_ = true;
        SetEvent(stop_event_.get());
        SetTimer(hwnd_, TIMER_STOP_INDEX, 6000, nullptr);
        notify_ui();
        update_tip();
    }

    static void close_everything_instance() {
        const FixedWString<64> cls = [&] {
            FixedWString<64> s{L"EVERYTHING_TASKBAR_NOTIFICATION_("};
            s.append(EVERYTHING_INSTANCE);
            s.push_back(L')');
            return s;
        }();
        if (HWND h = FindWindowW(cls.c_str(), nullptr)) {
            DWORD_PTR r = 0;
            SendMessageTimeoutW(h, WM_USER, 4 /* EVERYTHING_IPC_EXIT */, 0, SMTO_ABORTIFHUNG, 2000, &r);
        }
    }

    void on_process_exit(HANDLE h) {
        if (h == ui_.hProcess) {
            DWORD code = 0;
            GetExitCodeProcess(ui_.hProcess, &code);
            ui_.reset();
            ui_hwnd_ = nullptr;
            tray_hide_on_select_ = false;
            if (code != 0) {
                push_notification_to_disk(
                    "error", "Launcher", "UI process exited unexpectedly",
                    ulp::str::concat_strings("usyseek-ui.exe terminated with exit code ", to_fixed_str(code), "."),
                    "ui-exit", false);
            }
        } else if (h == indexer_.hProcess) {
            DWORD code = 0;
            GetExitCodeProcess(indexer_.hProcess, &code);
            KillTimer(hwnd_, TIMER_STOP_INDEX);
            KillTimer(hwnd_, TIMER_TRAY_PROGRESS);
            indexer_.reset();
            if (db_want_event_) ResetEvent(db_want_event_.get());
            const bool was_stopping = stopping_index_;
            stopping_index_ = false;
            std::wstring status_msg;
            if (auto b = indexer_status_ ? std::move(indexer_status_) : StatusBlock::open()) {
                const StatusData d = b->read();
                if (d.message[0]) status_msg = d.message;
            }
            indexer_status_.reset();
            close_everything_instance();
            if (code != 0 && code != 2 && code != 3) {
                if (status_msg.empty()) status_msg = to_wide(read_indexer_log_error());
                const std::wstring msg = status_msg.empty()
                    ? L"Indexing failed. See data\\indexer.log for details."
                    : ulp::str::concat_strings(L"Indexing failed: ", status_msg);
                if (!was_stopping) {
                    push_notification_to_disk(
                        "error", "Indexer", "Indexing ended with errors",
                        ulp::str::concat_strings(to_utf8(msg), " (exit code ", to_fixed_str(code), ", log: data\\indexer.log)"),
                        "indexer-exit", true);
                }
                if (!is_ui_running()) {
                    show_balloon(APP_NAME, msg, NIIF_ERROR, BalloonTarget::Notifications);
                }
            }
            notify_ui();
            update_tip();
        }
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
    }

    void notify_ui(std::optional<bool> force_active = std::nullopt) noexcept {
        const bool active = force_active.value_or(indexer_.hProcess != nullptr);
        if (ui_hwnd_ && IsWindow(ui_hwnd_)) PostMessageW(ui_hwnd_, msg_indexer_changed(), active ? 1 : 0, 0);
    }

    // ------------------------------------------------------------------------------------------
    // Tray / hotkey
    // ------------------------------------------------------------------------------------------

    enum class BalloonTarget : uint8_t { Settings, Notifications, About };

    void show_balloon(const wchar_t* title, const std::wstring& body, DWORD flags, BalloonTarget target) {
        balloon_target_ = target;
        NOTIFYICONDATAW nid = tray_data();
        nid.uFlags = NIF_INFO;
        wcscpy_s(nid.szInfoTitle, title);
        wcsncpy_s(nid.szInfo, body.c_str(), _TRUNCATE);
        nid.dwInfoFlags = flags;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void register_hotkey() {
        UnregisterHotKey(hwnd_, HOTKEY_ID);
        if (!settings_.hotkey_enabled || settings_.hotkey_vk == 0) {
            hotkey_state_ = HotkeyState::Disabled;
            clear_notification_key_on_disk("Hotkeys", "hotkey-unavailable");
            return;
        }
        if (RegisterHotKey(hwnd_, HOTKEY_ID, settings_.hotkey_mods | MOD_NOREPEAT, settings_.hotkey_vk)) {
            hotkey_state_ = HotkeyState::Registered;
            clear_notification_key_on_disk("Hotkeys", "hotkey-unavailable");
            return;
        }
        // Tell the user once per combination; the UI shows the state for as long as it lasts
        const bool already_reported = hotkey_state_ == HotkeyState::Unavailable &&
                                      failed_mods_ == settings_.hotkey_mods && failed_vk_ == settings_.hotkey_vk;
        hotkey_state_ = HotkeyState::Unavailable;
        failed_mods_ = settings_.hotkey_mods;
        failed_vk_ = settings_.hotkey_vk;
        push_notification_to_disk(
            "warning", "Hotkeys", "Global shortcut is already in use",
            ulp::str::concat_strings(to_utf8(hotkey_text(settings_)), " could not be registered because another application already owns it. Pick a different shortcut in Settings."),
            "hotkey-unavailable", true);
        if (!already_reported) {
            show_balloon(APP_NAME, L"The search shortcut is already used by another app. Pick a different one in Settings.", NIIF_WARNING, BalloonTarget::Settings);
        }
    }

    NOTIFYICONDATAW tray_data() const noexcept {
        NOTIFYICONDATAW nid{sizeof(nid)};
        nid.hWnd = hwnd_;
        nid.uID = TRAY_ID;
        return nid;
    }

    void add_tray() {
        last_tip_.clear();
        NOTIFYICONDATAW nid = tray_data();
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = WM_TRAY;
        tray_icon_.reset(static_cast<HICON>(LoadImageW(inst_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR)));
        nid.hIcon = tray_icon_.get();
        wcscpy_s(nid.szTip, APP_NAME);
        Shell_NotifyIconW(NIM_ADD, &nid);
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        update_tip();
    }

    void remove_tray() noexcept {
        NOTIFYICONDATAW nid = tray_data();
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }

    static std::string read_indexer_log_error() {
        wil::unique_hfile f{CreateFileW((data_dir() / L"indexer.log").c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (!f) return {};
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(f.get(), &size) || size.QuadPart <= 0) return {};
        constexpr LONGLONG TAIL = 8192;
        if (size.QuadPart > TAIL) {
            LARGE_INTEGER off{};
            off.QuadPart = size.QuadPart - TAIL;
            SetFilePointerEx(f.get(), off, nullptr, FILE_BEGIN);
        }
        const auto want = static_cast<DWORD>(std::min<LONGLONG>(size.QuadPart, TAIL));
        std::array<char, TAIL> buf; // NOLINT(cppcoreguidelines-pro-type-member-init)
        DWORD got = 0;
        if (!ReadFile(f.get(), buf.data(), want, &got, nullptr) || got == 0) return {};
        const std::string_view content{buf.data(), got};

        std::string_view last_line;
        std::string_view best_line;
        size_t pos = 0;
        while (pos < content.size()) {
            const size_t nl = content.find('\n', pos);
            const size_t end = (nl == std::string_view::npos) ? content.size() : nl;
            std::string_view line = content.substr(pos, end - pos);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
            if (!line.empty()) {
                last_line = line;
                if (line.contains("runtime error:") ||
                    line.contains("AddressSanitizer") ||
                    line.contains("UndefinedBehaviorSanitizer") ||
                    line.contains("ERROR:") ||
                    line.contains("[Error]")) {
                    best_line = line;
                }
            }
            if (nl == std::string_view::npos) break;
            pos = nl + 1;
        }
        std::string out{!best_line.empty() ? best_line : last_line};
        if (out.size() > 220) {
            out.resize(217);
            out += "...";
        }
        return out;
    }

    std::wstring format_index_progress(bool multiline) const {
        if (!indexer_.hProcess) return {};
        if (!indexer_status_) indexer_status_ = StatusBlock::open();
        if (indexer_status_) {
            const StatusData d = indexer_status_->read();
            if (stopping_index_) {
                if (d.found > 0) {
                    const double pct = std::clamp(100.0 * static_cast<double>(d.embedded) / static_cast<double>(d.found), 0.0, 100.0);
                    return ulp::str::concat_strings(
                        L"Stopping indexer… (", format_count(d.embedded), L" / ", format_count(d.found),
                        L" • ", to_fixed_wstr(pct, 1), L"%)");
                }
                return L"Stopping indexer…";
            }
            if (d.phase == Phase::Embedding || d.found > 0) {
                const double pct = d.found > 0
                    ? std::clamp(100.0 * static_cast<double>(d.embedded) / static_cast<double>(d.found), 0.0, 100.0)
                    : 0.0;
                std::wstring line = ulp::str::concat_strings(
                    L"Indexing ", to_fixed_wstr(pct, 1), L"%", multiline ? L"\n" : L"  •  ",
                    format_count(d.embedded), L" / ", format_count(d.found));
                if (d.rate > 0.05f) {
                    append_to(line, L" • ", to_fixed_wstr(d.rate, 1), L"/s");
                    if (d.todo > d.processed) {
                        const uint64_t rem_ms = static_cast<uint64_t>((d.todo - d.processed) / d.rate * 1000.0f);
                        append_to(line, L" • ETA ", format_duration_ms(rem_ms));
                    }
                }
                return line;
            }
            std::wstring line = phase_label(d.phase);
            if (d.message[0]) append_to(line, L" — ", d.message);
            return line;
        }
        return stopping_index_ ? L"Stopping indexer…" : L"Indexing…";
    }

    std::wstring status_line() const {
        if (!indexer_.hProcess) return ulp::str::concat_strings(APP_NAME, L"  v", APP_VERSION);
        return format_index_progress(false);
    }

    void update_tip() {
        std::wstring tip = APP_NAME;
        if (indexer_.hProcess) {
            const std::wstring prog = format_index_progress(true);
            if (!prog.empty()) append_to(tip, L" — ", prog);
        }
        if (tip.size() >= 127) tip.resize(127);
        if (tip == last_tip_) return;
        last_tip_ = tip;

        NOTIFYICONDATAW nid = tray_data();
        nid.uFlags = NIF_TIP | NIF_SHOWTIP;
        wcscpy_s(nid.szTip, tip.c_str());
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    static FixedWString<96> hotkey_text(const Settings& s) noexcept {
        FixedWString<96> t;
        if (s.hotkey_mods & MOD_CONTROL) t.append(L"Ctrl+");
        if (s.hotkey_mods & MOD_ALT) t.append(L"Alt+");
        if (s.hotkey_mods & MOD_SHIFT) t.append(L"Shift+");
        if (s.hotkey_mods & MOD_WIN) t.append(L"Win+");
        std::array<wchar_t, 64> name; // NOLINT(cppcoreguidelines-pro-type-member-init)
        const UINT sc = MapVirtualKeyW(s.hotkey_vk, MAPVK_VK_TO_VSC) << 16;
        if (s.hotkey_vk == VK_SPACE) {
            wcscpy_s(name.data(), name.size(), L"Space");
        } else if (GetKeyNameTextW(static_cast<LONG>(sc), name.data(), static_cast<int>(name.size())) == 0) {
            name[0] = L'\0';
        }
        t.append(name.data());
        return t;
    }

    void show_menu() {
        update_tip();
        wil::unique_hmenu menu{CreatePopupMenu()};
        const std::wstring header = status_line();
        AppendMenuW(menu.get(), MF_STRING | MF_DISABLED, 0, header.c_str());
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        std::wstring search = L"Open Search";
        if (settings_.hotkey_enabled && hotkey_state_ == HotkeyState::Registered) append_to(search, L"\t", hotkey_text(settings_));
        AppendMenuW(menu.get(), MF_STRING, IdSearch, search.c_str());
        AppendMenuW(menu.get(), MF_STRING, IdSettings, L"Open Settings");
        const bool ui_alive = is_ui_running();
        AppendMenuW(menu.get(), MF_STRING | (ui_alive ? 0 : (MF_DISABLED | MF_GRAYED)), IdCloseUi,
                    ui_alive ? L"Close UI Process" : L"Close UI Process (Not Running)");
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu.get(), MF_STRING, IdToggleIndex,
                    indexer_.hProcess ? (stopping_index_ ? L"Stopping Indexing…" : L"Stop Indexing") : L"Start Indexing");
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu.get(), MF_STRING, IdQuit, L"Quit usyseek");
        SetMenuDefaultItem(menu.get(), IdSearch, FALSE);

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, hwnd_, nullptr));
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        switch (cmd) {
            case IdSearch: toggle_search(); break;
            case IdSettings: open_settings(); break;
            case IdCloseUi: close_ui(); break;
            case IdToggleIndex: indexer_.hProcess ? stop_index() : start_index(); break;
            case IdQuit: DestroyWindow(hwnd_); break;
            default: break;
        }
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
    }

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND ui_hwnd_ = nullptr;
    UINT taskbar_created_ = 0;
    Settings settings_;
    HotkeyState hotkey_state_ = HotkeyState::Disabled;
    uint32_t failed_mods_ = 0, failed_vk_ = 0;
    wil::unique_handle job_;
    wil::unique_event_nothrow stop_event_;
    wil::unique_event_nothrow db_want_event_;
    wil::unique_process_information ui_;
    wil::unique_process_information indexer_;
    wil::unique_hicon tray_icon_;
    mutable std::optional<StatusBlock> indexer_status_;
    bool stopping_index_ = false;
    bool tray_hide_on_select_ = false;
    BalloonTarget balloon_target_ = BalloonTarget::Notifications;
    std::wstring last_tip_;
};

int run_apply_update(DWORD wait_pid, const fs::path& staging_root, const fs::path& target_dir, bool open_about) {
    if (wait_pid != 0) {
        if (const wil::unique_handle proc{OpenProcess(SYNCHRONIZE, FALSE, wait_pid)}) {
            WaitForSingleObject(proc.get(), 15'000);
        }
    }

    std::error_code ec;
    const auto copy_one = [&](const fs::path& src, const fs::path& dst) -> bool {
        for (int attempt = 0; attempt < 30; ++attempt) {
            if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) return true;
            const DWORD err = GetLastError();
            if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION || err == ERROR_ACCESS_DENIED) {
                if (attempt >= 5) {
                    const fs::path backup = ulp::str::concat_strings(dst.wstring(), L".old");
                    if (MoveFileExW(dst.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                        MoveFileExW(backup.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
                        if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) return true;
                    }
                }
                Sleep(150);
                continue;
            }
            return false;
        }
        return false;
    };

    for (const auto& entry : fs::recursive_directory_iterator(staging_root, ec)) {
        if (ec) {
            MessageBoxW(nullptr, ulp::str::concat_strings(L"Failed to read update staging directory: ", to_wide(ec.message())).c_str(),
                        APP_NAME, MB_ICONERROR | MB_OK);
            return 1;
        }
        const fs::path rel = fs::relative(entry.path(), staging_root, ec);
        if (ec || rel.empty()) continue;
        // Never overwrite user data/
        if (_wcsicmp(rel.begin()->c_str(), L"data") == 0) continue;

        const fs::path dst = target_dir / rel;
        if (entry.is_directory(ec)) {
            fs::create_directories(dst, ec);
            if (ec) {
                MessageBoxW(nullptr, ulp::str::concat_strings(L"Could not create directory ", dst.wstring()).c_str(), APP_NAME, MB_ICONERROR | MB_OK);
                return 1;
            }
        } else if (entry.is_regular_file(ec)) {
            fs::create_directories(dst.parent_path(), ec);
            if (!copy_one(entry.path(), dst)) {
                const std::wstring msg = ulp::str::concat_strings(
                    L"Could not update ", dst.filename().wstring(),
                    L" (", to_wide(win32_error_utf8(GetLastError())), L").");
                MessageBoxW(nullptr, msg.c_str(), APP_NAME, MB_ICONERROR | MB_OK);
                return 1;
            }
        }
    }

    const fs::path next_exe = target_dir / L"usyseek.exe";
    const fs::path update_dir = target_dir / L"data" / L"update";
    std::wstring cmd = ulp::str::concat_strings(
        L"\"", next_exe.wstring(), L"\" --cleanup-update \"", update_dir.wstring(), L"\"",
        open_about ? L" --open-about" : L"");
    STARTUPINFOW si{.cb = sizeof(STARTUPINFOW)};
    wil::unique_process_information pi;
    if (!CreateProcessW(next_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, nullptr, target_dir.c_str(), &si, &pi)) {
        MessageBoxW(nullptr, ulp::str::concat_strings(L"Update was installed, but usyseek.exe could not restart: ", to_wide(win32_error_utf8(GetLastError()))).c_str(),
                    APP_NAME, MB_ICONWARNING | MB_OK);
        return 1;
    }
    return 0;
}

void cleanup_update_artifacts(const fs::path& update_dir, const fs::path& target_dir) {
    std::error_code ec;
    for (int attempt = 0; attempt < 20 && fs::exists(update_dir, ec); ++attempt) {
        fs::remove_all(update_dir, ec);
        if (!fs::exists(update_dir, ec)) break;
        Sleep(150);
    }
    for (const auto& entry : fs::recursive_directory_iterator(target_dir, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec) && entry.path().extension() == L".old") {
            fs::remove(entry.path(), ec);
        }
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdline, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 5 && std::wstring_view{argv[1]} == L"--apply-update") {
        const DWORD wait_pid = static_cast<DWORD>(_wtol(argv[2]));
        const fs::path staging_root{argv[3]};
        const fs::path target_dir{argv[4]};
        bool open_about = false;
        for (int i = 5; i < argc; ++i) {
            if (std::wstring_view{argv[i]} == L"--open-about") open_about = true;
        }
        LocalFree(static_cast<HLOCAL>(argv));
        return run_apply_update(wait_pid, staging_root, target_dir, open_about);
    }

    fs::path cleanup_dir;
    bool open_about = false;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view a{argv[i]};
            if (a == L"--cleanup-update" && i + 1 < argc) cleanup_dir = fs::path{argv[++i]};
            else if (a == L"--open-about") open_about = true;
        }
        LocalFree(static_cast<HLOCAL>(argv));
    }
    if (!cleanup_dir.empty()) {
        cleanup_update_artifacts(cleanup_dir, exe_dir());
    }

    wil::unique_mutex_nothrow single{CreateMutexW(nullptr, TRUE, LAUNCHER_MUTEX)};
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(LAUNCHER_CLASS, nullptr)) {
            const std::wstring_view args{cmdline ? cmdline : L""};
            if (args.contains(L"--index")) send_cmd(existing, Cmd::StartIndex);
            else PostMessageW(existing, msg_toggle_search(), 0, 0);
        }
        return 0;
    }

    ulp::win::dark::init_dark_mode();

    Launcher launcher;
    if (!launcher.init(inst)) {
        MessageBoxW(nullptr, L"Could not initialize the background usyseek launcher window.", APP_NAME, MB_ICONERROR | MB_OK);
        return 1;
    }
    if (open_about) launcher.open_about();
    const int code = launcher.run();
    launcher.shutdown();
    return code;
}
