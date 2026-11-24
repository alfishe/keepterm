#include "workspace/ids.h"

#include <chrono>
#include <cstring>

#include "platform/random.h"

namespace kt::ws {
namespace {

class SystemRandom final : public IRandom {
public:
    void fill(std::span<std::byte> out) override {
        if (platform::random_bytes(out)) return;
        // The OS CSPRNG never fails in practice; keep ids unique anyway.
        static std::uint64_t counter = 0;
        SeededRandom fallback(static_cast<std::uint64_t>(
                                  std::chrono::steady_clock::now().time_since_epoch().count()) ^
                              (++counter * 0x9E3779B97F4A7C15ull));
        fallback.fill(out);
    }
};

class SystemClock final : public IClock {
public:
    std::int64_t steady_ms() const override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    std::int64_t unix_ms() const override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
};

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

IRandom& system_random() {
    static SystemRandom r;
    return r;
}

IClock& system_clock() {
    static SystemClock c;
    return c;
}

void SeededRandom::fill(std::span<std::byte> out) {
    std::size_t i = 0;
    while (i < out.size()) {
        state_ += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        for (int b = 0; b < 8 && i < out.size(); ++b, ++i) out[i] = static_cast<std::byte>((z >> (8 * b)) & 0xFF);
    }
}

Id128 Id128::generate(IRandom& rng) {
    Id128 id;
    std::array<std::byte, 16> raw{};
    rng.fill(raw);
    for (std::size_t i = 0; i < 16; ++i) id.bytes[i] = static_cast<std::uint8_t>(raw[i]);
    id.bytes[6] = static_cast<std::uint8_t>((id.bytes[6] & 0x0F) | 0x40);   // version 4
    id.bytes[8] = static_cast<std::uint8_t>((id.bytes[8] & 0x3F) | 0x80);   // variant 1
    return id;
}

bool Id128::parse(std::string_view text, Id128& out) {
    if (text.size() != 36) return false;
    Id128 id;
    std::size_t b = 0;
    for (std::size_t i = 0; i < 36;) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return false;
            ++i;
            continue;
        }
        const int hi = hex_value(text[i]), lo = hex_value(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        id.bytes[b++] = static_cast<std::uint8_t>((hi << 4) | lo);
        i += 2;
    }
    out = id;
    return true;
}

std::string Id128::to_string() const {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(36);
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
        s.push_back(kHex[bytes[i] >> 4]);
        s.push_back(kHex[bytes[i] & 0x0F]);
    }
    return s;
}

bool Id128::is_nil() const {
    for (auto b : bytes)
        if (b) return false;
    return true;
}

session::SessionId new_session_id(IRandom& rng) {
    std::array<std::byte, 10> raw{};
    rng.fill(raw);
    return session::SessionId::from_bytes(raw);
}

}  // namespace kt::ws
