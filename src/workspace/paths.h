// Path helpers of the workspace. Core code, pure string work.
#pragma once

#include <string>
#include <string_view>

namespace kt::ws {

enum class PathStyle : unsigned char { Posix, Windows };
#ifdef _WIN32
inline constexpr PathStyle kHostPathStyle = PathStyle::Windows;
#else
inline constexpr PathStyle kHostPathStyle = PathStyle::Posix;
#endif

// '\' -> '/' (Windows style only; on POSIX a backslash is a file name character).
std::string to_slashes(std::string_view path, PathStyle style = kHostPathStyle);

// The comparison form: '/' separators, '.' and '..' removed, repeated and trailing separators
// removed, on Windows the drive letter uppercased and the whole path lowercased (the comparison is
// case-insensitive). Symlinks are not resolved. Relative paths stay relative.
std::string canonical_path(std::string_view path, PathStyle style = kHostPathStyle);

bool same_folder(std::string_view a, std::string_view b, PathStyle style = kHostPathStyle);

// The last component ("api" for "C:/work/api/"), for project names.
std::string folder_name(std::string_view path);

// UTF-8 aware trim of ASCII whitespace.
std::string trim(std::string_view s);

}  // namespace kt::ws
