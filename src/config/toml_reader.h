// Internal: TOML text to a positioned tree (the only place that sees toml++ types).
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "config/value.h"

namespace kt::config::detail {

struct PNode;
using PTable = std::vector<std::pair<std::string, PNode>>;  // file order
using PArray = std::vector<PNode>;

// One of a scalar, a table or an array. Three members and a kind rather than a std::variant:
// a variant of containers of the node itself needs the node complete too early for clang with
// libstdc++ 15.
struct PNode {
    int line = 0, column = 0;
    bool inline_table = false;  // `{ ... }` (dotted-key and [header] tables are not inline)

    const RawValue* scalar() const { return kind_ == Kind::Scalar ? &scalar_ : nullptr; }
    const PTable* table() const { return kind_ == Kind::Table ? &table_ : nullptr; }
    const PArray* array() const { return kind_ == Kind::Array ? &array_ : nullptr; }

    void set(RawValue v) {
        clear();
        kind_ = Kind::Scalar;
        scalar_ = std::move(v);
    }
    void set(PTable t) {
        clear();
        kind_ = Kind::Table;
        table_ = std::move(t);
    }
    void set(PArray a) {
        clear();
        kind_ = Kind::Array;
        array_ = std::move(a);
    }

private:
    enum class Kind : unsigned char { Scalar, Table, Array };
    Kind kind_ = Kind::Scalar;  // RawValue holds scalars only
    RawValue scalar_;
    PTable table_;
    PArray array_;
    void clear() {
        scalar_ = RawValue{};
        table_.clear();
        array_.clear();
    }
};

RawValue to_raw(const PNode& n);

struct ParseError {
    std::string message;
    int line = 0, column = 0;
};

// Parses a document; the root is a table. `source` is used in messages only.
std::variant<PNode, ParseError> parse_toml(std::string_view text, std::string_view source);

}  // namespace kt::config::detail
