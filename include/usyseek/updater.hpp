#pragma once

#include "http.hpp"
#include "notifications.hpp"
#include "settings.hpp"
#include "update_logic.hpp"

#include <usylibpp/windows/updater.hpp>

#include <array>

namespace usyseek {

inline constexpr std::wstring_view GITHUB_LATEST_RELEASE_URL = L"https://api.github.com/repos/usyless/usyseek/releases/latest";
inline constexpr std::wstring_view GITHUB_RELEASES_PAGE_URL = L"https://github.com/usyless/usyseek/releases";

using usylibpp::windows::updater::sha256_file_hex;
using usylibpp::windows::updater::system_tar_path;
using usylibpp::windows::updater::tar_supports_7z;

inline UpdateState load_update_state() {
    return load_json_file<UpdateState>(update_state_path());
}

inline bool save_update_state(const UpdateState& st) {
    return save_json_file(update_state_path(), st);
}

inline const fs::path& update_work_dir() {
    static const fs::path p = data_dir() / L"update";
    return p;
}

inline const usylibpp::windows::updater::Config& updater_config() {
    static constexpr std::array<std::wstring_view, 2> bins = {L"usyseek.exe", L"usyseek-ui.exe"};
    static const usylibpp::windows::updater::Config cfg{
        .user_agent = APP_NAME,
        .latest_release_url = GITHUB_LATEST_RELEASE_URL,
        .releases_page_url = GITHUB_RELEASES_PAGE_URL,
        .current_version = APP_VERSION,
        .preferred_7z_asset = "usyseek.7z",
        .preferred_zip_asset = "usyseek.zip",
        .primary_exe = L"usyseek.exe",
        .required_bins = bins,
        .missing_bins_error = L"Archive does not contain usyseek.exe and usyseek-ui.exe.",
        .write_probe_prefix = L".usyseek-write-probe-",
        .work_dir = update_work_dir(),
    };
    return cfg;
}

[[nodiscard]] inline std::optional<fs::path> resolve_staging_root(const fs::path& staging_dir) {
    return usylibpp::windows::updater::resolve_staging_root(staging_dir, updater_config().required_bins);
}

inline bool is_dir_writable(const fs::path& dir) {
    return usylibpp::windows::updater::is_dir_writable(dir, updater_config().write_probe_prefix);
}

inline bool check_latest_release(UpdateState& st, bool& is_newer, const std::stop_token& stop = {}) {
    return usylibpp::windows::updater::check_latest_release(
        updater_config(), st, is_newer, [](const UpdateState& s) { (void)save_update_state(s); }, stop);
}

inline bool download_and_stage_update(const UpdateState& st, fs::path& out_staging_root, std::wstring& err,
                                      const std::function<void(std::wstring_view)>& status_cb,
                                      const http::Progress& progress, const std::stop_token& stop) {
    return usylibpp::windows::updater::download_and_stage_update(
        updater_config(), st, out_staging_root, err, status_cb, progress, stop);
}

inline bool launch_staged_updater(const fs::path& staging_root, const fs::path& target_dir, DWORD wait_pid,
                                  bool open_about_after, bool& uac_cancelled, std::wstring& err) {
    return usylibpp::windows::updater::launch_staged_updater(
        updater_config(), staging_root, target_dir, wait_pid, open_about_after, uac_cancelled, err);
}

} // namespace usyseek
