#include "check.hpp"
#include "usyseek/indexer/everything_config.hpp"

#include <string>
#include <string_view>
#include <unordered_set>

using usyseek::indexer::EVERYTHING_LEAN_CONFIG;
using usyseek::indexer::build_everything_args;

namespace {

bool has_line(std::string_view text, std::string_view line) {
    return text.find("\r\n" + std::string{line} + "\r\n") != std::string_view::npos;
}

} // namespace

int main() {
    CHECK(EVERYTHING_LEAN_CONFIG.starts_with("[Everything]\r\n"));
    CHECK(EVERYTHING_LEAN_CONFIG.ends_with("\r\n"));

    // Verify all lines use CRLF and contain no duplicate INI keys.
    std::unordered_set<std::string> keys;
    std::size_t pos = 0;
    while (pos < EVERYTHING_LEAN_CONFIG.size()) {
        const std::size_t eol = EVERYTHING_LEAN_CONFIG.find("\r\n", pos);
        CHECK(eol != std::string_view::npos);
        const std::string_view line = EVERYTHING_LEAN_CONFIG.substr(pos, eol - pos);
        CHECK(line.find('\r') == std::string_view::npos);
        CHECK(line.find('\n') == std::string_view::npos);
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

    return usyseek::test::failures != 0;
}
