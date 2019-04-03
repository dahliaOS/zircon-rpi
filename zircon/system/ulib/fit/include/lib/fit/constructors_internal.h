// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIT_CONSTRUCTORS_INTERNAL_H_
#define LIB_FIT_CONSTRUCTORS_INTERNAL_H_

#include <type_traits>

namespace fit {
namespace internal {

// Mixin type the modulates the copy/move constructors and assignment operators
// by selectively defaulting or deleting constructors and assignment operators
// based on the traits of type T.
//
// For example:
//
//     template <typename T>
//     class my_value_type : private modulate_copy_and_move<my_value_type<T>, T> {
//         ...
//     };
//
template <typename Subclass, typename T,
          bool copy_constructible = std::is_copy_constructible<T>::value,
          bool copy_assignable = (std::is_copy_constructible<T>::value &&
                                  std::is_copy_assignable<T>::value),
          bool move_constructible = std::is_move_constructible<T>::value,
          bool move_assignable = (std::is_move_constructible<T>::value &&
                                  std::is_move_assignable<T>::value)>
struct modulate_copy_and_move {
    static_assert(sizeof(T) != sizeof(T),
                  "Invalid combination of copy/move construction and assignment!");
};

template <typename Subclass, typename T>
struct modulate_copy_and_move<Subclass, T, false, false, false, false> {
    constexpr modulate_copy_and_move() = default;

    constexpr modulate_copy_and_move(const modulate_copy_and_move&) noexcept = delete;
    constexpr modulate_copy_and_move& operator=(const modulate_copy_and_move&) noexcept = delete;

    constexpr modulate_copy_and_move(modulate_copy_and_move&&) noexcept = delete;
    constexpr modulate_copy_and_move& operator=(modulate_copy_and_move&&) noexcept = delete;
};

template <typename Subclass, typename T>
struct modulate_copy_and_move<Subclass, T, false, false, true, false> {
    constexpr modulate_copy_and_move() = default;

    constexpr modulate_copy_and_move(const modulate_copy_and_move&) noexcept = delete;
    constexpr modulate_copy_and_move& operator=(const modulate_copy_and_move&) noexcept = delete;

    constexpr modulate_copy_and_move(modulate_copy_and_move&&) noexcept = default;
    constexpr modulate_copy_and_move& operator=(modulate_copy_and_move&&) noexcept = delete;
};

template <typename Subclass, typename T>
struct modulate_copy_and_move<Subclass, T, false, false, true, true> {
    constexpr modulate_copy_and_move() = default;

    constexpr modulate_copy_and_move(const modulate_copy_and_move&) noexcept = delete;
    constexpr modulate_copy_and_move& operator=(const modulate_copy_and_move&) noexcept = delete;

    constexpr modulate_copy_and_move(modulate_copy_and_move&&) noexcept = default;
    constexpr modulate_copy_and_move& operator=(modulate_copy_and_move&&) noexcept = default;
};

template <typename Subclass, typename T>
struct modulate_copy_and_move<Subclass, T, true, false, false, false> {
    constexpr modulate_copy_and_move() = default;

    constexpr modulate_copy_and_move(const modulate_copy_and_move&) noexcept = default;
    constexpr modulate_copy_and_move& operator=(const modulate_copy_and_move&) noexcept = delete;

    constexpr modulate_copy_and_move(modulate_copy_and_move&&) noexcept = delete;
    constexpr modulate_copy_and_move& operator=(modulate_copy_and_move&&) noexcept = delete;
};

template <typename Subclass, typename T>
struct modulate_copy_and_move<Subclass, T, true, false, true, false> {
    constexpr modulate_copy_and_move() = default;

    constexpr modulate_copy_and_move(const modulate_copy_and_move&) noexcept = default;
    constexpr modulate_copy_and_move& operator=(const modulate_copy_and_move&) noexcept = delete;

    constexpr modulate_copy_and_move(modulate_copy_and_move&&) noexcept = default;
    constexpr modulate_copy_and_move& operator=(modulate_copy_and_move&&) noexcept = delete;
};

template <typename Subclass, typename T>
struct modulate_copy_and_move<Subclass, T, true, false, true, true> {
    constexpr modulate_copy_and_move() = default;

    constexpr modulate_copy_and_move(const modulate_copy_and_move&) noexcept = default;
    constexpr modulate_copy_and_move& operator=(const modulate_copy_and_move&) noexcept = delete;

    constexpr modulate_copy_and_move(modulate_copy_and_move&&) noexcept = default;
    constexpr modulate_copy_and_move& operator=(modulate_copy_and_move&&) noexcept = default;
};

template <typename Subclass, typename T>
struct modulate_copy_and_move<Subclass, T, true, true, false, false> {
    constexpr modulate_copy_and_move() = default;

    constexpr modulate_copy_and_move(const modulate_copy_and_move&) noexcept = default;
    constexpr modulate_copy_and_move& operator=(const modulate_copy_and_move&) noexcept = default;

    constexpr modulate_copy_and_move(modulate_copy_and_move&&) noexcept = delete;
    constexpr modulate_copy_and_move& operator=(modulate_copy_and_move&&) noexcept = delete;
};

template <typename Subclass, typename T>
struct modulate_copy_and_move<Subclass, T, true, true, true, false> {
    constexpr modulate_copy_and_move() = default;

    constexpr modulate_copy_and_move(const modulate_copy_and_move&) noexcept = default;
    constexpr modulate_copy_and_move& operator=(const modulate_copy_and_move&) noexcept = default;

    constexpr modulate_copy_and_move(modulate_copy_and_move&&) noexcept = default;
    constexpr modulate_copy_and_move& operator=(modulate_copy_and_move&&) noexcept = delete;
};

template <typename Subclass, typename T>
struct modulate_copy_and_move<Subclass, T, true, true, true, true> {
    constexpr modulate_copy_and_move() = default;

    constexpr modulate_copy_and_move(const modulate_copy_and_move&) noexcept = default;
    constexpr modulate_copy_and_move& operator=(const modulate_copy_and_move&) noexcept = default;

    constexpr modulate_copy_and_move(modulate_copy_and_move&&) noexcept = default;
    constexpr modulate_copy_and_move& operator=(modulate_copy_and_move&&) noexcept = default;
};

} // namespace internal
} // namespace fit

#endif //  LIB_FIT_CONSTRUCTORS_INTERNAL_H_
