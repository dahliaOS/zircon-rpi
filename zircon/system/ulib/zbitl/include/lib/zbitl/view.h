// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_ZBITL_VIEW_H_
#define LIB_ZBITL_VIEW_H_

#include <zircon/assert.h>
#include <zircon/boot/image.h>

#include <functional>
#include <optional>
#include <type_traits>
#include <variant>

#include "checking.h"
#include "storage_traits.h"

namespace zbitl {

/// The zbitl::View template class provides an error-checking container view of
/// a ZBI.  It satisfies the C++20 std::forward_range concept; it satisfies the
/// std::view concept if the Storage and Storage::error_type types support
/// constant-time copy/move/assignment.
///
/// The "error-checking view" pattern means that the container/range/view API
/// of begin() and end() iterators is supported, but when begin() or
/// iterator::operator++() encounters an error, it simply returns end() so that
/// loops terminate normally.  Thereafter, take_error() must be called to check
/// whether the loop terminated because it iterated past the last item or
/// because it encountered an error.  Once begin() has been called,
/// take_error() must be called before the View is destroyed, so no error goes
/// undetected.  Since all use of iterators updates the error state, use of any
/// zbitl::View object must be serialized and after begin() or operator++()
/// yields end(), take_error() must be checked before using begin() again.
///
/// Each time begin() is called the underlying storage is examined afresh, so
/// it's safe to reuse a zbitl::View object after changing the data.  Reducing
/// the size of the underlying storage invalidates any iterators that pointed
/// past the new end of the image.  It's simplest just to assume that changing
/// the underlying storage always invalidates all iterators.
///
/// The Storage type is some type that can be abstractly considered to have
/// non-owning "view" semantics: it doesn't hold the storage of the ZBI, it
/// just refers to it somehow.  The zbitl::View:Error type describes errors
/// encountered while iterating.  It uses the Storage::error_type type to
/// propagate errors caused by access to the underlying storage.
///
/// Usually Storage and Storage:error_type types are small and can be copied.
/// zbitl::View is move-only if Storage is move-only or if Storage::error_type
/// is move-only.  Note that copying zbitl::View copies its error-checking
/// state exactly, so if the original View needed to be checked for errors
/// before destruction then both the original and the copy need to be checked
/// before their respective destructions.  A moved-from zbitl::View can always
/// be destroyed without checking.
template <typename Storage, Checking Check = Checking::kStrict>
class View {
 public:
  using storage_type = Storage;

  View() = default;
  View(const View&) = default;
  View& operator=(const View&) = default;

  // This is almost the same as the default move behavior.  But it also
  // explicitly resets the moved-from error state to Unused so that the
  // moved-from View can be destroyed without checking it.
  View(View&& other)
      : error_(std::move(other.error_)), storage_(std::move(other.storage_)), limit_(other.limit_) {
    other.error_.template emplace<Unused>();
    other.limit_ = 0;
  }
  View& operator=(View&& other) {
    error_ = std::move(other.error_);
    other.error_.template emplace<Unused>();
    storage_ = std::move(other.storage_);
    limit_ = std::move(other.limit_);
    other.limit_ = 0;
    return *this;
  }

  template <typename... Args, typename = std::enable_if_t<sizeof...(Args)>>
  explicit View(Args&&... args) : storage_(std::forward<Args>(args)...) {}

  ~View() {
    ZX_ASSERT(std::holds_alternative<Unused>(error_) || std::holds_alternative<Taken>(error_));
  }

  /// The API details of what Storage means are delegated to the StorageTraits
  /// template; see <lib/zbitl/storage_traits.h>.
  using Traits = StorageTraits<Storage>;
  static_assert(std::is_default_constructible_v<typename Traits::error_type>);
  static_assert(std::is_copy_constructible_v<typename Traits::error_type>);
  static_assert(std::is_copy_assignable_v<typename Traits::error_type>);
  static_assert(std::is_default_constructible_v<typename Traits::payload_type>);
  static_assert(std::is_copy_constructible_v<typename Traits::payload_type>);
  static_assert(std::is_copy_assignable_v<typename Traits::payload_type>);

  /// The header is represented by an opaque type that can be dereferenced as
  /// if it were `const zbi_header_t*`, i.e. `*header` or `header->member`.
  /// Either it stores the `zbi_header_t` directly or it holds a pointer into
  /// someplace owned or viewed by the Storage object.  In the latter case,
  /// i.e. when Storage represents something already in memory, `header_type`
  /// should be no larger than a plain pointer.
  class header_type {
   public:
    header_type() = default;
    header_type(const header_type&) = default;
    header_type(header_type&&) = default;
    header_type& operator=(const header_type&) = default;
    header_type& operator=(header_type&&) = default;

    /// `*header` always copies, so lifetime of `this` doesn't matter.
    zbi_header_t operator*() const {
      if constexpr (kCopy) {
        return stored_;
      } else {
        return *stored_;
      }
    }

    /// `header->member` refers to the header in place, so never do
    /// `&header->member` but always dereference a member directly.
    const zbi_header_t* operator->() const {
      if constexpr (kCopy) {
        return &stored_;
      } else {
        return stored_;
      }
    }

   private:
    using TraitsHeader = decltype(Traits::Header(std::declval<Storage&>(), 0));
    static constexpr bool kCopy =
        std::is_same_v<TraitsHeader, std::variant<typename Traits::error_type, zbi_header_t>>;
    static constexpr bool kReference =
        std::is_same_v<TraitsHeader, std::variant<typename Traits::error_type,
                                                  std::reference_wrapper<const zbi_header_t>>>;
    static_assert(kCopy || kReference,
                  "zbitl::StorageTraits specialization's Header function returns wrong type");

    friend View;
    using HeaderStorage = std::conditional_t<kCopy, zbi_header_t, const zbi_header_t*>;
    HeaderStorage stored_;

    // This can only be used by begin(), below.
    template <typename T>
    explicit header_type(const T& header)
        : stored_([&header]() {
            if constexpr (kCopy) {
              static_assert(std::is_same_v<zbi_header_t, T>);
              return header;
            } else {
              static_assert(std::is_same_v<std::reference_wrapper<const zbi_header_t>, T>);
              return &(header.get());
            }
          }()) {}
  };

  /// The payload type is provided by the StorageTraits specialization.  It's
  /// opaque to View, but must be default-constructible, copy-constructible,
  /// and copy-assignable.  It's expected to have "view"-style semantics,
  /// i.e. be small and not own any storage itself but only refer to storage
  /// owned by the Storage object.
  using payload_type = typename Traits::payload_type;

  /// The element type is a trivial struct morally equivalent to
  /// std::pair<header_type, payload_type>.  Both member types are
  /// default-constructible, copy-constructible, and copy-assignable, so
  /// value_type as a whole is as well.
  struct value_type {
    header_type header;
    payload_type payload;
  };

  /// The Error type is returned by take_error() after begin() or an iterator
  /// operator encountered an error.  There is always a string description of
  /// the error.  Errors arising from Storage access also provide an error
  /// value defined via StorageTraits; see <lib/zbitl/storage_traits.h>.
  struct Error {
    /// A string constant describing the error.
    std::string_view zbi_error{};

    /// This is the offset into the ZBI of the item (header) at fault.  This is
    /// zero for problems with the overall container, which begin() detects.
    /// In iterator operations, it refers to the offset into the image where
    /// the item was (or should have been).
    uint32_t item_offset = 0;

    /// This reflects the underlying error from accessing the Storage object,
    /// if any.  If storage_error.has_value() is false, then the error is in
    /// the format of the contents of the ZBI, not in accessing the contents.
    std::optional<typename Traits::error_type> storage_error{};
  };

  /// Check the container for errors after using iterators.  When begin() or
  /// iterator::operator++() encounters an error, it simply returns end() so
  /// that loops terminate normally.  Thereafter, take_error() must be called
  /// to check whether the loop terminated because it iterated past the last
  /// item or because it encountered an error.  Once begin() has been called,
  /// take_error() must be called before the View is destroyed, so no error
  /// goes undetected.  After take_error() is called the error state is
  /// consumed and take_error() cannot be called again until another begin() or
  /// iterator::operator++() call has been made.
  [[nodiscard]] std::optional<Error> take_error() {
    ErrorState result{std::in_place_type<Taken>};
    error_.swap(result);
    ZX_ASSERT_MSG(!std::holds_alternative<Taken>(result),
                  "zbitl::View::take_error() was already called");
    if (std::holds_alternative<Error>(result)) {
      auto error = std::get<Error>(std::move(result));
      ZX_DEBUG_ASSERT(!error.zbi_error.empty());
      return {std::move(error)};
    }
    return {};  // No error.
  }

  /// If you explicitly don't care about any error that might have terminated
  /// the last loop early, then call ignore_error() instead of take_error().
  void ignore_error() { static_cast<void>(take_error()); }

  /// Trivial accessor for the underlying Storage (view) object.
  storage_type& storage() { return storage_; }

  class iterator {
   public:
    /// The default-constructed iterator is invalid for all uses except
    /// equality comparison.
    iterator() = default;

    iterator& operator=(const iterator&) = default;

    bool operator==(const iterator& other) const {
      return other.view_ == view_ && other.offset_ == offset_;
    }

    bool operator!=(const iterator& other) const { return !(*this == other); }

    iterator& operator++() {  // prefix
      ZX_ASSERT_MSG(view_, "operator++() on default-constructed zbitl::View::iterator");
      ZX_ASSERT_MSG(offset_ != kEnd_, "operator++() on zbitl::View::end() iterator");
      view_->Start();
      ZX_DEBUG_ASSERT(offset_ >= sizeof(zbi_header_t));
      ZX_DEBUG_ASSERT(offset_ <= view_->limit_);
      ZX_DEBUG_ASSERT(offset_ % ZBI_ALIGNMENT == 0);
      if (view_->limit_ - offset_ < sizeof(zbi_header_t)) {
        // Reached the end of the container.
        if constexpr (Check != Checking::kPermissive) {
          if (offset_ != view_->limit_) {
            Fail("container too short for next item header", {});
          }
        }
        *this = view_->end();
      } else if (auto header = Traits::Header(view_->storage(), offset_);
                 std::holds_alternative<typename Traits::error_type>(header)) {
        // Failed to read the next header.
        Fail("cannot read item header", std::get<typename Traits::error_type>(header));
      } else if (auto header_error =
                     CheckHeader<Check>(std::get<1>(header), view_->limit_ - offset_);
                 !header_error.empty()) {
        Fail(header_error, {});
      } else {
        header_ = header_type(std::move(std::get<1>(header)));
        offset_ += sizeof(zbi_header_t);
        if (auto payload = Traits::Payload(view_->storage(), offset_, header_->length);
            std::holds_alternative<typename Traits::error_type>(payload)) {
          Fail("cannot extract payload view", std::get<typename Traits::error_type>(header));
        } else {
          offset_ += ZBI_ALIGN(header_->length);
          payload_ = std::move(std::get<payload_type>(payload));
          if constexpr (Check == Checking::kCrc) {
            if (auto crc = Traits::Crc32(view_->storage(), offset_, header_.length);
                std::holds_alternative<Traits::error_type>(crc)) {
              Fail("cannot compute payload CRC32", std::get<Traits::error_type>(crc));
            }
          }
        }
      }
      return *this;
    }

    iterator operator++(int) {  // postfix
      iterator old = *this;
      ++*this;
      return old;
    }

    value_type operator*() const {
      ZX_ASSERT_MSG(view_, "operator*() on zbitl::View::end() iterator");
      return {header_, payload_};
    }

   private:
    // The default-constructed state is almost the same as the end() state:
    // nothing but operator==() should ever be called if view_ is nullptr.
    View* view_ = nullptr;

    // The offset into the ZBI of the next item's header.  This is 0 in
    // default-constructed iterators and kEnd_ in end() iterators, where
    // operator*() can never be called.  A valid non-end() iterator holds the
    // header and payload (references) of the "current" item for operator*() to
    // return, and its offset_ always looks past to the horizon.  If offset_ as
    // at the end of the container, then operator++() will yield end().
    uint32_t offset_ = 0;

    // end() uses a different offset_ value to distinguish a true end iterator
    // from a particular view from a default-constructed iterator from nowhere.
    static constexpr uint32_t kEnd_ = std::numeric_limits<uint32_t>::max();

    // These are left uninitialized until a successful increment sets them.
    // They are only examined by a dereference, which is invalid without
    // a successful increment.
    header_type header_{};
    payload_type payload_;

    // This is called only by begin() and end().
    friend class View;
    iterator(View* view, bool is_end)
        : view_(view), offset_(is_end ? kEnd_ : sizeof(zbi_header_t)) {
      ZX_DEBUG_ASSERT(view_);
      if (!is_end) {
        // The initial offset_ points past the container header, to the first
        // item. The first increment reaches end() or makes the iterator valid.
        ++*this;
      }
    }

    void Fail(std::string_view sv, std::optional<typename Traits::error_type> error) {
      view_->Fail({sv, offset_, std::move(error)});
      *this = view_->end();
    }
  };

  std::optional<zbi_header_t> container_header(bool noerror = false) {
    auto capacity_error = Traits::Capacity(storage());
    if (std::holds_alternative<typename Traits::error_type>(capacity_error)) {
      Fail(
          {
              "cannot determine storage capacity",
              0,
              std::get<typename Traits::error_type>(capacity_error),
          },
          noerror);
      return {};
    }
    uint32_t capacity = std::get<uint32_t>(capacity_error);

    // Minimal sanity check before trying to read.
    if (capacity < sizeof(zbi_header_t)) {
      Fail(
          {
              "storage capacity too small for ZBI container header",
              capacity,
              {},
          },
          noerror);
      return {};
    }

    // Read and validate the container header.
    auto header_error = Traits::Header(storage(), 0);
    if (std::holds_alternative<typename Traits::error_type>(header_error)) {
      // Failed to read the container header.
      Fail(
          {
              "cannot read container header",
              0,
              std::get<typename Traits::error_type>(header_error),
          },
          noerror);
      return {};
    }

    const header_type header(std::move(std::get<1>(header_error)));

    auto check_error = CheckHeader<Check>(*header, capacity);
    if (!check_error.empty()) {
      Fail({check_error, 0, {}}, noerror);
      return {};
    }

    if (header->flags & ZBI_FLAG_CRC32) {
      Fail({"container header has CRC32 flag", 0, {}}, noerror);
      return {};
    }

    if (header->length % ZBI_ALIGNMENT != 0) {
      Fail({"container header has misaligned length", 0, {}}, noerror);
      return {};
    }

    return *header;
  }

  /// After calling begin(), it's mandatory to call take_error() before
  /// destroying the View object.  An iteration that encounters an error will
  /// simply end early, i.e. begin() or operator++() will yield an iterator
  /// that equals end().  At the end of a loop, call take_error() to check for
  /// errors.  It's also acceptable to call take_error() during and iteration
  /// that hasn't reached end() yet, but it cannot be called again before the
  /// next begin() or operator++() call.

  iterator begin() {
    Start();
    if (auto header = container_header()) {
      // The container's "payload" is all the items.  Don't scan past it.
      limit_ = static_cast<uint32_t>(sizeof(zbi_header_t) + header->length);
      return {this, false};
    }
    limit_ = 0;  // Reset from past uses.
    return end();
  }

  iterator end() { return {this, true}; }

  size_t size_bytes() {
    if (std::holds_alternative<Unused>(error_)) {
      ZX_ASSERT(limit_ == 0);

      // Taking the size before doing begin() takes extra work.
      auto capacity_error = Traits::Capacity(storage());
      if (std::holds_alternative<uint32_t>(capacity_error)) {
        uint32_t capacity = std::get<uint32_t>(capacity_error);
        if (capacity >= sizeof(zbi_header_t)) {
          auto header_error = Traits::Header(storage(), 0);
          if (std::holds_alternative<1>(header_error)) {
            const header_type header(std::move(std::get<1>(header_error)));
            if (header->length <= capacity - sizeof(zbi_header_t)) {
              return sizeof(zbi_header_t) + header->length;
            }
          }
        }
      }
    }
    return limit_;
  }

 private:
  struct Unused final {};
  struct NoError final {};
  struct Taken final {};
  using ErrorState = std::variant<Unused, NoError, Error, Taken>;

  ErrorState error_;
  storage_type storage_;
  uint32_t limit_ = 0;

  void Start() {
    ZX_ASSERT_MSG(!std::holds_alternative<Error>(error_),
                  "zbitl:View iterators used without taking prior error");
    if (std::holds_alternative<Unused>(error_)) {
      error_.template emplace<NoError>();
    }
  }

  void Fail(Error error, bool noerror = false) {
    if (noerror) {
      return;
    }
    ZX_DEBUG_ASSERT_MSG(!std::holds_alternative<Unused>(error_),
                        "Fail in Unused: missing zbitl::View::Start() call?");
    ZX_DEBUG_ASSERT_MSG(!std::holds_alternative<Error>(error_),
                        "Fail in Error: missing zbitl::View::Start() call?");
    error_ = std::move(error);
  }
};

// Deduction guide: View v(T{}) instantiates View<T>.
template <typename Storage>
explicit View(Storage) -> View<Storage>;

// A shorthand for permissive checking.
template <typename Storage>
using PermissiveView = View<Storage, Checking::kPermissive>;

// A shorthand for CRC checking.
template <typename Storage>
using CrcCheckingView = View<Storage, Checking::kCrc>;

}  // namespace zbitl

#endif  // LIB_ZBITL_VIEW_H_
