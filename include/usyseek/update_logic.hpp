#pragma once

#include <usylibpp/util/update_logic.hpp>

namespace usyseek {

using usylibpp::util::GithubAsset;
using usylibpp::util::GithubRelease;
using usylibpp::util::SemVer;
using usylibpp::util::UPDATE_CHECK_INTERVAL_S;
using usylibpp::util::UpdateState;
using usylibpp::util::extract_sha256_digest;
using usylibpp::util::is_newer_version;
using usylibpp::util::normalize_version_string;
using usylibpp::util::parse_semver;
using usylibpp::util::parse_tar_supports_7z;
using usylibpp::util::should_auto_check_updates;

/// Picks the best release archive matching the host's extraction capability (`.7z` if supported, else `.zip`).
[[nodiscard]] inline std::optional<GithubAsset> pick_release_asset(std::span<const GithubAsset> assets, bool prefer_7z) {
    return usylibpp::util::pick_release_asset(assets, prefer_7z, "usyseek.7z", "usyseek.zip");
}

} // namespace usyseek
