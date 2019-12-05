// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <random>

#include <benchmark/benchmark.h>

//
// Benchmark to evaluate various LRU edge cache approaches.
//
// This benchmark may be compiled on the host with the following command:
//
//   clang++ -std=c++17 -pthread -latomic graphbench.cc -lbenchmark -o bench
//
// This assumes google/benchmark is installed in the system path.
//

namespace {

uint64_t CurrentTime() {
  if constexpr (std::chrono::high_resolution_clock::is_steady) {
    const auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  } else {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  }
}

class Random {
 public:
  Random() = default;

  template <typename T, typename = void>
  struct BaseType {
    using Type = T;
  };
  template <typename Enum>
  struct BaseType<Enum, std::enable_if_t<std::is_enum<Enum>::value>> {
    using Type = std::underlying_type_t<Enum>;
  };
  template <typename T>
  using Base = typename BaseType<T>::Type;

  template <typename T>
  T GetUniform() {
    std::uniform_int_distribution<Base<T>> distribution{std::numeric_limits<Base<T>>::lowest(),
                                                        std::numeric_limits<Base<T>>::max()};
    return static_cast<T>(distribution(generator_));
  }

  template <typename T>
  T GetUniform(T min, T max) {
    std::uniform_int_distribution<Base<T>> distribution{static_cast<Base<T>>(min),
                                                        static_cast<Base<T>>(max)};
    return static_cast<T>(distribution(generator_));
  }

  template <typename T>
  T SelectUniform(std::initializer_list<T> il) {
    std::uniform_int_distribution<std::size_t> distribution{0, il.size() > 0 ? il.size() - 1 : 0};
    return il.begin()[distribution(generator_)];
  }

  float GetNormal(float mean, float standard_deviation) {
    std::normal_distribution distribution{mean, standard_deviation};
    return distribution(generator_);
  }

 private:
  std::mt19937_64 generator_{std::random_device{}()};
};

static constexpr size_t NextPrime(size_t n) {
  if (n < (1 << 2)) {
    return 7;
  } else if (n < (1 << 3)) {
    return 11;
  } else if (n < (1 << 4)) {
    return 23;
  } else if (n < (1 << 5)) {
    return 53;
  } else if (n < (1 << 6)) {
    return 97;
  } else if (n < (1 << 7)) {
    return 193;
  } else if (n < (1 << 8)) {
    return 389;
  } else if (n < (1 << 9)) {
    return 769;
  } else if (n < (1 << 10)) {
    return 1543;
  } else {
    __builtin_abort();  // The input exceeds the size of this prime table.
  }
}

template <size_t Size, bool Prime = true, bool OpenAddressing = true>
class Node128BitAtomic {
 public:
  Node128BitAtomic() = default;

  Node128BitAtomic(const Node128BitAtomic&) = delete;
  Node128BitAtomic& operator=(const Node128BitAtomic&) = delete;

  void AddEdge(uint64_t koid, uint64_t now) {
    AtomicEntry* target_entry = nullptr;
    Entry target_value{0, std::numeric_limits<uint64_t>::max()};

    for (size_t i = 0; i < kEntryCount; i++) {
      AtomicEntry& entry = GetEntry(koid, i);
      Entry value = entry.load(std::memory_order_relaxed);
      if (value.koid == koid) {
        target_entry = &entry;
        target_value = value;
        break;
      }
      if (value.timestamp < target_value.timestamp) {
        target_entry = &entry;
        target_value = value;
      }
    }

    if (target_entry) {
      while (!target_entry->compare_exchange_weak(
          target_value, {koid, now}, std::memory_order_relaxed, std::memory_order_relaxed)) {
        if (target_value.timestamp > now) {
          break;
        }
      }
    }
  }

 private:
  struct alignas(alignof(__int128)) Entry {
    uint64_t koid{0};
    uint64_t timestamp{0};
  };
  static_assert(sizeof(Entry) == sizeof(__int128));
  static_assert(alignof(Entry) == alignof(__int128));

  using AtomicEntry = std::atomic<Entry>;

  // Selects and entry using open addressing and linear probing.
  AtomicEntry& GetEntry(uint64_t koid, size_t offset) {
    const size_t index = OpenAddressing ? (koid + offset) % kEntryCount : offset;
    return entries_[index];
  }

  static constexpr auto kEntryCount = Prime ? NextPrime(Size) : Size;

  std::array<AtomicEntry, kEntryCount> entries_{};
};

template <size_t Size, bool Prime = true, bool OpenAddressing = true>
class Node64BitAtomic {
 public:
  Node64BitAtomic() = default;

  Node64BitAtomic(const Node64BitAtomic&) = delete;
  Node64BitAtomic& operator=(const Node64BitAtomic&) = delete;

  void AddEdge(uint32_t koid, uint32_t now) {
    AtomicEntry* target_entry = nullptr;
    Entry target_value{0, std::numeric_limits<uint32_t>::max()};

    for (size_t i = 0; i < kEntryCount; i++) {
      AtomicEntry& entry = GetEntry(koid, i);
      Entry value = entry.load(std::memory_order_relaxed);
      if (value.koid == koid) {
        target_entry = &entry;
        target_value = value;
        break;
      }
      if (value.timestamp < target_value.timestamp) {
        target_entry = &entry;
        target_value = value;
      }
    }

    if (target_entry) {
      while (!target_entry->compare_exchange_weak(
          target_value, {koid, now}, std::memory_order_relaxed, std::memory_order_relaxed)) {
        if (target_value.timestamp > now) {
          break;
        }
      }
    }
  }

 private:
  struct alignas(alignof(uint64_t)) Entry {
    uint32_t koid{0};
    uint32_t timestamp{0};
  };
  static_assert(sizeof(Entry) == sizeof(uint64_t));
  static_assert(alignof(Entry) == alignof(uint64_t));

  using AtomicEntry = std::atomic<Entry>;

  // Selects and entry using open addressing and linear probing.
  AtomicEntry& GetEntry(uint32_t koid, size_t offset) {
    const size_t index = OpenAddressing ? (koid + offset) % kEntryCount : offset;
    return entries_[index];
  }

  static constexpr auto kEntryCount = Prime ? NextPrime(Size) : Size;

  std::array<AtomicEntry, kEntryCount> entries_{};
};

template <size_t Size, bool Prime = true, bool OpenAddressing = true>
class Node128BitMutex {
 public:
  Node128BitMutex() = default;

  Node128BitMutex(const Node128BitMutex&) = delete;
  Node128BitMutex& operator=(const Node128BitMutex&) = delete;

  void AddEdge(uint64_t koid, uint64_t now) {
    std::lock_guard<std::mutex> guard{lock_};

    Entry* target_entry = nullptr;
    for (size_t i = 0; i < kEntryCount; i++) {
      Entry& entry = GetEntry(koid, i);
      if (entry.koid == koid) {
        target_entry = &entry;
        break;
      }
      if (!target_entry || entry.timestamp < target_entry->timestamp) {
        target_entry = &entry;
      }
    }

    if (target_entry) {
      *target_entry = {koid, now};
    }
  }

 private:
  struct alignas(alignof(__int128)) Entry {
    uint64_t koid{0};
    uint64_t timestamp{0};
  };
  static_assert(sizeof(Entry) == sizeof(__int128));
  static_assert(alignof(Entry) == alignof(__int128));

  // Selects and entry using open addressing and linear probing.
  Entry& GetEntry(uint64_t koid, size_t offset) {
    const size_t index = OpenAddressing ? (koid + offset) % kEntryCount : offset;
    return entries_[index];
  }

  static constexpr auto kEntryCount = Prime ? NextPrime(Size) : Size;

  std::mutex lock_;
  std::array<Entry, kEntryCount> entries_{};
};

}  // anonymous namespace

template <typename Node, size_t kTimeShift = 0>
static void BM_BaselineUniform(benchmark::State& state) {
  static Node node;
  Random random;

  while (state.KeepRunning()) {
    state.PauseTiming();
    const uint64_t koid = random.GetUniform(1, 1024);
    const uint64_t now = CurrentTime() >> kTimeShift;
    state.ResumeTiming();

    node.AddEdge(koid, now);
  }

  state.SetItemsProcessed(state.iterations());
}

template <typename Node, size_t kTimeShift = 0>
static void BM_BaselineNormal(benchmark::State& state) {
  static Node node;
  Random random;

  while (state.KeepRunning()) {
    state.PauseTiming();
    const uint64_t koid = std::round(random.GetNormal(1024, 32));
    const uint64_t now = CurrentTime() >> kTimeShift;
    state.ResumeTiming();

    node.AddEdge(koid, now);
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<4, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<4, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<4, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<4, true, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<8, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<8, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<8, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<8, true, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<16, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<16, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<16, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<16, true, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<32, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<32, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<32, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<32, true, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<64, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<64, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<64, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitAtomic<64, true, true>)->Threads(1)->Threads(8);

BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<4, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<4, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<4, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<4, true, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<8, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<8, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<8, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<8, true, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<16, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<16, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<16, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<16, true, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<32, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<32, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<32, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<32, true, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<64, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<64, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<64, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node64BitAtomic<64, true, true>)->Threads(1)->Threads(8);

BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<4, false, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<4, false, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<4, true, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<4, true, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<8, false, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<8, false, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<8, true, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<8, true, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<16, false, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<16, false, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<16, true, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<16, true, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<32, false, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<32, false, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<32, true, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<32, true, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<64, false, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<64, false, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<64, true, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineUniform, Node128BitMutex<64, true, true>, 24)->Threads(1)->Threads(8);

BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<4, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<4, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<4, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<4, true, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<8, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<8, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<8, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<8, true, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<16, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<16, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<16, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<16, true, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<32, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<32, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<32, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<32, true, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<64, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<64, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<64, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitAtomic<64, true, true>)->Threads(1)->Threads(8);

BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<4, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<4, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<4, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<4, true, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<8, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<8, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<8, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<8, true, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<16, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<16, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<16, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<16, true, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<32, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<32, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<32, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<32, true, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<64, false, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<64, false, true>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<64, true, false>)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node64BitAtomic<64, true, true>)->Threads(1)->Threads(8);

BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<4, false, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<4, false, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<4, true, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<4, true, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<8, false, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<8, false, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<8, true, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<8, true, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<16, false, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<16, false, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<16, true, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<16, true, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<32, false, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<32, false, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<32, true, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<32, true, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<64, false, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<64, false, true>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<64, true, false>, 24)->Threads(1)->Threads(8);
BENCHMARK_TEMPLATE(BM_BaselineNormal, Node128BitMutex<64, true, true>, 24)->Threads(1)->Threads(8);

BENCHMARK_MAIN();
