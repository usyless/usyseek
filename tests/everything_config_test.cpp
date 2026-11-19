#include "check.hpp"
#include "usyseek/indexer/everything_config.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_set>

using usyseek::indexer::EVERYTHING_LEAN_CONFIG;
using usyseek::indexer::build_everything_args;
using usyseek::indexer::is_usn_journal_filesystem;
using usyseek::indexer::join_sentinel_path;
using usyseek::indexer::make_usn_sentinel_name;

namespace {

bool has_line(std::string_view text, std::string_view line) {
    return text.contains("\r\n" + std::string{line} + "\r\n");
}

void run_everything_config_tests() {
    CHECK(EVERYTHING_LEAN_CONFIG.starts_with("[Everything]\r\n"));
    CHECK(EVERYTHING_LEAN_CONFIG.ends_with("\r\n"));

    // Verify all lines use CRLF and contain no duplicate INI keys.
    std::unordered_set<std::string> keys;
    std::size_t pos = 0;
    while (pos < EVERYTHING_LEAN_CONFIG.size()) {
        const std::size_t eol = EVERYTHING_LEAN_CONFIG.find("\r\n", pos);
        CHECK(eol != std::string_view::npos);
        const std::string_view line = EVERYTHING_LEAN_CONFIG.substr(pos, eol - pos);
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
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "app_data=0"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "allow_ipc=1"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "allow_ipc_pipe=1"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "allow_window_message_filter_ipc=1"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "ipc=1"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "ipc_pipe=1"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "no_db=0"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "db_load_crc=1"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "db_auto_save_on_close=1"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "db_save_on_exit=1"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "index_size=1"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "index_date_modified=1"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "run_in_background=1"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "monitor_pause=0"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "monitor_stop=0"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "monitor_notify_results_changed_delay=50"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "monitor_thread_mode_background=0"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "ntfs_update_thread_mode_background=0"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "exclude_hidden_files_and_folders=0"));

    // Unneeded features for headless SDK3 queries should be disabled.
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "show_tray_icon=0"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "index_folder_size=0"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "fast_size_sort=0"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "fast_date_modified_sort=0"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "journal=0"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "db_backup=0"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "ini_backup=0"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "csv_backup=0"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "search_history_enabled=0"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "run_history_enabled=0"));
    CHECK(has_line(EVERYTHING_LEAN_CONFIG, "plugins=0"));

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
    CHECK(make_usn_sentinel_name(1234, 56789) == L".usyseek-sync-1234-56789.tmp");
    CHECK(join_sentinel_path(L"D:\\", L".usyseek-sync-1-2.tmp") == L"D:\\.usyseek-sync-1-2.tmp");
    CHECK(join_sentinel_path(L"D:", L".usyseek-sync-1-2.tmp") == L"D:\\.usyseek-sync-1-2.tmp");
    CHECK(join_sentinel_path(L"C:\\Users\\usy\\AppData\\Roaming\\", L".usyseek-sync-1-2.tmp") ==
          L"C:\\Users\\usy\\AppData\\Roaming\\.usyseek-sync-1-2.tmp");
    CHECK(join_sentinel_path(L"C:\\Users\\usy\\AppData\\Roaming", L".usyseek-sync-1-2.tmp") ==
          L"C:\\Users\\usy\\AppData\\Roaming\\.usyseek-sync-1-2.tmp");
    CHECK(join_sentinel_path(L"", L".usyseek-sync-1-2.tmp").empty());
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
