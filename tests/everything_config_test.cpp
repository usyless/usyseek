#include "check.hpp"
#include "usyseek/indexer/everything_config.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_set>

using usyseek::indexer::EVERYTHING_LEAN_CONFIG;
using usyseek::indexer::USN_SENTINEL_EXT_ASCII;
using usyseek::indexer::build_everything_args;
using usyseek::indexer::build_everything_include_only_files;
using usyseek::indexer::build_everything_lean_config;
using usyseek::indexer::is_usn_journal_filesystem;
using usyseek::indexer::join_sentinel_path;
using usyseek::indexer::make_usn_sentinel_name;

namespace {

bool has_line(std::string_view text, std::string_view line) {
    return text.contains("\r\n" + std::string{line} + "\r\n");
}

void run_everything_config_tests() {
    const std::string cfg = build_everything_lean_config();
    CHECK(cfg.starts_with("[Everything]\r\n"));
    CHECK(cfg.ends_with("\r\n"));

    // Verify all lines use CRLF and contain no duplicate INI keys.
    std::unordered_set<std::string> keys;
    std::size_t pos = 0;
    while (pos < cfg.size()) {
        const std::size_t eol = cfg.find("\r\n", pos);
        CHECK(eol != std::string_view::npos);
        const std::string_view line = std::string_view{cfg}.substr(pos, eol - pos);
        CHECK(!line.contains('\r'));
        CHECK(!line.contains('\n'));
        if (pos > 0) {
            const std::size_t eq = line.find('=');
            CHECK(eq != std::string_view::npos && eq > 0);
            const std::string key{line.substr(0, eq)};
            CHECK(keys.insert(key).second);
        }
        pos = eol + 2;
    }

    // IPC, DB persistence, and queried metadata must stay enabled.
    CHECK(has_line(cfg, "app_data=0"));
    CHECK(has_line(cfg, "allow_ipc=1"));
    CHECK(has_line(cfg, "allow_ipc_pipe=1"));
    CHECK(has_line(cfg, "allow_window_message_filter_ipc=1"));
    CHECK(has_line(cfg, "ipc=1"));
    CHECK(has_line(cfg, "ipc_pipe=1"));
    CHECK(has_line(cfg, "no_db=0"));
    CHECK(has_line(cfg, "db_load_crc=1"));
    CHECK(has_line(cfg, "db_auto_save_on_close=1"));
    CHECK(has_line(cfg, "db_save_on_exit=1"));
    CHECK(has_line(cfg, "index_size=1"));
    CHECK(has_line(cfg, "index_date_modified=1"));
    CHECK(has_line(cfg, "run_in_background=1"));
    CHECK(has_line(cfg, "monitor_pause=0"));
    CHECK(has_line(cfg, "monitor_stop=0"));
    CHECK(has_line(cfg, "monitor_notify_results_changed_delay=50"));
    CHECK(has_line(cfg, "monitor_thread_mode_background=0"));
    CHECK(has_line(cfg, "ntfs_update_thread_mode_background=0"));
    CHECK(has_line(cfg, "exclude_list_enabled=1"));
    CHECK(has_line(cfg, "exclude_hidden_files_and_folders=0"));

    // Index-level file filter must cover every media extension plus the USN sentinel extension.
    const std::string include_only = build_everything_include_only_files();
    CHECK(has_line(cfg, "include_only_files=" + include_only));
    std::unordered_set<std::string> patterns;
    for (std::size_t p = 0; p < include_only.size();) {
        const std::size_t sep = include_only.find(';', p);
        const std::string_view tok = sep == std::string_view::npos
                                         ? std::string_view{include_only}.substr(p)
                                         : std::string_view{include_only}.substr(p, sep - p);
        CHECK(tok.starts_with("*."));
        CHECK(patterns.insert(std::string{tok}).second);
        if (sep == std::string_view::npos) break;
        p = sep + 1;
    }
    const auto expect_ext = [&](std::wstring_view ext) {
        std::string pat = "*.";
        for (const wchar_t c : ext) pat.push_back(static_cast<char>(c));
        CHECK(patterns.contains(pat));
    };
    for (const auto ext : usyseek::media::IMAGE_EXTS) expect_ext(ext);
    for (const auto ext : usyseek::media::VIDEO_EXTS) expect_ext(ext);
    CHECK(patterns.contains("*." + std::string{USN_SENTINEL_EXT_ASCII}));
    CHECK(patterns.size() == usyseek::media::IMAGE_EXTS.size() + usyseek::media::VIDEO_EXTS.size() + 1);

    // Unneeded features for headless SDK3 queries should be disabled.
    CHECK(has_line(cfg, "show_tray_icon=0"));
    CHECK(has_line(cfg, "index_folder_size=0"));
    CHECK(has_line(cfg, "fast_size_sort=0"));
    CHECK(has_line(cfg, "fast_date_modified_sort=0"));
    CHECK(has_line(cfg, "journal=0"));
    CHECK(has_line(cfg, "db_backup=0"));
    CHECK(has_line(cfg, "ini_backup=0"));
    CHECK(has_line(cfg, "csv_backup=0"));
    CHECK(has_line(cfg, "search_history_enabled=0"));
    CHECK(has_line(cfg, "run_history_enabled=0"));
    CHECK(has_line(cfg, "plugins=0"));

    // CLI args formatting with and without -add-config.
    CHECK(build_everything_args(L"C:;D:", L"C:\\Windows;**\\node_modules", L"C:\\Program Files\\usyseek\\data\\Everything\\everything-lean.ini") ==
          L"-safe-mode -no-auto-index -volumes \"C:;D:\" -exclude-folders \"C:\\Windows;**\\node_modules\""
          L" -add-config \"C:\\Program Files\\usyseek\\data\\Everything\\everything-lean.ini\"");
    CHECK(build_everything_args(L"C:", L"") ==
          L"-safe-mode -no-auto-index -volumes \"C:\" -exclude-folders \"\"");

    // USN journal filesystem detection.
    CHECK(is_usn_journal_filesystem(L"NTFS"));
    CHECK(is_usn_journal_filesystem(L"ntfs"));
    CHECK(is_usn_journal_filesystem(L"ReFS"));
    CHECK(is_usn_journal_filesystem(L"refs"));
    CHECK(!is_usn_journal_filesystem(L"FAT32"));
    CHECK(!is_usn_journal_filesystem(L"exFAT"));
    CHECK(!is_usn_journal_filesystem(L""));

    // Sentinel naming and path joining.
    CHECK(make_usn_sentinel_name(1234, 56789) == L".usyseek-sync-1234-56789.usyseek-sync");
    CHECK(join_sentinel_path(L"D:\\", L".usyseek-sync-1-2.usyseek-sync") == L"D:\\.usyseek-sync-1-2.usyseek-sync");
    CHECK(join_sentinel_path(L"D:", L".usyseek-sync-1-2.usyseek-sync") == L"D:\\.usyseek-sync-1-2.usyseek-sync");
    CHECK(join_sentinel_path(L"C:\\Users\\usy\\AppData\\Roaming\\", L".usyseek-sync-1-2.usyseek-sync") ==
          L"C:\\Users\\usy\\AppData\\Roaming\\.usyseek-sync-1-2.usyseek-sync");
    CHECK(join_sentinel_path(L"C:\\Users\\usy\\AppData\\Roaming", L".usyseek-sync-1-2.usyseek-sync") ==
          L"C:\\Users\\usy\\AppData\\Roaming\\.usyseek-sync-1-2.usyseek-sync");
    CHECK(join_sentinel_path(L"", L".usyseek-sync-1-2.usyseek-sync").empty());
    CHECK(join_sentinel_path(L"C:\\", L"").empty());
}

} // namespace

int main() {
    try {
        run_everything_config_tests();
    } catch (...) {
        (void)std::fputs("everything_config_test: unexpected exception\n", stderr);
        return 1;
    }
    return usyseek::test::failures != 0;
}
