// A skipping JSON scanner for the context probes.
//
// It walks one JSON document and reports only the scalars at the key paths the caller asks for;
// every other value (message text, tool output, arrays) is stepped over without being decoded or
// copied. Strings are delivered as views of their raw (still escaped) bytes. Strict enough to
// reject torn lines, bounded (depth 32), never throws. Core code.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kt::agents::jscan {

struct Scalar {
    enum class Kind : std::uint8_t { Null, Bool, Number, String };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string_view raw;   // String: the bytes between the quotes, escapes not resolved
    std::size_t begin = 0;  // byte offsets of the value in the document
    std::size_t end = 0;

    // A non-negative integer (fraction dropped); nullopt for other kinds or negative values.
    std::optional<std::uint64_t> u64() const;
    // The string with escapes resolved (\uXXXX to UTF-8); empty for other kinds.
    std::string text() const;
};

// Paths are member names joined by '.', for example "message.usage.input_tokens". Array
// elements are not reported. `wanted` lists the full paths of interest; values whose path is not
// a prefix of a wanted path are skipped unread. Returns false when the document is not valid JSON
// (a torn line); `on` may have been called for values before the error.
bool scan(std::string_view doc, std::span<const std::string_view> wanted,
          const std::function<void(std::string_view path, const Scalar&)>& on);

// Byte ranges of the members of the top-level object, in file order. The settings.json editor
// uses it to replace or insert one member and keep every other byte as it was.
struct Member {
    std::string_view key;      // raw key bytes
    std::size_t key_begin = 0; // offset of the opening quote of the key
    std::size_t value_begin = 0;
    std::size_t value_end = 0;  // one past the value
};
struct TopLevel {
    std::size_t open = 0;   // offset of '{'
    std::size_t close = 0;  // offset of '}'
    std::vector<Member> members;
};
std::optional<TopLevel> top_level(std::string_view doc);

}  // namespace kt::agents::jscan
