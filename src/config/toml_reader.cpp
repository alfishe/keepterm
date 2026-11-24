// toml++ adapter. toml++ is compiled in kt_tomlpp (TOML_HEADER_ONLY=0, TOML_EXCEPTIONS=0).
#include "config/toml_reader.h"

#include <sstream>

#include <toml.hpp>

namespace kt::config::detail {
namespace {

int to_int(toml::source_index v) { return v > 0x7fffffffu ? 0x7fffffff : static_cast<int>(v); }

PNode convert(const toml::node& n);

PNode convert_table(const toml::table& t) {
    PNode out;
    out.line = to_int(t.source().begin.line);
    out.column = to_int(t.source().begin.column);
    out.inline_table = t.is_inline();
    PTable items;
    for (auto&& [k, v] : t) {
        PNode child = convert(v);
        // Report the key's position; a [header] table's own position is its header line.
        if (k.source().begin.line) {
            child.line = to_int(k.source().begin.line);
            child.column = to_int(k.source().begin.column);
        }
        items.emplace_back(std::string(k.str()), std::move(child));
    }
    out.set(std::move(items));
    return out;
}

PNode convert(const toml::node& n) {
    PNode out;
    out.line = to_int(n.source().begin.line);
    out.column = to_int(n.source().begin.column);
    switch (n.type()) {
        case toml::node_type::table: return convert_table(*n.as_table());
        case toml::node_type::array: {
            PArray a;
            for (const auto& e : *n.as_array()) a.push_back(convert(e));
            out.set(std::move(a));
            return out;
        }
        case toml::node_type::string: out.set(RawValue{std::string(n.as_string()->get())}); return out;
        case toml::node_type::integer: out.set(RawValue{n.as_integer()->get()}); return out;
        case toml::node_type::floating_point: out.set(RawValue{n.as_floating_point()->get()}); return out;
        case toml::node_type::boolean: out.set(RawValue{n.as_boolean()->get()}); return out;
        case toml::node_type::date: {
            std::ostringstream s;
            s << *n.as_date();
            out.set(RawValue{s.str()});
            return out;
        }
        case toml::node_type::time: {
            std::ostringstream s;
            s << *n.as_time();
            out.set(RawValue{s.str()});
            return out;
        }
        case toml::node_type::date_time: {
            std::ostringstream s;
            s << *n.as_date_time();
            out.set(RawValue{s.str()});
            return out;
        }
        case toml::node_type::none: break;
    }
    out.set(RawValue{std::string()});
    return out;
}

}  // namespace

RawValue to_raw(const PNode& n) {
    if (const auto* s = n.scalar()) return *s;
    if (const auto* t = n.table()) {
        RawValue::Table out;
        out.reserve(t->size());
        for (const auto& [k, v] : *t) out.push_back(RawEntry{k, to_raw(v)});
        return RawValue{std::move(out)};
    }
    RawValue::Array out;
    for (const auto& e : *n.array()) out.push_back(to_raw(e));
    return RawValue{std::move(out)};
}

std::variant<PNode, ParseError> parse_toml(std::string_view text, std::string_view source) {
    toml::parse_result r = toml::parse(text, source);
    if (!r) {
        const auto& e = r.error();
        return ParseError{std::string(e.description()), to_int(e.source().begin.line),
                          to_int(e.source().begin.column)};
    }
    PNode root = convert_table(r.table());
    root.line = root.column = 0;
    return root;
}

}  // namespace kt::config::detail
