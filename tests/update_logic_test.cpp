#include "check.hpp"
#include "usyseek/update_logic.hpp"

int main() {
    using namespace usyseek;

    // 1. Semver parsing & comparison
    CHECK(is_newer_version("v0.3.0", "0.2.0"));
    CHECK(is_newer_version("0.3.0", "v0.2.0"));
    CHECK(is_newer_version("v0.10.0", "0.9.9"));
    CHECK(is_newer_version("1.0.0", "0.99.99"));
    CHECK(!is_newer_version("v0.2.0", "0.2.0"));
    CHECK(!is_newer_version("v0.1.9", "0.2.0"));
    CHECK(!is_newer_version("v0.2.0-rc1", "0.2.0"));
    CHECK(is_newer_version("v0.2.0", "0.2.0-rc1"));
    CHECK(!is_newer_version("not-a-version", "0.2.0"));
    CHECK(normalize_version_string(" v0.3.0 ") == "0.3.0");

    // 2. Hardcoded 24h auto-check cooldown
    constexpr int64_t day = 24LL * 60 * 60;
    CHECK(day == UPDATE_CHECK_INTERVAL_S);
    CHECK(!should_auto_check_updates(false, 0, 1000));
    CHECK(should_auto_check_updates(true, 0, 1000));
    CHECK(!should_auto_check_updates(true, 1000, 1000 + day - 1));
    CHECK(should_auto_check_updates(true, 1000, 1000 + day));
    CHECK(should_auto_check_updates(true, 1000, 1000 + day + 500));

    // 3. Windows tar.exe .7z compatibility detection
    CHECK(!parse_tar_supports_7z("bsdtar 3.5.2 - libarchive 3.5.2 zlib/1.2.5.f-ipp"));
    CHECK(parse_tar_supports_7z("bsdtar 3.8.9 - libarchive 3.8.9 zlib/1.2.13.1-motley liblzma/5.8.3 bz2lib/1.0.8 libzstd/1.5.7 cng/2.0 libb2/bundled"));
    CHECK(!parse_tar_supports_7z("bsdtar 3.6.0 - libarchive 3.6.0 zlib/1.2.13"));

    // 4. Asset selection & SHA-256 digest extraction
    const std::vector<GithubAsset> both{
        {.name = "usyseek.zip", .browser_download_url = "https://example.com/usyseek.zip", .size = 35'000'000, .digest = "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
        {.name = "usyseek.7z", .browser_download_url = "https://example.com/usyseek.7z", .size = 22'000'000, .digest = "sha256:ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789"},
    };
    const auto a7z = pick_release_asset(both, true);
    CHECK(a7z.has_value());
    if (a7z) {
        CHECK(a7z->name == "usyseek.7z");
        CHECK(extract_sha256_digest(a7z->digest) == "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
    }

    const auto azip = pick_release_asset(both, false);
    CHECK(azip.has_value());
    if (azip) {
        CHECK(azip->name == "usyseek.zip");
    }

    const std::vector<GithubAsset> only_7z{both[1]};
    CHECK(!pick_release_asset(only_7z, false).has_value());
    CHECK(pick_release_asset(only_7z, true).has_value());
    CHECK(extract_sha256_digest("sha256:short").empty());

    return 0;
}
