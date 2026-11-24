// Expected<T, E>: a value or an error (errors are values, no exceptions across modules).
// A small shim with the subset of std::expected that keepterm uses; C++20 has no std::expected.
#pragma once

#include <cassert>
#include <type_traits>
#include <utility>
#include <variant>

namespace kt {

template <class E>
class Unexpected {
public:
    explicit Unexpected(E e) : error_(std::move(e)) {}
    E& error() & { return error_; }
    E&& error() && { return std::move(error_); }
    const E& error() const& { return error_; }

private:
    E error_;
};

template <class E>
Unexpected(E) -> Unexpected<E>;

template <class T, class E>
class [[nodiscard]] Expected {
public:
    using value_type = T;
    using error_type = E;

    Expected() requires std::is_default_constructible_v<T> : v_(std::in_place_index<0>) {}
    Expected(T value) : v_(std::in_place_index<0>, std::move(value)) {}  // NOLINT(google-explicit-constructor)
    template <class G>
    Expected(Unexpected<G> u) : v_(std::in_place_index<1>, std::move(u).error()) {}  // NOLINT

    bool has_value() const { return v_.index() == 0; }
    explicit operator bool() const { return has_value(); }

    T& value() & { assert(has_value()); return std::get<0>(v_); }
    const T& value() const& { assert(has_value()); return std::get<0>(v_); }
    T&& value() && { assert(has_value()); return std::get<0>(std::move(v_)); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(*this).value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    E& error() & { assert(!has_value()); return std::get<1>(v_); }
    const E& error() const& { assert(!has_value()); return std::get<1>(v_); }
    E&& error() && { assert(!has_value()); return std::get<1>(std::move(v_)); }

    template <class U>
    T value_or(U&& fallback) const& {
        return has_value() ? std::get<0>(v_) : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, E> v_;
};

template <class E>
class [[nodiscard]] Expected<void, E> {
public:
    using value_type = void;
    using error_type = E;

    Expected() = default;
    template <class G>
    Expected(Unexpected<G> u) : error_(std::move(u).error()), ok_(false) {}  // NOLINT

    bool has_value() const { return ok_; }
    explicit operator bool() const { return ok_; }
    void value() const { assert(ok_); }
    E& error() & { assert(!ok_); return error_; }
    const E& error() const& { assert(!ok_); return error_; }

private:
    E error_{};
    bool ok_ = true;
};

}  // namespace kt
