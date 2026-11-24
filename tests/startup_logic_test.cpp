#include "check.hpp"
#include "usyseek/startup_logic.hpp"

#include <array>
#include <cstdio>

namespace {

void run_startup_logic_tests() {
    using namespace usyseek;

    // 1. StartupApproved\Run binary flag evaluation
    {
        CHECK(is_startup_approved_bytes({}));
        const std::array<uint8_t, 12> enabled_default{0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        const std::array<uint8_t, 12> enabled_alt{0x06, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        const std::array<uint8_t, 12> enabled_zero{0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        CHECK(is_startup_approved_bytes(enabled_default));
        CHECK(is_startup_approved_bytes(enabled_alt));
        CHECK(is_startup_approved_bytes(enabled_zero));

        const std::array<uint8_t, 12> disabled_user{0x03, 0, 0, 0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0x01};
        const std::array<uint8_t, 12> disabled_one{0x01, 0, 0, 0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0x01};
        const std::array<uint8_t, 12> disabled_seven{0x07, 0, 0, 0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0x01};
        CHECK(!is_startup_approved_bytes(disabled_user));
        CHECK(!is_startup_approved_bytes(disabled_one));
        CHECK(!is_startup_approved_bytes(disabled_seven));
    }

    // 2. CurrentVersion\Run command executable extraction
    {
        const auto quoted = extract_startup_command_exe(L"\"C:\\Program Files\\usyseek\\usyseek.exe\"");
        CHECK(quoted.primary == L"C:\\Program Files\\usyseek\\usyseek.exe");
        CHECK(quoted.fallback.empty());

        const auto quoted_args = extract_startup_command_exe(L"  \"C:\\Program Files\\usyseek\\usyseek.exe\" --minimized  ");
        CHECK(quoted_args.primary == L"C:\\Program Files\\usyseek\\usyseek.exe");
        CHECK(quoted_args.fallback.empty());

        const auto unquoted = extract_startup_command_exe(L"C:\\Apps\\usyseek.exe");
        CHECK(unquoted.primary == L"C:\\Apps\\usyseek.exe");
        CHECK(unquoted.fallback.empty());

        const auto unquoted_with_args = extract_startup_command_exe(L"C:\\Apps\\usyseek.exe --minimized");
        CHECK(unquoted_with_args.primary == L"C:\\Apps\\usyseek.exe --minimized");
        CHECK(unquoted_with_args.fallback == L"C:\\Apps\\usyseek.exe");

        const auto unquoted_spaces = extract_startup_command_exe(L"C:\\Program Files\\usyseek\\usyseek.exe");
        CHECK(unquoted_spaces.primary == L"C:\\Program Files\\usyseek\\usyseek.exe");
        CHECK(unquoted_spaces.fallback == L"C:\\Program");

        // Embedded NULs from RegQueryValueExW byte buffers
        const std::wstring_view with_nul{L"\"C:\\Apps\\usyseek.exe\"\0trailing", 31};
        const auto nul_res = extract_startup_command_exe(with_nul);
        CHECK(nul_res.primary == L"C:\\Apps\\usyseek.exe");
        CHECK(nul_res.fallback.empty());

        // Invalid / empty inputs
        CHECK(extract_startup_command_exe(L"").primary.empty());
        CHECK(extract_startup_command_exe(L"   \t\r\n ").primary.empty());
        CHECK(extract_startup_command_exe(L"\"\"").primary.empty());
        CHECK(extract_startup_command_exe(L"\"C:\\Apps\\usyseek.exe").primary.empty());
    }
}

} // namespace

int main() {
    try {
        run_startup_logic_tests();
    } catch (...) {
        (void)std::fputs("startup_logic_test: unexpected exception\n", stderr);
        return 1;
    }
    return usyseek::test::failures != 0;
}
