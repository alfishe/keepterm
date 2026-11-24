#include "commands/resolver.h"

#include <chrono>

#include "commands/layout.h"

namespace kt::commands {

std::string_view to_string(KeyResult r) {
    switch (r) {
        case KeyResult::Command: return "command";
        case KeyResult::Pending: return "pending";
        case KeyResult::Passthrough: return "passthrough";
        case KeyResult::Swallow: return "swallow";
    }
    return "passthrough";
}

// ---------------------------------------------------------------- TabIndexRun

std::uint32_t TabIndexRun::press(int digit, std::uint32_t tab_count, std::int64_t now_ms) {
    if (!active(now_ms)) value_ = 0;
    const std::uint64_t extended = static_cast<std::uint64_t>(value_) * 10u + static_cast<std::uint64_t>(digit);
    if (value_ != 0 && extended >= 1 && extended <= tab_count) {
        value_ = static_cast<std::uint32_t>(extended);
        last_ms_ = now_ms;
        return value_;
    }
    if (digit < 1 || static_cast<std::uint32_t>(digit) > tab_count) {
        reset();
        return 0;
    }
    value_ = static_cast<std::uint32_t>(digit);
    last_ms_ = now_ms;
    return value_;
}

// ---------------------------------------------------------------- helpers

namespace {

bool is_named(const Key& k, NamedKey n) { return k.kind == Key::Kind::Named && k.named == n; }

// Standard editing chords that belong to a focused text field.
bool reserved_for_text_field(const Chord& c, Os os) {
    if (c.key.kind == Key::Kind::Physical || c.key.empty()) return false;
    const std::uint8_t primary = os == Os::MacOS ? Super : Ctrl;
    const std::uint8_t word = os == Os::MacOS ? Alt : Ctrl;
    const Key& k = c.key;
    if (c.mods == primary && k.kind == Key::Kind::Char) {
        switch (k.ch) {
            case 'a': case 'c': case 'v': case 'x': case 'z': case 'y': return true;
            default: break;
        }
    }
    if (c.mods == (primary | Shift) && k.kind == Key::Kind::Char && k.ch == 'z') return true;
    const auto m = static_cast<std::uint8_t>(c.mods & ~Shift);
    if (m == word && (is_named(k, NamedKey::Left) || is_named(k, NamedKey::Right) ||
                      is_named(k, NamedKey::Backspace) || is_named(k, NamedKey::Delete)))
        return true;
    if (m == 0 && (is_named(k, NamedKey::Home) || is_named(k, NamedKey::End))) return true;
    return false;
}

int digit_of(const Chord& c) {
    if (c.key.kind == Key::Kind::Char && c.key.ch >= '0' && c.key.ch <= '9') return static_cast<int>(c.key.ch - '0');
    return -1;
}

std::int64_t steady_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// ---------------------------------------------------------------- KeymapResolver

KeymapResolver::KeymapResolver(const Keymap& km, Clock now_ms) : km_(&km), clock_(std::move(now_ms)) {
    tab_args_.set("index", config::RawValue{std::int64_t{1}});
}

void KeymapResolver::set_keymap(const Keymap& km) {
    km_ = &km;
    reset();
}

std::int64_t KeymapResolver::now() const { return clock_ ? clock_() : steady_ms(); }

void KeymapResolver::reset() {
    pending_ = {};
    next_key_armed_ = false;
    tab_run_.reset();
    tab_run_mods_ = 0;
    consumed_.reset();
}

std::optional<Sequence> KeymapResolver::pending() const {
    if (pending_.empty()) return std::nullopt;
    return pending_;
}

void KeymapResolver::arm_send_next_key() {
    next_key_armed_ = true;
    next_key_since_ = now();
    pending_ = {};
}

bool KeymapResolver::tick() {
    const std::int64_t t = now();
    bool changed = false;
    if (!pending_.empty() && t - pending_since_ > kSequenceTimeoutMs) {
        pending_ = {};
        changed = true;
    }
    if (next_key_armed_ && t - next_key_since_ > kSendNextKeyTimeoutMs) {
        next_key_armed_ = false;
        changed = true;
    }
    return changed;
}

void KeymapResolver::consume_key(const platform::KeyEvent& e) {
    const auto hid = static_cast<std::size_t>(e.key);
    if (hid < consumed_.size()) consumed_.set(hid);
}

bool KeymapResolver::yields_to_program(const Binding& b, const KeyContext& ctx) const {
    if (!b.passthrough) return false;
    const auto programs = km_->passthrough_programs();
    if (programs.empty()) return false;   // an empty list is inert
    if (!ctx.foreground_known || ctx.foreground_program.empty()) return false;   // fail toward the app action
    for (const auto& p : programs)
        if (program_matches(ctx.foreground_program, p, km_->os())) return true;
    return false;
}

KeyResult KeymapResolver::finish(const Binding& b, std::uint32_t index, const platform::KeyEvent& e,
                                 const KeyContext& ctx, ResolvedCommand& out) {
    pending_ = {};
    if (yields_to_program(b, ctx)) return KeyResult::Passthrough;
    const bool repeat = e.action == platform::KeyAction::repeat;
    if (repeat && !(b.cmd && b.cmd->repeatable)) {
        // Keys a widget shares (Backspace pops a scope only on a fresh press): the repeat goes
        // to the widget, so a held Backspace keeps deleting.
        if (b.cmd && b.cmd->yields_when_unavailable) return KeyResult::Passthrough;
        consume_key(e);
        return KeyResult::Swallow;   // a held Cmd+T opens one tab, and never types 't'
    }
    out.binding = &b;
    out.command = b.cmd;
    out.args = &b.args;
    out.repeat = repeat;
    if (index != 0) {
        tab_args_.set("index", config::RawValue{static_cast<std::int64_t>(index)});
        out.args = &tab_args_;
    }
    if (b.command == "terminal.send_next_key") arm_send_next_key();
    consume_key(e);
    return KeyResult::Command;
}

KeyResult KeymapResolver::lookup(const platform::KeyEvent& e, const KeyContext& ctx, ResolvedCommand& out,
                                 bool fresh_only) {
    const EventChords ev = map_event(e, km_->os());
    const Chord variants[3] = {ev.physical, ev.logical, ev.fallback};
    const auto bindings = km_->bindings();
    const std::int64_t t = now();

    // Continue a pending sequence.
    if (!fresh_only && !pending_.empty()) {
        if (const auto* list = km_->candidates(pending_.front())) {
            for (const Chord& v : variants) {
                if (v.key.empty()) continue;
                Sequence s = pending_;
                s.chords[s.length++] = v;
                for (const std::uint32_t i : *list) {
                    const Binding& b = bindings[i];
                    if (b.seq.length < s.length || !context_active(b.effective, ctx.bits)) continue;
                    if (!s.is_prefix_of(b.seq)) continue;
                    if (b.seq.length == s.length) return finish(b, 0, e, ctx, out);
                    pending_ = s;
                    pending_since_ = t;
                    consume_key(e);
                    return KeyResult::Pending;
                }
            }
        }
        pending_ = {};   // the chord does not continue it: drop, resolve the chord fresh
    }

    // A multi-digit tab run: a digit with the run's modifiers continues it, 0 too.
    if (tab_run_mods_ != 0 && tab_run_.active(t)) {
        int d = digit_of(ev.logical);
        if (d < 0) d = digit_of(ev.fallback);
        if (d >= 0 && ev.logical.mods == tab_run_mods_ && run_binding_ < bindings.size()) {
            const Binding& b = bindings[run_binding_];
            if (context_active(b.effective, ctx.bits)) {
                if (e.action == platform::KeyAction::repeat) {
                    consume_key(e);
                    return KeyResult::Swallow;
                }
                const std::uint32_t index = tab_run_.press(d, ctx.tab_count, t);
                if (index == 0) {
                    tab_run_mods_ = 0;
                    consume_key(e);
                    return KeyResult::Swallow;
                }
                return finish(b, index, e, ctx, out);
            }
        }
    }

    for (const Chord& v : variants) {
        if (v.key.empty()) continue;
        const auto* list = km_->candidates(v);
        if (!list) continue;
        for (const std::uint32_t i : *list) {
            const Binding& b = bindings[i];
            if (!context_active(b.effective, ctx.bits)) continue;
            if (b.seq.length > 1) {
                pending_ = Sequence::of(v);
                pending_since_ = t;
                consume_key(e);
                return KeyResult::Pending;
            }
            // tab.select on a digit starts a run; the first digit selects at once.
            if (b.command == "tab.select" && ctx.tab_count > 0) {
                const int d = digit_of(v);
                const auto idx = b.args.get_int("index");
                if (d >= 1 && idx && *idx == d) {
                    if (e.action == platform::KeyAction::repeat) {
                        consume_key(e);
                        return KeyResult::Swallow;
                    }
                    tab_run_mods_ = v.mods;
                    run_binding_ = i;
                    const std::uint32_t index = tab_run_.press(d, ctx.tab_count, t);
                    if (index == 0) {
                        tab_run_mods_ = 0;
                        consume_key(e);
                        return KeyResult::Swallow;   // no such tab: the chord is still ours
                    }
                    return finish(b, index, e, ctx, out);
                }
            }
            return finish(b, 0, e, ctx, out);
        }
    }
    return KeyResult::Passthrough;
}

KeyResult KeymapResolver::resolve(const platform::KeyEvent& e, const KeyContext& ctx, ResolvedCommand& out) {
    out = ResolvedCommand{};
    tick();
    const auto hid = static_cast<std::size_t>(e.key);
    if (e.action == platform::KeyAction::release) {
        // The run ends when its modifier is released.
        if (tab_run_mods_ != 0 && (chord_mods(e.mods) & tab_run_mods_) != tab_run_mods_) {
            tab_run_.reset();
            tab_run_mods_ = 0;
        }
        if (hid < consumed_.size() && consumed_.test(hid)) {
            consumed_.reset(hid);
            return KeyResult::Swallow;   // the press ran a command; its release is ours too
        }
        return KeyResult::Passthrough;
    }
    if (e.composing) return KeyResult::Passthrough;   // IME and dead keys never reach the keymap
    const EventChords ev = map_event(e, km_->os());
    if (ev.modifier_key) return KeyResult::Passthrough;
    if (tab_run_mods_ != 0 && ev.logical.mods != tab_run_mods_) {
        tab_run_.reset();
        tab_run_mods_ = 0;
    }

    // Send Next Key to Terminal: this chord, Escape included, goes to the terminal as is.
    if (next_key_armed_) {
        next_key_armed_ = false;
        out.to_terminal = true;
        return KeyResult::Passthrough;
    }
    // AltGr guard: Ctrl+Alt that types a character is text.
    if (altgr_types_text(e, km_->os())) {
        pending_ = {};
        return KeyResult::Passthrough;
    }
    // Text input reservation: the field's editing chords never reach the keymap.
    if (any(ctx.bits & Ctx::TextInput) && reserved_for_text_field(ev.logical, km_->os())) {
        pending_ = {};
        return KeyResult::Passthrough;
    }
    return lookup(e, ctx, out, false);
}

KeyResult KeymapResolver::dispatch(const platform::KeyEvent& e, CommandContext& ctx, const CommandRegistry& r) {
    ResolvedCommand rc;
    const KeyResult res = resolve(e, ctx.keys, rc);
    if (res != KeyResult::Command || !rc.command) return res;
    ctx.source = InvokeSource::Keymap;
    const Availability av = r.availability(*rc.command, ctx);
    if (!av.enabled) {
        if (rc.command->yields_when_unavailable) {
            const auto hid = static_cast<std::size_t>(e.key);
            if (hid < consumed_.size()) consumed_.reset(hid);
            if (rc.command->id == "terminal.send_next_key") next_key_armed_ = false;
            return KeyResult::Passthrough;
        }
        if (rc.command->id == "terminal.send_next_key") next_key_armed_ = false;
        if (av.notify_when_invoked && !av.reason.empty()) ctx.target.notify(av.reason);
        return KeyResult::Swallow;   // never leaks into the terminal
    }
    if (auto st = r.invoke(rc.command->id, ctx, *rc.args); !st && st.error().code != Errc::unavailable)
        ctx.target.notify(st.error().what);
    return KeyResult::Command;
}

}  // namespace kt::commands
