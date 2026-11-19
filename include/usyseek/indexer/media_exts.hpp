#pragma once

#include <array>
#include <string_view>

namespace usyseek::media {

inline constexpr std::array<std::wstring_view, 17> IMAGE_EXTS = {
    L"jpg", L"jpeg", L"jpe", L"jfif", L"png", L"bmp", L"gif", L"tif", L"tiff", L"webp", L"heic", L"heif", L"avif", L"jxl", L"dng",
    L"svg", L"svgz",
};

inline constexpr std::array<std::wstring_view, 12> VIDEO_EXTS = {
    L"mp4", L"m4v", L"mov", L"mkv", L"webm", L"avi", L"wmv", L"mpg", L"mpeg", L"ts", L"m2ts", L"3gp",
};

} // namespace usyseek::media
