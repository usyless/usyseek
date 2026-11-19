#pragma once

#include <usyseek/indexer/media_exts.hpp>
#include <usyseek/string_helpers.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace usyseek::indexer {

/// Extension used for temporary per-volume USN journal fence files (kept in `include_only_files`).
inline constexpr std::string_view USN_SENTINEL_EXT_ASCII = "usyseek-sync";
inline constexpr std::wstring_view USN_SENTINEL_EXT = L"usyseek-sync";

/// Static headless, IPC-only overrides applied via Everything 1.5 `-add-config`.
inline constexpr std::string_view EVERYTHING_LEAN_CONFIG =
    "[Everything]\r\n"
    "app_data=0\r\n"
    "allow_ipc=1\r\n"
    "allow_ipc_pipe=1\r\n"
    "allow_window_message_filter_ipc=1\r\n"
    "ipc=1\r\n"
    "ipc_pipe=1\r\n"
    "no_db=0\r\n"
    "db_load_crc=1\r\n"
    "db_auto_save_on_close=1\r\n"
    "db_save_on_exit=1\r\n"
    "index_size=1\r\n"
    "index_date_modified=1\r\n"
    "run_in_background=1\r\n"
    "monitor_pause=0\r\n"
    "monitor_stop=0\r\n"
    "monitor_notify_results_changed_delay=50\r\n"
    "monitor_thread_mode_background=0\r\n"
    "ntfs_update_thread_mode_background=0\r\n"
    "exclude_list_enabled=1\r\n"
    "exclude_hidden_files_and_folders=0\r\n"
    "allow_plugins=0\r\n"
    "plugins=0\r\n"
    "show_tray_icon=0\r\n"
    "show_in_taskbar=0\r\n"
    "include_selected_folder_size_in_statusbar=0\r\n"
    "index_folder_size=0\r\n"
    "fast_size_sort=0\r\n"
    "fast_date_modified_sort=0\r\n"
    "journal=0\r\n"
    "db_backup=0\r\n"
    "ini_backup=0\r\n"
    "csv_backup=0\r\n"
    "search_history_enabled=0\r\n"
    "run_history_enabled=0\r\n"
    "undo_history=0\r\n"
    "nav_history=0\r\n"
    "session_store=0\r\n"
    "session_store_restore_last_closed_window=0\r\n"
    "file_list_history_enabled=0\r\n"
    "column_history_enabled=0\r\n"
    "statistics=0\r\n"
    "icons=0\r\n"
    "listview_icons=0\r\n"
    "icon_shell_extensions=0\r\n"
    "thumbnails=0\r\n"
    "thumbnail_cache_enabled=0\r\n"
    "shell_item_image_factory=0\r\n"
    "extract_image=0\r\n"
    "context_menu_shell_extensions=0\r\n"
    "property_system=0\r\n"
    "property_builtin_handler=0\r\n"
    "summary_information=0\r\n"
    "metadata_efu=0\r\n"
    "property_cache_include_folders=0\r\n"
    "property_cache_include_files=0\r\n"
    "content_indexing_enabled=0\r\n"
    "content_builtin_text_plain_handler=0\r\n"
    "content_builtin_eml_handler=0\r\n"
    "content_ifilter=0\r\n"
    "filelist_fast_size_sort=0\r\n"
    "filelist_fast_date_modified_sort=0\r\n"
    "filelist_index_folder_size=0\r\n"
    "filelist_preview=0\r\n"
    "filelist_context_menu=0\r\n"
    "add_clipboard_format_listener=0\r\n"
    "add_to_recent_docs=0\r\n"
    "regenerate_user_environment=0\r\n"
    "notify_win_event=0\r\n";

/// Builds the static `include_only_files` pattern list covering all supported image/video extensions
/// plus the USN fence sentinel extension. Always includes both images and videos so toggling settings
/// does not invalidate Everything's persisted `.db`.
[[nodiscard]] inline std::string build_everything_include_only_files() {
    std::size_t total = 0;
    const auto measure = [&](auto&& list) noexcept {
        for (const std::wstring_view ext : list) {
            if (total > 0) ++total; // ';'
            total += 2 + ext.size(); // "*." + ext
        }
    };
    measure(media::IMAGE_EXTS);
    measure(media::VIDEO_EXTS);
    if (total > 0) ++total;
    total += 2 + USN_SENTINEL_EXT_ASCII.size();

    std::string out;
    out.resize_and_overwrite(total, [&](char* dst, std::size_t) noexcept {
        char* cur = dst;
        bool first = true;
        const auto emit_wide = [&](auto&& list) noexcept {
            for (const std::wstring_view ext : list) {
                if (!first) *cur++ = ';';
                first = false;
                *cur++ = '*';
                *cur++ = '.';
                for (const wchar_t c : ext) *cur++ = static_cast<char>(c);
            }
        };
        emit_wide(media::IMAGE_EXTS);
        emit_wide(media::VIDEO_EXTS);
        if (!first) *cur++ = ';';
        *cur++ = '*';
        *cur++ = '.';
        std::char_traits<char>::copy(cur, USN_SENTINEL_EXT_ASCII.data(), USN_SENTINEL_EXT_ASCII.size());
        cur += USN_SENTINEL_EXT_ASCII.size();
        return static_cast<std::size_t>(cur - dst);
    });
    return out;
}

/// Builds the complete Everything 1.5 `-add-config` INI contents including `include_only_files`.
[[nodiscard]] inline std::string build_everything_lean_config() {
    const std::string include_only = build_everything_include_only_files();
    std::string out;
    append_to(out, EVERYTHING_LEAN_CONFIG, "include_only_files=", include_only, "\r\n");
    return out;
}

/// Builds the Everything 1.5 extra launch arguments appended by `ulp::Everything3::try_load`.
[[nodiscard]] inline std::wstring build_everything_args(
    std::wstring_view volumes,
    std::wstring_view exclude_folders,
    std::wstring_view add_config_path = {}
) {
    std::wstring args;
    if (!add_config_path.empty()) {
        append_to(
            args,
            L"-safe-mode -no-auto-index -volumes \"", volumes,
            L"\" -exclude-folders \"", exclude_folders,
            L"\" -add-config \"", add_config_path, L"\""
        );
    } else {
        append_to(
            args,
            L"-safe-mode -no-auto-index -volumes \"", volumes,
            L"\" -exclude-folders \"", exclude_folders, L"\""
        );
    }
    return args;
}

/// Returns true if the filesystem name supports a Windows USN Change Journal monitored by Everything.
[[nodiscard]] constexpr bool is_usn_journal_filesystem(std::wstring_view fs_name) noexcept {
    constexpr auto ascii_ieq = [](std::wstring_view a, std::wstring_view b) constexpr noexcept {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const wchar_t ca = (a[i] >= L'A' && a[i] <= L'Z') ? static_cast<wchar_t>(a[i] + (L'a' - L'A')) : a[i];
            const wchar_t cb = (b[i] >= L'A' && b[i] <= L'Z') ? static_cast<wchar_t>(b[i] + (L'a' - L'A')) : b[i];
            if (ca != cb) return false;
        }
        return true;
    };
    return ascii_ieq(fs_name, L"NTFS") || ascii_ieq(fs_name, L"ReFS");
}

/// Builds a unique non-media temporary sentinel filename used to fence Everything's USN journal catch-up.
[[nodiscard]] inline FixedWString<64> make_usn_sentinel_name(std::uint32_t pid, std::uint64_t nonce) noexcept {
    FixedWString<64> name{L".usyseek-sync-"};
    name.append(to_fixed_wstr(pid));
    name.push_back(L'-');
    name.append(to_fixed_wstr(nonce));
    name.push_back(L'.');
    name.append(USN_SENTINEL_EXT);
    return name;
}

/// Joins a directory path and sentinel filename with a single backslash (safe for both `D:\` and `C:\dir`).
[[nodiscard]] inline std::wstring join_sentinel_path(std::wstring_view dir, std::wstring_view filename) {
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) {
        dir.remove_suffix(1);
    }
    if (dir.empty() || filename.empty()) return {};
    std::wstring out;
    append_to(out, dir, L"\\", filename);
    return out;
}

} // namespace usyseek::indexer
