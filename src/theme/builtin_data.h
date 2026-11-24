// Internal: the built-in theme files, embedded by src/theme/module.cmake (builtin_data.cpp in
// the build tree) from src/theme/builtin/ and third_party/iterm2-color-schemes/ghostty/.
#pragma once

#include <cstddef>

namespace kt::theme::detail {

struct BuiltinFile {
    const char* filename;          // "Dracula", "Keepterm Dark.toml"
    const unsigned char* data;
    std::size_t size;
};

extern const BuiltinFile kBuiltinFiles[];
extern const std::size_t kBuiltinFileCount;

}  // namespace kt::theme::detail
