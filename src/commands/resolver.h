// Key event → command. One resolver per window, UI thread.
//
// The window calls it at step 3 of the toolkit's keyboard order (after IME composition and
// modal overlay navigation, before the focused widget):
//
//     commands::ResolvedCommand rc;
//     switch (resolver.resolve(event, key_context, rc)) {
//         case KeyResult::Command:     registry.invoke(rc.command->id, cmd_ctx, *rc.args); consume
//         case KeyResult::Pending:     consume; show the "Ctrl+K …" pill from pending()
//         case KeyResult::Swallow:     consume
//         case KeyResult::Passthrough: deliver to the focused widget (rc.to_terminal: to the
//                                      window's last focused terminal, as is)
//     }
//
// or dispatch(), which does the invoke and the unavailable rules in one call.
// Hot path: three hash lookups and a short vector walk, no allocation.
#pragma once

#include <bitset>
#include <cstdint>
#include <functional>
#include <optional>

#include "commands/context.h"
#include "commands/keymap.h"
#include "commands/keys.h"
#include "commands/registry.h"
#include "platform_gui/input.h"

namespace kt::commands {

enum class KeyResult : std::uint8_t {
    Command,       // run it; consume
    Pending,       // a sequence prefix; consume and wait
    Passthrough,   // deliver to the focused widget / terminal unchanged
    Swallow,       // consume (unavailable command, cancelled sequence, ignored repeat)
};
std::string_view to_string(KeyResult r);

struct ResolvedCommand {
    const Binding* binding = nullptr;
    const Command* command = nullptr;
    const CommandArgs* args = nullptr;   // never null after KeyResult::Command
    bool repeat = false;                 // an auto-repeat of a repeatable command
    bool to_terminal = false;            // Passthrough after send-next-key: target the terminal
};

// Multi-digit tab selection while the modifier is held (Macterm's TabIndexChord).
class TabIndexRun {
public:
    static constexpr std::int64_t kWindowMs = 1000;
    // The 1-based tab the digit addresses now, or 0 when none can be addressed.
    std::uint32_t press(int digit, std::uint32_t tab_count, std::int64_t now_ms);
    void reset() { value_ = 0; last_ms_ = -1; }
    bool active(std::int64_t now_ms) const { return value_ != 0 && now_ms - last_ms_ <= kWindowMs; }

private:
    std::uint32_t value_ = 0;
    std::int64_t last_ms_ = -1;
};

class KeymapResolver {
public:
    using Clock = std::function<std::int64_t()>;   // monotonic milliseconds
    static constexpr std::int64_t kSendNextKeyTimeoutMs = 5000;

    explicit KeymapResolver(const Keymap& km, Clock now_ms = {});

    void set_keymap(const Keymap& km);   // after a keymap change; resets the state
    const Keymap& keymap() const { return *km_; }

    KeyResult resolve(const platform::KeyEvent& e, const KeyContext& ctx, ResolvedCommand& out);

    // resolve() + CommandRegistry::invoke(). Unavailable: Swallow (a toast when the command
    // asks for it) or Passthrough for yields_when_unavailable commands.
    KeyResult dispatch(const platform::KeyEvent& e, CommandContext& ctx, const CommandRegistry& r);

    void reset();                                   // focus change, window deactivation
    std::optional<Sequence> pending() const;        // for the "Ctrl+K …" status pill
    bool send_next_key_armed() const { return next_key_armed_; }
    void arm_send_next_key();                       // terminal.send_next_key does this itself
    // Expires a pending sequence (1 s) and the send-next-key state (5 s). resolve() checks too;
    // a UI timer calls this to hide the pills on time. True when something expired.
    bool tick();

private:
    std::int64_t now() const;
    KeyResult lookup(const platform::KeyEvent& e, const KeyContext& ctx, ResolvedCommand& out, bool fresh_only);
    KeyResult finish(const Binding& b, std::uint32_t index, const platform::KeyEvent& e, const KeyContext& ctx,
                     ResolvedCommand& out);
    bool yields_to_program(const Binding& b, const KeyContext& ctx) const;
    void consume_key(const platform::KeyEvent& e);

    const Keymap* km_;
    Clock clock_;
    Sequence pending_{};
    std::int64_t pending_since_ = 0;
    bool next_key_armed_ = false;
    std::int64_t next_key_since_ = 0;
    TabIndexRun tab_run_;
    std::uint8_t tab_run_mods_ = 0;
    std::uint32_t run_binding_ = 0xFFFFFFFFu;   // the tab.select binding that started the run
    CommandArgs tab_args_;               // reused: { index = N } of a multi-digit run
    std::bitset<256> consumed_;          // physical keys whose press we consumed (their release too)
};

}  // namespace kt::commands
