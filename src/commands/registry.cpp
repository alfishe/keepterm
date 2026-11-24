#include "commands/registry.h"

#include <algorithm>
#include <cassert>
#include <exception>
#include <variant>

#include "base/log.h"

namespace kt::commands {

std::string_view category_title(Category c) {
    switch (c) {
        case Category::Palettes: return "Palettes";
        case Category::Tabs: return "Tabs";
        case Category::Panes: return "Panes";
        case Category::Terminal: return "Terminal";
        case Category::Projects: return "Projects";
        case Category::Window: return "Window";
        case Category::App: return "App";
    }
    return "App";
}

// ---------------------------------------------------------------- CommandArgs

bool CommandArgs::empty() const {
    const auto* t = value.table();
    return !t || t->empty();
}

const config::RawValue* CommandArgs::get(std::string_view name) const { return value.find(name); }

std::optional<std::int64_t> CommandArgs::get_int(std::string_view name) const {
    const auto* v = get(name);
    if (!v) return std::nullopt;
    if (const auto* i = std::get_if<std::int64_t>(&v->v)) return *i;
    return std::nullopt;
}

std::optional<std::string_view> CommandArgs::get_string(std::string_view name) const {
    const auto* v = get(name);
    if (!v) return std::nullopt;
    if (const auto* s = std::get_if<std::string>(&v->v)) return std::string_view(*s);
    return std::nullopt;
}

void CommandArgs::set(std::string_view name, config::RawValue v) {
    if (!value.is_table()) value.v = config::RawValue::Table{};
    auto& t = std::get<config::RawValue::Table>(value.v);
    for (auto& e : t) {
        if (e.key == name) {
            e.value = std::move(v);
            return;
        }
    }
    t.push_back(config::RawEntry{std::string(name), std::move(v)});
}

Status validate_args(const ArgSpec& spec, const CommandArgs& a) {
    const auto* t = a.value.table();
    if (!t) return make_error(Errc::invalid_argument, "arguments must be a table");
    for (const auto& e : *t) {
        const bool known = std::any_of(spec.begin(), spec.end(), [&](const ArgField& f) { return f.name == e.key; });
        if (!known) {
            std::string names;
            for (const auto& f : spec) names += (names.empty() ? "" : ", ") + std::string(f.name);
            return make_error(Errc::invalid_argument, "unknown argument '" + e.key + "'" +
                                                          (names.empty() ? " (the command takes none)"
                                                                         : " (expected " + names + ")"));
        }
    }
    for (const auto& f : spec) {
        const auto* v = a.get(f.name);
        if (!v) {
            if (f.required) return make_error(Errc::invalid_argument, "missing argument '" + std::string(f.name) + "'");
            continue;
        }
        if (f.type == ArgType::Int) {
            const auto* i = std::get_if<std::int64_t>(&v->v);
            if (!i) return make_error(Errc::invalid_argument, "argument '" + std::string(f.name) + "' must be an integer");
            if (*i < f.min || (f.max != 0 && *i > f.max))
                return make_error(Errc::invalid_argument, "argument '" + std::string(f.name) + "' is out of range (" +
                                                              std::to_string(f.min) + ".." +
                                                              (f.max ? std::to_string(f.max) : std::string("")) + ")");
        } else {
            if (!std::holds_alternative<std::string>(v->v))
                return make_error(Errc::invalid_argument, "argument '" + std::string(f.name) + "' must be a string");
            if (f.type == ArgType::Chord) {
                auto s = parse_sequence(std::get<std::string>(v->v), current_os());
                if (!s)
                    return make_error(Errc::invalid_argument, "argument '" + std::string(f.name) +
                                                                  "': " + s.error().message);
            }
        }
    }
    return {};
}

// ---------------------------------------------------------------- CommandRegistry

void CommandRegistry::add(Command c) {
    if (by_id_.count(c.id)) {
        assert(false && "duplicate command id");
        return;
    }
    commands_.push_back(std::move(c));
    by_id_.clear();   // the vector may have moved; ids are static string_views, so rebuild
    for (std::size_t i = 0; i < commands_.size(); ++i) by_id_.emplace(commands_[i].id, i);
}

const Command* CommandRegistry::find(CommandId id) const {
    const auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : &commands_[it->second];
}

std::size_t CommandRegistry::index_of(CommandId id) const {
    const auto it = by_id_.find(id);
    return it == by_id_.end() ? static_cast<std::size_t>(-1) : it->second;
}

Availability CommandRegistry::availability(const Command& c, const CommandContext& ctx) const {
    if (!c.availability) return {};
    return c.availability(ctx);
}

Status CommandRegistry::invoke(CommandId id, CommandContext& ctx, const CommandArgs& a) const {
    const Command* c = find(id);
    if (!c) {
        std::string msg = "unknown command '" + std::string(id) + "'";
        if (auto s = suggest(id); !s.empty()) msg += " (" + s + ")";
        return make_error(Errc::invalid_argument, std::move(msg));
    }
    if (auto v = validate_args(c->args, a); !v) return v;
    const Availability av = availability(*c, ctx);
    if (!av.enabled) {
        if (ctx.source == InvokeSource::Keymap && av.notify_when_invoked && !av.reason.empty())
            ctx.target.notify(av.reason);
        return make_error(Errc::unavailable, av.reason.empty() ? std::string(c->title) + " is not available" : av.reason);
    }
    if (!c->handler) return make_error(Errc::not_supported, std::string(c->id) + " has no handler");
    try {
        return c->handler(ctx, a);
    } catch (const std::exception& e) {
        log::error("commands", std::string(c->id) + " failed: " + e.what());
    } catch (...) {
        log::error("commands", std::string(c->id) + " failed");
    }
    ctx.target.notify("Command failed");
    return make_error(Errc::os_error, "command failed");
}

std::string CommandRegistry::title(const Command& c) const {
    if (translate_) {
        std::string t = translate_(c.title);
        if (!t.empty()) return t;
    }
    return std::string(c.title);
}

namespace {

std::size_t distance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, sub});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

}  // namespace

std::string CommandRegistry::suggest(std::string_view unknown) const {
    std::string_view best;
    std::size_t best_d = 4;
    for (const auto& c : commands_) {
        const std::size_t d = distance(unknown, c.id);
        if (d < best_d) {
            best_d = d;
            best = c.id;
        }
    }
    if (best.empty()) {   // a known verb in another group ("split_right" → "pane.split_right")
        for (const auto& c : commands_) {
            const auto dot = c.id.find('.');
            if (dot != std::string_view::npos && c.id.substr(dot + 1) == unknown) {
                best = c.id;
                break;
            }
        }
    }
    return best.empty() ? std::string() : "did you mean '" + std::string(best) + "'?";
}

}  // namespace kt::commands
