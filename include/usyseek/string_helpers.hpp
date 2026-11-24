#pragma once

#include <usylibpp/strings.hpp>

namespace usyseek {

namespace detail {
using usylibpp::strings::detail::NativePathLike;
using usylibpp::strings::detail::StringPart;
using usylibpp::strings::detail::SupportedChar;
} // namespace detail

using usylibpp::strings::FixedBasicString;
using usylibpp::strings::FixedString;
using usylibpp::strings::FixedWString;

using usylibpp::strings::append_to;
using usylibpp::strings::concat_to;
using usylibpp::strings::concat_utf8;
using usylibpp::strings::concat_wide;
using usylibpp::strings::overwrite_to;
using usylibpp::strings::to_fixed_str;
using usylibpp::strings::to_fixed_string;
using usylibpp::strings::to_fixed_wstr;
using usylibpp::strings::to_utf8;
using usylibpp::strings::to_wide;

} // namespace usyseek
