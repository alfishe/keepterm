// Workspace ids, the random source and the clock of the workspace model.
// Core code: no OS headers.
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "base/expected.h"
#include "session/ids.h"

namespace kt::ws {

// Random bytes. The real one is the OS CSPRNG (platform::random_bytes); tests use SeededRandom.
class IRandom {
public:
    virtual ~IRandom() = default;
    virtual void fill(std::span<std::byte> out) = 0;
};
IRandom& system_random();

// A deterministic generator for tests (splitmix64).
class SeededRandom final : public IRandom {
public:
    explicit SeededRandom(std::uint64_t seed = 1) : state_(seed) {}
    void fill(std::span<std::byte> out) override;

private:
    std::uint64_t state_;
};

// Time. steady_ms drives the save debounce; unix_ms stamps recent folders and file names.
class IClock {
public:
    virtual ~IClock() = default;
    virtual std::int64_t steady_ms() const = 0;
    virtual std::int64_t unix_ms() const = 0;
};
IClock& system_clock();

// A clock that moves only when told to (tests).
class ManualClock final : public IClock {
public:
    std::int64_t steady_ms() const override { return steady_; }
    std::int64_t unix_ms() const override { return unix_; }
    void advance(std::int64_t ms) { steady_ += ms; unix_ += ms; }
    void set_unix_ms(std::int64_t ms) { unix_ = ms; }

private:
    std::int64_t steady_ = 1'000'000;
    std::int64_t unix_ = 1'763'547'302'000;   // 2025-11-19
};

// 128-bit random id, rendered as lowercase UUID v4 text ("3f2a...-..."). Never derived from names.
struct Id128 {
    std::array<std::uint8_t, 16> bytes{};

    static Id128 generate(IRandom& rng);
    // Accepts the 36-char form, any hex case. false on anything else.
    static bool parse(std::string_view text, Id128& out);
    std::string to_string() const;   // 36 chars, lowercase
    bool is_nil() const;

    auto operator<=>(const Id128&) const = default;
};

template <class Tag>
struct TypedId {
    Id128 value;
    std::string to_string() const { return value.to_string(); }
    bool is_nil() const { return value.is_nil(); }
    auto operator<=>(const TypedId&) const = default;
};
using ProjectId = TypedId<struct ProjectTag>;
using TabId = TypedId<struct TabTag>;
using PaneId = TypedId<struct PaneTag>;
using WindowId = TypedId<struct WindowTag>;

template <class Id>
Id new_id(IRandom& rng) {
    return Id{Id128::generate(rng)};
}
template <class Id>
bool parse_id(std::string_view text, Id& out) {
    return Id128::parse(text, out.value);
}

// The scratch workspace: f1e0d0d0-0000-4000-8000-70616e736372.
inline constexpr ProjectId kScratchProject{Id128{{0xf1, 0xe0, 0xd0, 0xd0, 0x00, 0x00, 0x40, 0x00,
                                                  0x80, 0x00, 0x70, 0x61, 0x6e, 0x73, 0x63, 0x72}}};
// The quick terminal's workspace: f1e0d0d0-0000-4000-8000-717569636b31.
inline constexpr ProjectId kQuickProject{Id128{{0xf1, 0xe0, 0xd0, 0xd0, 0x00, 0x00, 0x40, 0x00,
                                                0x80, 0x00, 0x71, 0x75, 0x69, 0x63, 0x6b, 0x31}}};

// True for the two fixed workspaces, which are not rows of the project list.
inline bool is_fixed_workspace(ProjectId p) { return p == kScratchProject || p == kQuickProject; }

// Session ids are kt::session::SessionId. This one draws its 80 bits from `rng`.
session::SessionId new_session_id(IRandom& rng);

}  // namespace kt::ws
