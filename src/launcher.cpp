// usyseek.exe - the always-running launcher.
// Owns the tray icon and global hotkey, supervises the search UI, indexer and qdrant processes.
// Deliberately does nothing else so its working set stays tiny.

#include <usyseek/settings.hpp>
#include <usyseek/status.hpp>
#include <usyseek/db.hpp>

#include <shellapi.h>
#include <wil/resource.h>

#include "resource.h"

namespace {

using namespace usyseek;

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT TRAY_ID = 1;
constexpr int HOTKEY_ID = 1;
constexpr UINT_PTR TIMER_STOP_INDEX = 1;

enum MenuId : UINT {
    IdSearch = 100,
    IdSettings,
    IdToggleIndex,
    IdQuit,
};

class Launcher {
public:
    bool init(HINSTANCE inst) {
        inst_ = inst;
        settings_ = load_settings();
        job_ = make_kill_on_close_job();
        stop_event_.reset(CreateEventW(nullptr, TRUE, FALSE, INDEXER_STOP_EVENT));
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
            HANDLE handles[3];
            DWORD n = 0;
            if (ui_.hProcess) handles[n++] = ui_.hProcess;
            if (indexer_.hProcess) handles[n++] = indexer_.hProcess;
            if (qdrant_.hProcess) handles[n++] = qdrant_.hProcess;

            const DWORD r = MsgWaitForMultipleObjectsEx(n, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
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

    void shutdown() {
        remove_tray();
        if (indexer_.hProcess) {
            SetEvent(stop_event_.get());
            if (WaitForSingleObject(indexer_.hProcess, 3000) != WAIT_OBJECT_0) TerminateProcess(indexer_.hProcess, 1);
            close_everything_instance();
        }
        for (auto* p : {&ui_, &qdrant_}) {
            if (p->hProcess) TerminateProcess(p->hProcess, 0);
        }
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
                    case NIN_SELECT:
                    case NIN_KEYSELECT:
                        toggle_search();
                        break;
                    case WM_CONTEXTMENU:
                    case WM_RBUTTONUP:
                        show_menu();
                        break;
                }
                return 0;
            case WM_COPYDATA:
                return on_copydata(reinterpret_cast<const COPYDATASTRUCT*>(lp));
            case WM_TIMER:
                if (wp == TIMER_STOP_INDEX) {
                    KillTimer(hwnd, TIMER_STOP_INDEX);
                    if (indexer_.hProcess) TerminateProcess(indexer_.hProcess, 1);
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
                ensure_qdrant();
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
                if (payload(uint32_t{0})) UnregisterHotKey(hwnd_, HOTKEY_ID);
                else register_hotkey();
                return TRUE;
            case Cmd::NeedQdrant:
                ensure_qdrant();
                return TRUE;
            case Cmd::Quit:
                DestroyWindow(hwnd_);
                return TRUE;
        }
        return FALSE;
    }

    // ------------------------------------------------------------------------------------------
    // Processes
    // ------------------------------------------------------------------------------------------

    wil::unique_process_information spawn(const fs::path& exe, std::wstring args, bool console_log, const wchar_t* log_name,
                                          const std::wstring* env = nullptr) {
        wil::unique_process_information pi;
        std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
        STARTUPINFOW si{sizeof(si)};
        wil::unique_handle log;
        if (console_log) {
            SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
            log.reset(CreateFileW((data_dir() / log_name).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (log.get() == INVALID_HANDLE_VALUE) log.release();
            if (log) {
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdOutput = log.get();
                si.hStdError = log.get();
            }
        }
        DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
        if (console_log) flags |= CREATE_NO_WINDOW;
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, log ? TRUE : FALSE, flags,
                            env ? const_cast<wchar_t*>(env->data()) : nullptr, exe_dir().c_str(), &si, &pi)) {
            return {};
        }
        AssignProcessToJobObject(job_.get(), pi.hProcess);
        ResumeThread(pi.hThread);
        AllowSetForegroundWindow(pi.dwProcessId);
        return pi;
    }

    void ensure_qdrant() {
        if (qdrant_.hProcess) return;
        qdrant_ = start_qdrant(settings_, job_.get());
    }

    void maybe_stop_qdrant() {
        if (!ui_.hProcess && !indexer_.hProcess && qdrant_.hProcess) {
            TerminateProcess(qdrant_.hProcess, 0);
            WaitForSingleObject(qdrant_.hProcess, 2000);
            qdrant_.reset();
        }
    }

    void spawn_ui(const wchar_t* arg) {
        ensure_qdrant();
        ui_ = spawn(exe_dir() / L"usyseek-ui.exe", arg, false, nullptr);
    }

    void toggle_search() {
        if (ui_.hProcess && ui_hwnd_ && IsWindow(ui_hwnd_)) {
            AllowSetForegroundWindow(ui_.dwProcessId);
            SetForegroundWindow(ui_hwnd_);
            PostMessageW(ui_hwnd_, msg_toggle_search(), 0, 0);
        } else if (!ui_.hProcess) {
            spawn_ui(L"--show");
        }
    }

    void open_settings() {
        if (ui_.hProcess && ui_hwnd_ && IsWindow(ui_hwnd_)) {
            AllowSetForegroundWindow(ui_.dwProcessId);
            SetForegroundWindow(ui_hwnd_);
            PostMessageW(ui_hwnd_, msg_open_settings(), 0, 0);
        } else if (!ui_.hProcess) {
            spawn_ui(L"--settings");
        }
    }

    void start_index() {
        if (indexer_.hProcess) return;
        settings_ = load_settings();
        ensure_qdrant();
        ResetEvent(stop_event_.get());

        // python3XX.dll is resolved at load time, so expose the venv's base interpreter on PATH
        std::wstring env;
        const VenvInfo venv = read_venv(to_wide(settings_.venv_dir));
        std::wstring prefix;
        if (!venv.home.empty()) prefix = venv.home + L";" + venv.home + L"\\DLLs;" + to_wide(settings_.venv_dir) + L"\\Scripts;";
        if (wchar_t* block = GetEnvironmentStringsW()) {
            for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
                if (_wcsnicmp(p, L"PATH=", 5) == 0) {
                    env += L"PATH=" + prefix + (p + 5);
                } else if (_wcsnicmp(p, L"PYTHON", 6) == 0) {
                    continue; // never let a global PYTHONHOME/PYTHONPATH leak into the embedded interpreter
                } else {
                    env += p;
                }
                env.push_back(L'\0');
            }
            FreeEnvironmentStringsW(block);
        }
        env += L"PYTHONUNBUFFERED=1";
        env.push_back(L'\0');
        env.push_back(L'\0');

        indexer_ = spawn(exe_dir() / L"usyseek-indexer.exe", L"", true, L"indexer.log", &env);
        stopping_index_ = false;
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

    void close_everything_instance() {
        const std::wstring cls = std::wstring{L"EVERYTHING_TASKBAR_NOTIFICATION_("} + EVERYTHING_INSTANCE + L")";
        if (HWND h = FindWindowW(cls.c_str(), nullptr)) {
            DWORD_PTR r = 0;
            SendMessageTimeoutW(h, WM_USER, 4 /* EVERYTHING_IPC_EXIT */, 0, SMTO_ABORTIFHUNG, 2000, &r);
        }
    }

    void on_process_exit(HANDLE h) {
        if (h == ui_.hProcess) {
            ui_.reset();
            ui_hwnd_ = nullptr;
            maybe_stop_qdrant();
        } else if (h == indexer_.hProcess) {
            KillTimer(hwnd_, TIMER_STOP_INDEX);
            indexer_.reset();
            stopping_index_ = false;
            close_everything_instance();
            notify_ui();
            maybe_stop_qdrant();
            update_tip();
        } else if (h == qdrant_.hProcess) {
            qdrant_.reset();
        }
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
    }

    void notify_ui() {
        if (ui_hwnd_ && IsWindow(ui_hwnd_)) PostMessageW(ui_hwnd_, msg_indexer_changed(), indexer_.hProcess ? 1 : 0, 0);
    }

    // ------------------------------------------------------------------------------------------
    // Tray / hotkey
    // ------------------------------------------------------------------------------------------

    void register_hotkey() {
        UnregisterHotKey(hwnd_, HOTKEY_ID);
        if (!settings_.hotkey_enabled || settings_.hotkey_vk == 0) return;
        if (!RegisterHotKey(hwnd_, HOTKEY_ID, settings_.hotkey_mods | MOD_NOREPEAT, settings_.hotkey_vk)) {
            NOTIFYICONDATAW nid = tray_data();
            nid.uFlags = NIF_INFO;
            wcscpy_s(nid.szInfoTitle, APP_NAME);
            wcscpy_s(nid.szInfo, L"The search shortcut is already used by another app.");
            nid.dwInfoFlags = NIIF_WARNING;
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
    }

    NOTIFYICONDATAW tray_data() const {
        NOTIFYICONDATAW nid{sizeof(nid)};
        nid.hWnd = hwnd_;
        nid.uID = TRAY_ID;
        return nid;
    }

    void add_tray() {
        NOTIFYICONDATAW nid = tray_data();
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = WM_TRAY;
        nid.hIcon = static_cast<HICON>(LoadImageW(inst_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        wcscpy_s(nid.szTip, APP_NAME);
        Shell_NotifyIconW(NIM_ADD, &nid);
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }

    void remove_tray() {
        NOTIFYICONDATAW nid = tray_data();
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }

    std::wstring status_line() const {
        if (!indexer_.hProcess) return std::wstring{APP_NAME} + L"  v" + APP_VERSION;
        if (stopping_index_) return L"Stopping indexer…";
        std::wstring line = L"Indexing";
        if (auto block = StatusBlock::open()) {
            const StatusData d = block->read();
            if (d.phase == Phase::Embedding) line += L" (" + format_count(d.embedded) + L" / " + format_count(d.found) + L")";
            else line = phase_label(d.phase);
        }
        return line;
    }

    void update_tip() {
        NOTIFYICONDATAW nid = tray_data();
        nid.uFlags = NIF_TIP | NIF_SHOWTIP;
        const std::wstring tip = indexer_.hProcess
            ? (std::wstring{APP_NAME} + (stopping_index_ ? L" - stopping indexer" : L" - indexing"))
            : std::wstring{APP_NAME};
        wcscpy_s(nid.szTip, tip.c_str());
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    static std::wstring hotkey_text(const Settings& s) {
        std::wstring t;
        if (s.hotkey_mods & MOD_CONTROL) t += L"Ctrl+";
        if (s.hotkey_mods & MOD_ALT) t += L"Alt+";
        if (s.hotkey_mods & MOD_SHIFT) t += L"Shift+";
        if (s.hotkey_mods & MOD_WIN) t += L"Win+";
        wchar_t name[64]{};
        UINT sc = MapVirtualKeyW(s.hotkey_vk, MAPVK_VK_TO_VSC) << 16;
        if (s.hotkey_vk == VK_SPACE) wcscpy_s(name, L"Space");
        else GetKeyNameTextW(static_cast<LONG>(sc), name, 64);
        return t + name;
    }

    void show_menu() {
        wil::unique_hmenu menu{CreatePopupMenu()};
        const std::wstring header = status_line();
        AppendMenuW(menu.get(), MF_STRING | MF_DISABLED, 0, header.c_str());
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        std::wstring search = L"Open Search";
        if (settings_.hotkey_enabled) search += L"\t" + hotkey_text(settings_);
        AppendMenuW(menu.get(), MF_STRING, IdSearch, search.c_str());
        AppendMenuW(menu.get(), MF_STRING, IdSettings, L"Open Settings");
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
    wil::unique_handle job_;
    wil::unique_event_nothrow stop_event_;
    wil::unique_process_information ui_;
    wil::unique_process_information indexer_;
    wil::unique_process_information qdrant_;
    bool stopping_index_ = false;
};

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdline, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    wil::unique_mutex_nothrow single{CreateMutexW(nullptr, TRUE, LAUNCHER_MUTEX)};
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(LAUNCHER_CLASS, nullptr)) {
            const std::wstring_view args{cmdline ? cmdline : L""};
            if (args.find(L"--index") != std::wstring_view::npos) send_cmd(existing, Cmd::StartIndex);
            else PostMessageW(existing, msg_toggle_search(), 0, 0);
        }
        return 0;
    }

    ulp::win::dark::init_dark_mode();

    Launcher launcher;
    if (!launcher.init(inst)) return 1;
    const int code = launcher.run();
    launcher.shutdown();
    return code;
}
