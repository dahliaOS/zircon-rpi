// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIT_STORAGE_INTERNAL_H_
#define LIB_FIT_STORAGE_INTERNAL_H_

#include <limits>
#include <type_traits>

namespace fit {
namespace internal {

template <typename T>
struct type_tag {};

template <typename T>
constexpr auto type_tag_v = type_tag<T>{};

// Type tag to select trivial initialization.
enum trivial_init_t { trivial_init_v };

// Type tag to select unconditional construction of type T.
template <typename T>
struct always_init {};

// Constant expression to select unconditional construction of type T.
template <typename T>
constexpr auto always_init_v = always_init<T>{};

// Type tag to select conditional construction.
enum maybe_init_t { maybe_init_v };

// Represents the pair (T, Index) in the type system.
template <typename T, size_t Index>
struct type_index {};

// Utility that evaluates to true when all of its truth-like parameter types
// evaluate to true.
template <typename... Ts>
struct and_type : std::true_type {};
template <typename A>
struct and_type<A> : A {};
template <typename A, typename B>
struct and_type<A, B> : std::integral_constant<bool, A::value && B::value> {};
template <typename A, typename B, typename... Rest>
struct and_type<A, B, Rest...> : and_type<A, and_type<B, Rest...>> {};

template <typename... Ts>
constexpr bool and_v = and_type<Ts...>::value;

// Utility that evaluates to true when any of its truth-like parameter types
// evaluates to true.
template <typename... T>
struct or_type : std::true_type {};
template <typename A>
struct or_type<A> : A {};
template <typename A, typename B>
struct or_type<A, B> : std::integral_constant<bool, A::value || B::value> {};
template <typename A, typename B, typename... Rest>
struct or_type<A, B, Rest...> : or_type<A, or_type<B, Rest...>> {};

template <typename... Ts>
constexpr bool or_v = or_type<Ts...>::value;

// Utility type that negates its truth-like parameter type.
template <typename T>
struct not_type : std::integral_constant<bool, !T::value> {};

template <typename T>
constexpr bool not_v = not_type<T>::value;

// Evaluates to true of every element type of Ts is trivially destructible.
template <typename... Ts>
constexpr bool is_trivial_v = and_type<std::is_trivially_destructible<Ts>...>::value;

// Constants denoting whether or not a type is trivially destructible.
enum class storage_class {
    trivial,
    non_trivial,
};

// Evaluates to the most restrictive storage class that can apply to all element
// types of Ts.
template <typename... Ts>
constexpr storage_class storage_class_v =
    is_trivial_v<Ts...> ? storage_class::trivial : storage_class::non_trivial;

// Base type for lazy-initialized union storage types. This type implements a
// recursive union of the element types in Ts. Specializations handle the
// recursive and terminal cases, and the different storage requirements for
// trivially and non-trivially destructible types.
template <storage_class, typename... Ts>
union storage_base;

template <typename T, size_t Index>
union storage_base<storage_class::non_trivial, type_index<T, Index>> {
    // Non-trivial destructor.
    ~storage_base() {}

    // Trivial initialization constructor.
    storage_base(trivial_init_t)
        : dummy{} {}

    // Initializes the storage corresponding to this element's type.
    template <typename Storage, typename... Args>
    storage_base(always_init<T>, Storage* storage, Args&&... args)
        : value{std::forward<Args>(args)...} {
        storage->set_index(Index);
    }

    // Terminal constructor for recursive storage element selection.
    template <typename U, typename Storage, typename... Args>
    storage_base(always_init<U>, Storage* storage, Args&&...)
        : dummy{} {
        storage->set_empty();
    }

    void construct_at(size_t index, const storage_base& other) {
        if (index == Index) {
            new (&value) T{other.value};
        }
    }
    void construct_at(size_t index, storage_base&& other) {
        if (index == Index) {
            new (&value) T{std::move(other.value)};
        }
    }

    template <typename... Args>
    size_t construct(type_tag<T>, Args&&... args) {
        new (&value) T{std::forward<Args>(args)...};
        return Index;
    }

    template <typename Storage>
    void reset(Storage* storage) {
        if (storage->index() == Index) {
            value.~T();
            storage->set_empty();
        }
    }

    T& get(type_tag<T>) {
        return value;
    }
    const T& get(type_tag<T>) const {
        return value;
    }

    unsigned char dummy;
    T value;
};

template <typename T, size_t Index>
union storage_base<storage_class::trivial, type_index<T, Index>> {
    // Trivial destructor.
    ~storage_base() = default;

    // Trivial initialization constructor.
    constexpr storage_base(trivial_init_t)
        : dummy{} {}

    // Initializes the storage corresponding to this element's type.
    template <typename Storage, typename... Args>
    constexpr storage_base(always_init<T>, Storage* storage, Args&&... args)
        : value{std::forward<Args>(args)...} {
        storage->set_index(Index);
    }

    // Terminal constructor for recursive storage element selection.
    template <typename U, typename Storage, typename... Args>
    constexpr storage_base(always_init<U>, Storage* storage, Args&&...)
        : dummy{} {
        storage->set_empty();
    }

    constexpr void construct_at(size_t index, const storage_base& other) {
        if (index == Index) {
            new (&value) T{other.value};
        }
    }
    constexpr void construct_at(size_t index, storage_base&& other) {
        if (index == Index) {
            new (&value) T{std::move(other.value)};
        }
    }

    template <typename... Args>
    constexpr size_t construct(type_tag<T>, Args&&... args) {
        new (&value) T{std::forward<Args>(args)...};
        return Index;
    }

    template <typename Storage>
    constexpr void reset(Storage* storage) {
        storage->set_empty();
    }

    constexpr T& get(type_tag<T>) {
        return value;
    }
    constexpr const T& get(type_tag<T>) const {
        return value;
    }

    unsigned char dummy;
    T value;
};

template <typename T, size_t Index, typename... Ts, size_t... Indices>
union storage_base<storage_class::non_trivial, type_index<T, Index>, type_index<Ts, Indices>...> {
    // Non-trivial destructor.
    ~storage_base() {}

    // Trivial initialization constructor.
    storage_base(trivial_init_t)
        : dummy{} {}

    // Initializes the storage corresponding to this element's type.
    template <typename Storage, typename... Args>
    storage_base(always_init<T>, Storage* storage, Args&&... args)
        : value{std::forward<Args>(args)...} {
        storage->set_index(Index);
    }

    // Constructor for recursive storage element selection.
    template <typename U, typename Storage, typename... Args>
    storage_base(always_init<U> type, Storage* storage, Args&&... args)
        : rest{type, storage, std::forward<Args>(args)...} {
    }

    void construct_at(size_t index, const storage_base& other) {
        if (index == Index) {
            new (&value) T{other.value};
        } else {
            rest.construct_at(index, other.rest);
        }
    }
    void construct_at(size_t index, storage_base&& other) {
        if (index == Index) {
            new (&value) T{std::move(other.value)};
        } else {
            rest.construct_at(index, std::move(other.rest));
        }
    }

    template <typename... Args>
    size_t construct(type_tag<T>, Args&&... args) {
        new (&value) T{std::forward<Args>(args)...};
        return Index;
    }
    template <typename U, typename... Args>
    size_t construct(type_tag<U>, Args&&... args) {
        return rest.construct(type_tag_v<U>, std::forward<Args>(args)...);
    }

    template <typename Storage>
    void reset(Storage* storage) {
        if (storage->index() == Index) {
            value.~T();
            storage->set_empty();
        } else {
            rest.reset(storage);
        }
    }

    T& get(type_tag<T>) {
        return value;
    }
    template <typename U>
    U& get(type_tag<U>) {
        return rest.get(type_tag_v<U>);
    }
    const T& get(type_tag<T>) const {
        return value;
    }
    template <typename U>
    const U& get(type_tag<U>) const {
        return rest.get(type_tag_v<U>);
    }

    unsigned char dummy;
    T value;
    storage_base<storage_class::non_trivial, type_index<Ts, Indices>...> rest;
};

template <typename T, size_t Index, typename... Ts, size_t... Indices>
union storage_base<storage_class::trivial, type_index<T, Index>, type_index<Ts, Indices>...> {
    // Trivial destructor.
    ~storage_base() = default;

    // Trivial initialization constructor.
    constexpr storage_base(trivial_init_t)
        : dummy{} {}

    constexpr void construct_at(size_t index, const storage_base& other) {
        if (index == Index) {
            new (&value) T{other.value};
        } else {
            rest.construct_at(index, other.rest);
        }
    }
    constexpr void construct_at(size_t index, storage_base&& other) {
        if (index == Index) {
            new (&value) T{std::move(other.value)};
        } else {
            rest.construct_at(index, std::move(other.rest));
        }
    }

    template <typename... Args>
    constexpr size_t construct(type_tag<T>, Args&&... args) {
        new (&value) T{std::forward<Args>(args)...};
        return Index;
    }
    template <typename U, typename... Args>
    constexpr size_t construct(type_tag<U>, Args&&... args) {
        return rest.construct(type_tag_v<U>, std::forward<Args>(args)...);
    }

    template <typename Storage>
    constexpr void reset(Storage* storage) {
        storage->set_empty();
    }

    constexpr T& get(type_tag<T>) {
        return value;
    }
    template <typename U>
    constexpr U& get(type_tag<U>) {
        return rest.get(type_tag_v<U>);
    }
    constexpr const T& get(type_tag<T>) const {
        return value;
    }
    template <typename U>
    constexpr const U& get(type_tag<U>) const {
        return rest.get(type_tag_v<U>);
    }

    unsigned char dummy;
    T value;
    storage_base<storage_class::trivial, type_index<Ts, Indices>...> rest;
};

template <size_t element_count, typename Enabled = void>
struct index_type {
    using type = size_t;
};
template <size_t element_count>
struct index_type<element_count,
                  std::enable_if<(element_count < std::numeric_limits<uint8_t>::max())>> {
    using type = uint8_t;
};
template <size_t element_count>
struct index_type<element_count,
                  std::enable_if<(element_count >= std::numeric_limits<uint8_t>::max() &&
                                  element_count < std::numeric_limits<uint16_t>::max())>> {
    using type = uint16_t;
};
template <size_t element_count>
struct index_type<element_count,
                  std::enable_if<(element_count >= std::numeric_limits<uint16_t>::max() &&
                                  element_count < std::numeric_limits<uint32_t>::max())>> {
    using type = uint32_t;
};

template <typename... Ts>
class storage;

template <typename... Ts, size_t... Indices>
class storage<type_index<Ts, Indices>...> {
public:
    using index_type = typename ::fit::internal::index_type<sizeof...(Ts)>::type;
    enum : index_type { empty_index = std::numeric_limits<index_type>::max() };

    constexpr storage()
        : base_{trivial_init_v} {}

    template <typename T, typename U>
    constexpr storage(always_init<T>, U&& value)
        : base_{trivial_init_v} {
        index_ = base_.construct(type_tag_v<T>, std::forward<U>(value));
    }
    constexpr storage(maybe_init_t, const storage& other)
        : index_{other.index()}, base_{trivial_init_v} {
        base_.construct_at(other.index(), other.base_);
    }
    constexpr storage(maybe_init_t, storage&& other)
        : index_{other.index()}, base_{trivial_init_v} {
        base_.construct_at(other.index(), std::move(other.base_));
    }

    constexpr index_type index() const { return index_; }
    constexpr bool has_value() const { return index() != empty_index; }

    template <typename T>
    constexpr auto& get(type_tag<T>) {
        return base_.get(type_tag_v<T>);
    }
    template <typename T>
    constexpr const auto& get(type_tag<T>) const {
        return base_.get(type_tag_v<T>);
    }

    template <typename T, typename... Args>
    constexpr void construct(type_tag<T>, Args&&... args) {
        index_ = base_.construct(type_tag_v<T>, std::forward<Args>(args)...);
    }

    constexpr void reset() {
        base_.reset(this);
    }

private:
    template <storage_class, typename...>
    friend union storage_base;

    constexpr void set_index(index_type index) { index_ = index; }
    constexpr void set_empty() { index_ = empty_index; }

    index_type index_{empty_index};
    storage_base<storage_class_v<Ts...>, type_index<Ts, Indices>...> base_;
};

template <typename... Ts, size_t... Indices>
constexpr auto make_storage(std::index_sequence<Indices...>) {
    return storage<type_index<Ts, Indices>...>{};
}

template <typename... Ts>
using storage_type = decltype(make_storage<Ts...>(std::make_index_sequence<sizeof...(Ts)>{}));

} // namespace internal
} // namespace fit

#endif // LIB_FIT_STORAGE_INTERNAL_H_
