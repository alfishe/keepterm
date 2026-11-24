// A small YAML reader for the project files. Core code, no OS calls.
//
// It reads the subset that project files use, written by hand or by Macterm and keepterm:
// block mappings and sequences (also a sequence at its key's indent, "compact" style), flow
// mappings and sequences ({ run: "x" }, [a, b], also over several lines), plain, single- and
// double-quoted scalars (all escapes), literal and folded block scalars (| > with - + and an
// indent digit), comments, one optional "---" document start. Not supported, each reported as
// an error with its line and column: anchors and aliases (& *), tags (!), complex keys (?),
// several documents, tabs in indentation. Duplicate keys are an error.
//
// Scalars keep their text; typing (null, numbers) is left to the reader of the tree, so "1.10"
// stays "1.10" and "no" stays "no".
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/expected.h"

namespace kt::ws::yaml {

struct Node {
    enum class Kind : std::uint8_t { Null, Scalar, Map, Seq };
    Kind kind = Kind::Null;
    std::string text;                                  // Scalar
    bool quoted = false;                               // Scalar: quoted or block form
    std::vector<std::pair<std::string, Node>> map;     // Map, in file order
    std::vector<Node> seq;                             // Seq
    int line = 0, col = 0;                             // 1-based start

    // Declared here, defaulted in the .cpp: inline special members would instantiate the
    // pair<string, Node> of the member list while Node is incomplete (clang with libstdc++ 15).
    Node();
    Node(const Node&);
    Node(Node&&) noexcept;
    Node& operator=(const Node&);
    Node& operator=(Node&&) noexcept;
    ~Node();

    // The value of `key` in a mapping; nullptr when absent or this is not a mapping.
    const Node* get(std::string_view key) const;
    // Null: empty, or a plain "~" / "null" / "Null" / "NULL".
    bool is_null() const;
    bool is_scalar() const { return kind == Kind::Scalar && !is_null(); }
};

struct ParseError {
    std::string message;
    int line = 0, col = 0;
};

inline constexpr std::size_t kMaxText = 1u << 20;   // project files are a few KiB
inline constexpr int kMaxDepth = 64;

// The document's root (Null for an empty document).
Expected<Node, ParseError> parse(std::string_view text);

// `s` as a scalar on one line: plain when that reads back as the same string, else
// double-quoted with escapes.
std::string scalar(std::string_view s);

}  // namespace kt::ws::yaml
