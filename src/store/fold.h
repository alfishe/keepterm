// Case folding for the search index: per scalar, Unicode simple folding
// (CaseFolding.txt C + S), no normalization. Invalid UTF-8 bytes pass through unchanged.
#pragma once

#include <string>
#include <string_view>

namespace kt::store {

char32_t fold_cp(char32_t cp);
// Appends the folded form of `in` to `out`.
void fold_utf8(std::string_view in, std::string& out);
inline std::string folded(std::string_view in) {
    std::string out;
    fold_utf8(in, out);
    return out;
}
// Appends the UTF-8 form of `cp` (no checks beyond the range).
void append_utf8(std::string& out, char32_t cp);

}  // namespace kt::store
