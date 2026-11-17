#pragma once

#include "http.hpp"
#include "notifications.hpp"
#include "settings.hpp"
#include "update_logic.hpp"

#include <bcrypt.h>
#include <shellapi.h>
#include <wil/resource.h>

#include <array>
#include <functional>
#include <stop_token>

namespace usyseek {

inline constexpr std::wstring_view GITHUB_LATEST_RELEASE_URL = L"https://api.github.com/repos/usyless/usyseek/releases/latest";
inline constexpr std::wstring_view GITHUB_RELEASES_PAGE_URL = L"https://github.com/usyless/usyseek/releases";

inline const fs::path& system_tar_path() {
    static const fs::path p = [] {
        std::array<wchar_t, MAX_PATH> sys_dir; // NOLINT(cppcoreguidelines-pro-type-member-init)
        const UINT n = GetSystemDirectoryW(sys_dir.data(), static_cast<UINT>(sys_dir.size()));
        return fs::path{std::wstring_view{sys_dir.data(), n < sys_dir.size() ? n : 0}} / L"tar.exe";
    }();
    return p;
}

/// True when the system `tar.exe` (`bsdtar` / `libarchive`) supports extracting `.7z` archives (Windows 11 23H2+).
inline bool tar_supports_7z() {
    static const bool supported = [] {
        const fs::path& tar = system_tar_path();
        std::error_code ec;
        if (!fs::exists(tar, ec)) return false;
        const std::wstring cmd = L"\"" + tar.wstring() + L"\" --version";
        std::string output;
        const auto res = ulp::win::proc::run_process<{.allow_visible_windows = false}>(
            ulp::win::proc::process_settings{
                .commandline = cmd,
                .on_stdout_line = [&](std::string_view line) {
                    output.append(line);
                    output.push_back('\n');
                },
            });
        if (res.status != 0) return false;
        return parse_tar_supports_7z(output);
    }();
    return supported;
}

inline UpdateState load_update_state() {
    return load_json_file<UpdateState>(update_state_path());
}

inline bool save_update_state(const UpdateState& st) {
    return save_json_file(update_state_path(), st);
}

/// Checks GitHub's latest release endpoint, always recording `last_check_unix` regardless of success or failure.
inline bool check_latest_release(UpdateState& st, bool& is_newer) {
    is_newer = false;
    st.last_check_unix = unix_now();

    const auto body = http::get(std::wstring{GITHUB_LATEST_RELEASE_URL});
    if (!body) {
        st.last_error = "Could not reach GitHub releases (network error or API rate limit).";
        save_update_state(st);
        return false;
    }

    GithubRelease rel;
    if (glz::read<JSON_READ>(rel, *body) || rel.tag_name.empty()) {
        st.last_error = "Unexpected response from GitHub releases API.";
        save_update_state(st);
        return false;
    }

    st.latest_tag = rel.tag_name;
    st.latest_version = normalize_version_string(rel.tag_name);
    st.release_url = rel.html_url.empty() ? to_utf8(GITHUB_RELEASES_PAGE_URL) : rel.html_url;
    st.release_notes = rel.body;
    st.last_error.clear();

    const std::string current = to_utf8(APP_VERSION);
    is_newer = is_newer_version(st.latest_version, current);

    if (const auto asset = pick_release_asset(rel.assets, tar_supports_7z())) {
        st.asset_name = asset->name;
        st.asset_url = asset->browser_download_url;
        st.asset_size = asset->size;
        st.asset_sha256 = extract_sha256_digest(asset->digest);
    } else {
        st.asset_name.clear();
        st.asset_url.clear();
        st.asset_size = 0;
        st.asset_sha256.clear();
        if (is_newer) {
            st.last_error = tar_supports_7z()
                ? "Release has no compatible .7z or .zip asset."
                : "Release has no compatible .zip asset for this Windows version.";
        }
    }

    save_update_state(st);
    return true;
}

inline std::string sha256_file_hex(const fs::path& path, std::wstring& err) {
    const wil::unique_hfile file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                             FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (!file) {
        err = L"Could not open downloaded archive for verification.";
        return {};
    }

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        err = L"Could not initialize SHA-256 provider.";
        return {};
    }
    const auto close_alg = wil::scope_exit([&] { BCryptCloseAlgorithmProvider(alg, 0); });

    DWORD hash_obj_len = 0;
    DWORD cb = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hash_obj_len), sizeof(hash_obj_len), &cb, 0))) {
        err = L"Could not query SHA-256 object size.";
        return {};
    }

    auto hash_obj = std::make_unique_for_overwrite<uint8_t[]>(hash_obj_len); // NOLINT(modernize-avoid-c-arrays)
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, hash_obj.get(), hash_obj_len, nullptr, 0, 0))) {
        err = L"Could not create SHA-256 hash.";
        return {};
    }
    const auto close_hash = wil::scope_exit([&] { BCryptDestroyHash(hash); });

    constexpr DWORD BUF_CAP = 65536;
    auto buf = std::make_unique_for_overwrite<uint8_t[]>(BUF_CAP); // NOLINT(modernize-avoid-c-arrays)
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file.get(), buf.get(), BUF_CAP, &got, nullptr)) {
            err = L"Failed while reading archive for checksum verification.";
            return {};
        }
        if (got == 0) break;
        if (!BCRYPT_SUCCESS(BCryptHashData(hash, buf.get(), got, 0))) {
            err = L"SHA-256 hashing failed.";
            return {};
        }
    }

    std::array<uint8_t, 32> digest; // NOLINT(cppcoreguidelines-pro-type-member-init)
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
        err = L"Could not finalize SHA-256 hash.";
        return {};
    }

    static constexpr std::string_view hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const uint8_t b : digest) {
        out.push_back(hex[(static_cast<unsigned>(b) >> 4U) & 0x0FU]);
        out.push_back(hex[static_cast<unsigned>(b) & 0x0FU]);
    }
    return out;
}

/// Finds the folder containing `usyseek.exe` and `usyseek-ui.exe` inside an extracted archive directory.
[[nodiscard]] inline std::optional<fs::path> resolve_staging_root(const fs::path& staging_dir) {
    std::error_code ec;
    const auto has_bins = [&](const fs::path& dir) {
        return fs::is_regular_file(dir / L"usyseek.exe", ec) &&
               fs::is_regular_file(dir / L"usyseek-ui.exe", ec);
    };
    if (has_bins(staging_dir)) return staging_dir;

    std::optional<fs::path> match;
    for (const auto& entry : fs::directory_iterator(staging_dir, ec)) {
        if (!entry.is_directory(ec)) continue;
        if (has_bins(entry.path())) {
            if (match) return std::nullopt; // ambiguous
            match = entry.path();
        }
    }
    return match;
}

inline bool is_dir_writable(const fs::path& dir) {
    const fs::path probe = dir / (L".usyseek-write-probe-" + std::to_wstring(GetCurrentProcessId()));
    const wil::unique_hfile f{CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                          FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr)};
    if (f) return true;
    std::error_code ec;
    fs::remove(probe, ec);
    return false;
}

inline const fs::path& update_work_dir() {
    static const fs::path p = data_dir() / L"update";
    return p;
}

inline bool download_and_stage_update(const UpdateState& st, fs::path& out_staging_root, std::wstring& err,
                                      const std::function<void(std::wstring_view)>& status_cb,
                                      const http::Progress& progress, const std::stop_token& stop) {
    if (st.asset_url.empty() || st.asset_name.empty()) {
        err = L"No compatible release archive URL available.";
        return false;
    }

    const fs::path& work_dir = update_work_dir();
    const fs::path archive = work_dir / to_wide(st.asset_name);
    const fs::path staging = work_dir / L"staging";
    std::error_code ec;
    fs::create_directories(work_dir, ec);
    if (ec) {
        err = L"Could not create update folder: " + to_wide(ec.message());
        return false;
    }

    if (status_cb) status_cb(L"Downloading " + to_wide(st.asset_name) + L"…");
    const http::Result dl = http::download(to_wide(st.asset_url), archive, progress, stop);
    if (!dl.ok) {
        err = dl.cancelled ? L"Download cancelled — it can resume next time." : (L"Download failed: " + dl.error);
        return false;
    }

    if (!st.asset_sha256.empty()) {
        if (status_cb) status_cb(L"Verifying checksum…");
        std::wstring hash_err;
        const std::string got = sha256_file_hex(archive, hash_err);
        if (got.empty()) {
            err = hash_err;
            return false;
        }
        if (got != st.asset_sha256) {
            fs::remove(archive, ec);
            err = L"Downloaded file checksum mismatch (expected " + to_wide(st.asset_sha256) + L").";
            return false;
        }
    }

    if (status_cb) status_cb(L"Extracting update…");
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) {
        err = L"Could not prepare staging directory: " + to_wide(ec.message());
        return false;
    }

    const fs::path& tar = system_tar_path();
    const std::wstring tar_cmd = L"\"" + tar.wstring() + L"\" -xf \"" + archive.wstring() + L"\" -C \"" + staging.wstring() + L"\"";
    std::string tar_err;
    const auto res = ulp::win::proc::run_process<{.allow_visible_windows = false}>(
        ulp::win::proc::process_settings{
            .commandline = tar_cmd,
            .on_stderr_line = [&](std::string_view line) {
                if (!tar_err.empty()) tar_err += '\n';
                tar_err.append(line);
            },
        });
    if (res.status != 0) {
        fs::remove_all(staging, ec);
        err = L"Extraction failed (tar exited with code " + std::to_wstring(res.status) + L")" +
              (tar_err.empty() ? L"." : (L": " + to_wide(tar_err)));
        return false;
    }

    const auto root = resolve_staging_root(staging);
    if (!root) {
        fs::remove_all(staging, ec);
        err = L"Archive does not contain usyseek.exe and usyseek-ui.exe.";
        return false;
    }
    out_staging_root = *root;
    return true;
}

inline bool launch_staged_updater(const fs::path& staging_root, const fs::path& target_dir, DWORD wait_pid,
                                  bool open_about_after, bool& uac_cancelled, std::wstring& err) {
    uac_cancelled = false;
    const fs::path staged_exe = staging_root / L"usyseek.exe";
    std::wstring args = L"--apply-update " + std::to_wstring(wait_pid) +
                        L" \"" + staging_root.wstring() + L"\" \"" + target_dir.wstring() + L"\"";
    if (open_about_after) args += L" --open-about";

    if (is_dir_writable(target_dir)) {
        std::wstring cmd = L"\"" + staged_exe.wstring() + L"\" " + args;
        STARTUPINFOW si{.cb = sizeof(STARTUPINFOW)};
        wil::unique_process_information pi;
        if (CreateProcessW(staged_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                           CREATE_UNICODE_ENVIRONMENT | CREATE_BREAKAWAY_FROM_JOB, nullptr,
                           staging_root.c_str(), &si, &pi)) {
            return true;
        }
        const DWORD e = GetLastError();
        BOOL in_job = FALSE;
        // If our parent job forbids breakaway (e.g. an older launcher), fall through to ShellExecuteEx("runas") to escape the job.
        if (e != ERROR_ACCESS_DENIED || !IsProcessInJob(GetCurrentProcess(), nullptr, &in_job) || !in_job) {
            err = to_wide(win32_error_utf8(e));
            return false;
        }
    }

    SHELLEXECUTEINFOW sei{
        .cbSize = sizeof(SHELLEXECUTEINFOW),
        .fMask = SEE_MASK_NOASYNC,
        .lpVerb = L"runas",
        .lpFile = staged_exe.c_str(),
        .lpParameters = args.c_str(),
        .lpDirectory = staging_root.c_str(),
        .nShow = SW_SHOWNORMAL,
    };
    if (!ShellExecuteExW(&sei)) {
        const DWORD e = GetLastError();
        if (e == ERROR_CANCELLED) {
            uac_cancelled = true;
            err = L"Administrator permission (UAC) was cancelled.";
        } else {
            err = to_wide(win32_error_utf8(e));
        }
        return false;
    }
    return true;
}

} // namespace usyseek
