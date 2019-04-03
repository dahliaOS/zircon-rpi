// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIT_OPTIONAL_H_
#define LIB_FIT_OPTIONAL_H_

#include <cassert>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "constructors_internal.h"
#include "in_place_internal.h"
#include "storage_internal.h"

namespace fit {

// A sentinel value for indicating that it contains no value.
struct nullopt_t {
    explicit constexpr nullopt_t(int) {}
};
static constexpr nullopt_t nullopt{0};

// A reasonably complete implementation of |std::optional<T>| for C++ 14.
//
// See also |fit::nullable<T>| which may be more efficient in certain
// circumstances if T can be initialized, assigned, and compared with
// nullptr.
//
template <typename T>
class optional : private ::fit::internal::modulate_copy_and_move<optional<T>, T> {
private:
    // Helper types and values for SFINAE and noexcept rules.
    static constexpr bool nothrow_move_constructible =
        std::is_nothrow_move_constructible<T>::value;

    static constexpr bool nothrow_swappable =
        std::is_nothrow_move_constructible<T>::value &&
        std::is_nothrow_swappable<T>::value;

    static constexpr auto always_init_v = ::fit::internal::always_init_v<T>;
    static constexpr auto maybe_init_v = ::fit::internal::maybe_init_v;
    static constexpr auto type_tag_v = ::fit::internal::type_tag_v<T>;

    template <typename... Conditions>
    using and_type = ::fit::internal::and_type<Conditions...>;

    template <typename... Conditions>
    static constexpr bool and_v = ::fit::internal::and_v<Conditions...>;

    template <typename... Conditions>
    using or_type = ::fit::internal::or_type<Conditions...>;

    template <typename Condition>
    using not_type = ::fit::internal::not_type<Condition>;

    template <typename U, typename V>
    using converts_from_optional =
        or_type<
            std::is_constructible<U, const optional<V>&>,
            std::is_constructible<U, optional<V>&>,
            std::is_constructible<U, const optional<V>&&>,
            std::is_constructible<U, optional<V>&&>,
            std::is_convertible<const optional<V>&, U>,
            std::is_convertible<optional<V>&, U>,
            std::is_convertible<const optional<V>&&, U>,
            std::is_convertible<optional<V>&&, U>>;

    template <typename U, typename V>
    using assigns_from_optional =
        or_type<
            std::is_assignable<U&, const optional<V>&>,
            std::is_assignable<U&, optional<V>&>,
            std::is_assignable<U&, const optional<V>&&>,
            std::is_assignable<U&, optional<V>&&>>;

    template <typename U>
    using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<U>>;

    template <typename U>
    using not_this_type = not_type<std::is_same<optional, remove_cvref_t<U>>>;

    template <typename U>
    using not_in_place = not_type<std::is_same<in_place_t, remove_cvref_t<U>>>;

    template <typename... Conditions>
    using requires_conditions = std::enable_if_t<and_v<Conditions...>, bool>;

    template <typename... Conditions>
    using assignment_requires_conditions = std::enable_if_t<and_v<Conditions...>, optional&>;

    template <typename... Args>
    using emplace_constructible = std::enable_if_t<
        std::is_constructible<T, Args...>::value, T&>;

public:
    using value_type = T;

    ~optional() = default;

    constexpr optional() = default;

    constexpr optional(nullopt_t) noexcept {}

    // Converting constructors.

    template <typename U = T,
              requires_conditions<
                  not_this_type<U>,
                  not_in_place<U>,
                  std::is_constructible<T, U&&>,
                  std::is_convertible<U&&, T>> = true>
    constexpr optional(U&& value)
        : storage_{always_init_v, std::forward<U>(value)} {}

    template <typename U = T,
              requires_conditions<
                  not_this_type<U>,
                  not_in_place<U>,
                  std::is_constructible<T, U&&>,
                  not_type<std::is_convertible<U&&, T>>> = false>
    explicit constexpr optional(U&& value)
        : storage_{always_init_v, std::forward<U>(value)} {}

    template <typename U,
              requires_conditions<
                  not_type<std::is_same<T, U>>,
                  std::is_constructible<T, const U&>,
                  std::is_convertible<const U&, T>,
                  not_type<converts_from_optional<T, U>>> = true>
    constexpr optional(const optional<U>& other)
        : storage_{maybe_init_v, other.storage_} {}

    template <typename U,
              requires_conditions<
                  not_type<std::is_same<T, U>>,
                  std::is_constructible<T, const U&>,
                  not_type<std::is_convertible<const U&, T>>,
                  not_type<converts_from_optional<T, U>>> = false>
    explicit constexpr optional(const optional<U>& other)
        : storage_{maybe_init_v, other.storage_} {}

    template <typename U,
              requires_conditions<
                  not_type<std::is_same<T, U>>,
                  std::is_constructible<T, U&&>,
                  std::is_convertible<U&&, T>,
                  not_type<converts_from_optional<T, U>>> = true>
    constexpr optional(optional<U>&& other)
        : storage_{maybe_init_v, std::move(other.storage_)} {}

    template <typename U,
              requires_conditions<
                  not_type<std::is_same<T, U>>,
                  std::is_constructible<T, U&&>,
                  not_type<std::is_convertible<U&&, T>>,
                  not_type<converts_from_optional<T, U>>> = false>
    explicit constexpr optional(optional<U>&& other)
        : storage_{maybe_init_v, std::move(other.storage_)} {}

    template <typename... Args,
              requires_conditions<std::is_constructible<T, Args&&...>> = false>
    explicit constexpr optional(in_place_t, Args&&... args)
        : storage_{always_init_v, std::forward<Args>(args)...} {}

    template <typename U, typename... Args,
              requires_conditions<
                  std::is_constructible<T, std::initializer_list<U>&, Args&&...>> = false>
    explicit constexpr optional(in_place_t, std::initializer_list<U> init_list, Args&&... args)
        : storage_{always_init_v, init_list, std::forward<Args>(args)...} {}

    // Checked accessors.

    constexpr T& value() & {
        assert(has_value());
        return storage_.get(type_tag_v);
    }
    constexpr const T& value() const& {
        assert(has_value());
        return storage_.get(type_tag_v);
    }
    constexpr T&& value() && {
        assert(has_value());
        return std::move(storage_.get(type_tag_v));
    }
    constexpr const T&& value() const&& {
        assert(has_value());
        return std::move(storage_.get(type_tag_v));
    }

    template <typename U>
    constexpr T value_or(U&& default_value) const& {
        static_assert(std::is_copy_constructible<T>::value,
                      "value_or() requires copy-constructible value_type!");
        static_assert(std::is_convertible<U&&, T>::value,
                      "Default value must be convertible to value_type!");

        return has_value() ? storage_.get(type_tag_v)
                           : static_cast<T>(std::forward<U>(default_value));
    }
    template <typename U>
    constexpr T value_or(U&& default_value) && {
        static_assert(std::is_move_constructible<T>::value,
                      "value_or() requires move-constructible value_type!");
        static_assert(std::is_convertible<U&&, T>::value,
                      "Default value must be convertible to value_type!");

        return has_value() ? std::move(storage_.get(type_tag_v))
                           : static_cast<T>(std::forward<U>(default_value));
    }

    // Unchecked accessors.

    constexpr T* operator->() { return std::addressof(storage_.get(type_tag_v)); }
    constexpr const T* operator->() const { return &storage_.get(type_tag_v); }

    constexpr T& operator*() { return storage_.get(type_tag_v); }
    constexpr const T& operator*() const { return storage_.get(type_tag_v); }

    // Availability accessors/operators.

    constexpr bool has_value() const { return storage_.has_value(); }
    constexpr explicit operator bool() const { return has_value(); }

    // Assignment operators.

    template <typename U>
    constexpr assignment_requires_conditions<
        not_this_type<U>,
        not_type<
            and_type<std::is_scalar<T>,
                     std::is_same<T, std::decay_t<U>>>>,
        std::is_constructible<T, U>,
        std::is_assignable<T&, U>>
    operator=(U&& value) {
        if (has_value()) {
            storage_.get(type_tag_v) = std::forward<U>(value);
        } else {
            storage_.construct(type_tag_v, std::forward<U>(value));
        }
        return *this;
    }

    template <typename U>
    constexpr assignment_requires_conditions<
        not_type<std::is_same<T, U>>,
        std::is_constructible<T, const U&>,
        std::is_assignable<T&, U>,
        not_type<converts_from_optional<T, U>>,
        not_type<assigns_from_optional<T, U>>>
    operator=(const optional<U>& other) {
        if (other.has_value()) {
            if (has_value()) {
                storage_.get(type_tag_v) = *other;
            } else {
                storage_.construct(type_tag_v, *other);
            }
        } else {
            storage_.reset();
        }
        return *this;
    }

    template <typename U>
    constexpr assignment_requires_conditions<
        not_type<std::is_same<T, U>>,
        std::is_constructible<T, U>,
        std::is_assignable<T&, U>,
        not_type<converts_from_optional<T, U>>,
        not_type<assigns_from_optional<T, U>>>
    operator=(optional<U>&& other) {
        if (other.has_value()) {
            if (has_value()) {
                storage_.get(type_tag_v) = std::move(*other);
            } else {
                storage_.construct(type_tag_v, std::move(*other));
            }
        } else {
            storage_.reset();
        }
        return *this;
    }

    constexpr optional& operator=(nullopt_t) {
        storage_.reset();
        return *this;
    }

    // Swap.

    void swap(optional& other) noexcept(nothrow_swappable) {
        if (has_value() && other.has_value()) {
            using std::swap;
            swap(storage_.get(type_tag_v), other.storage_.get(type_tag_v));
        } else if (has_value()) {
            other.storage_.get(type_tag_v) = std::move(storage_.get(type_tag_v));
            storage_.reset();
        } else if (other.has_value()) {
            storage_.get(type_tag_v) = std::move(other.storage_.get(type_tag_v));
            other.storage_.reset();
        }
    }

    // Emplacement.

    template <typename... Args>
    constexpr emplace_constructible<Args&&...>
    emplace(Args&&... args) {
        storage_.reset();
        storage_.construct(type_tag_v, std::forward<Args>(args)...);
        return storage_.get(type_tag_v);
    }

    template <typename U, typename... Args>
    constexpr emplace_constructible<std::initializer_list<U>&, Args&&...>
    emplace(std::initializer_list<U> init_list, Args&&... args) {
        storage_.reset();
        storage_.construct(type_tag_v, init_list, std::forward<Args>(args)...);
        return storage_.get(type_tag_v);
    }

    // Reset.

    constexpr void reset() noexcept {
        storage_.reset();
    }

private:
    ::fit::internal::storage_type<T> storage_{};
};

template <typename T>
void swap(optional<T>& a, optional<T>& b) {
    a.swap(b);
}

template <typename T>
constexpr bool operator==(const optional<T>& lhs, nullopt_t) {
    return !lhs.has_value();
}
template <typename T>
constexpr bool operator!=(const optional<T>& lhs, nullopt_t) {
    return lhs.has_value();
}

template <typename T>
constexpr bool operator==(nullopt_t, const optional<T>& rhs) {
    return !rhs.has_value();
}
template <typename T>
constexpr bool operator!=(nullopt_t, const optional<T>& rhs) {
    return rhs.has_value();
}

template <typename T, typename U>
constexpr bool operator==(const optional<T>& lhs, const optional<U>& rhs) {
    return (lhs.has_value() == rhs.has_value()) && (!lhs.has_value() || *lhs == *rhs);
}
template <typename T, typename U>
constexpr bool operator!=(const optional<T>& lhs, const optional<U>& rhs) {
    return (lhs.has_value() != rhs.has_value()) || (lhs.has_value() && *lhs != *rhs);
}

template <typename T, typename U>
constexpr bool operator==(const optional<T>& lhs, const U& rhs) {
    return lhs.has_value() && *lhs == rhs;
}
template <typename T, typename U>
constexpr bool operator!=(const optional<T>& lhs, const U& rhs) {
    return !lhs.has_value() || *lhs != rhs;
}

template <typename T, typename U>
constexpr bool operator==(const T& lhs, const optional<U>& rhs) {
    return rhs.has_value() && lhs == *rhs;
}
template <typename T, typename U>
constexpr bool operator!=(const T& lhs, const optional<U>& rhs) {
    return !rhs.has_value() || lhs != *rhs;
}

} // namespace fit

#endif // LIB_FIT_OPTIONAL_H_
