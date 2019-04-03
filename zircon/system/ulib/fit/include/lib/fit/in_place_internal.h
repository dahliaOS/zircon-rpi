// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIT_IN_PLACE_INTERNAL_H_
#define LIB_FIT_IN_PLACE_INTERNAL_H_

namespace fit {

// Tag for requesting in-place initialization.
struct in_place_t {
    explicit in_place_t() = default;
};

// Tag for requesting in-place initialization by type.
template <typename T>
struct in_place_type_t {
    explicit in_place_type_t() = default;
};

// Tag for requesting in-place initialization by index.
template <size_t index>
struct in_place_index_t final {
    explicit in_place_index_t() = default;
};

#ifdef __cpp_inline_variables

// Inline variables are only available on C++ 17 and beyond.

inline constexpr in_place_t in_place{};

template <typename T>
inline constexpr in_place_type_t<T> in_place_type{};

template <size_t index>
inline constexpr in_place_index_t<index> in_place_index{};

#else

// For C++ 14 we need to provide storage for the variable so we define
// a reference instead.

template <typename Dummy = void>
struct in_place_holder {
    static constexpr in_place_t instance{};
};

template <typename T>
struct in_place_type_holder {
    static constexpr in_place_type_t<T> instance{};
};

template <size_t index>
struct in_place_index_holder {
    static constexpr in_place_index_t<index> instance{};
};

template <typename Dummy>
constexpr in_place_t in_place_holder<Dummy>::instance;

template <typename T>
constexpr in_place_type_t<T> in_place_type_holder::instance;

template <size_t index>
constexpr in_place_index_t<index> in_place_index_holder<index>::instance;

static constexpr const in_place_t& in_place = in_place_holder<>::instance;

template <typename T>
static constexpr const in_place_type_t<T>& in_place_type =
    in_place_type_holder<T>::instance;

template <size_t index>
static constexpr const in_place_index_t<index>& in_place_index =
    in_place_index_holder<index>::instance;

#endif // __cpp_inline_variables


} // namespace fit

#endif // LIB_FIT_IN_PLACE_INTERNAL_H_

