// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zx/time.h>

#include <zircon/errors.h>
#include <zircon/syscalls.h>
#include <zircon/time.h>
#include <zircon/types.h>

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <variant>
#include <vector>

#include <rapidjson/reader.h>

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
    std::uniform_int_distribution<Base<T>> distribution{
        std::numeric_limits<Base<T>>::lowest(),
        std::numeric_limits<Base<T>>::max()};
    return static_cast<T>(distribution(generator_));
  }

  template <typename T>
  T GetUniform(T min, T max) {
    std::uniform_int_distribution<Base<T>> distribution{
        static_cast<Base<T>>(min), static_cast<Base<T>>(max)};
    return static_cast<T>(distribution(generator_));
  }

  template <typename T>
  T SelectUniform(std::initializer_list<T> il) {
    std::uniform_int_distribution<std::size_t> distribution{
        0, il.size() > 0 ? il.size() - 1 : 0};
    return il.begin()[distribution(generator_)];
  }

 private:
  std::mt19937_64 generator_{std::random_device{}()};
};

class Worker;

// Abstract interface for actions that worker threads can perform.
struct Action {
  virtual ~Action() = default;

  // Performs the this action by/on the given worker.
  virtual void Perform(Worker* worker) = 0;
};

class Worker {
 public:
  Worker(Worker&&) = delete;
  Worker& operator=(Worker&&) = delete;
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  ~Worker() = default;

  static std::thread Create(std::vector<std::unique_ptr<Action>> actions) {
    return std::thread{&Worker::Run, new Worker{std::move(actions)}};
  }

  // Sleeps the worker for the given duration. Returns early if the termination
  // flag is set.
  void Sleep(std::chrono::nanoseconds duration_ns) {
    NullLock lock;
    terminate_condition_.wait_for(lock, duration_ns,
                                  []() { return should_terminate(); });
  }

  // Spins the worker for the given duration. Returns early if the termination
  // flag is set.
  void Spin(std::chrono::nanoseconds duration_ns) {
    const auto end_time = std::chrono::steady_clock::now() + duration_ns;
    while (std::chrono::steady_clock::now() < end_time && !should_terminate()) {
      spin_work_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // Yields the worker.
  void Yield() {
    zx::nanosleep(zx::time{0});
  }

  static void TerminateAll() {
    terminate_flag_ = true;
    terminate_condition_.notify_all();
  }

 private:
  Worker(std::vector<std::unique_ptr<Action>> actions)
      : id_{thread_counter_++}, actions_{std::move(actions)} {}

  void Run() {
    std::cout << "Starting up worker " << id_ << std::endl;

    while (!terminate_flag_.load(std::memory_order_relaxed)) {
      for (auto& action : actions_) {
        action->Perform(this);
      }
    }

    std::cout << "Terminating worker " << id_ << std::endl;
  }

  int id_;
  std::vector<std::unique_ptr<Action>> actions_;

  struct NullLock {
    void lock() {}
    void unlock() {}
  };

  static bool should_terminate() {
    return terminate_flag_.load(std::memory_order_relaxed);
  }

  inline static std::atomic<int> thread_counter_{0};
  inline static std::atomic<bool> terminate_flag_{false};
  inline static std::condition_variable_any terminate_condition_{};

  inline static std::atomic<uint64_t> spin_work_{0};
};

struct SleepAction : Action {
  SleepAction(std::chrono::nanoseconds duration_ns)
      : duration_ns{duration_ns} {}
  void Perform(Worker* worker) override { worker->Sleep(duration_ns); }
  const std::chrono::nanoseconds duration_ns;
};

struct SpinAction : Action {
  SpinAction(std::chrono::nanoseconds duration_ns) : duration_ns{duration_ns} {}
  void Perform(Worker* worker) override { worker->Spin(duration_ns); }
  const std::chrono::nanoseconds duration_ns;
};

struct YieldAction : Action {
  void Perform(Worker* worker) override { worker->Yield(); }
};

int main(int /*argc*/, char** /*argv*/) {
  Random random;

  std::vector<std::thread> workers;

  for (int i = 0; i < 10; i++) {
    const std::chrono::nanoseconds min_time = std::chrono::milliseconds{10};
    const std::chrono::nanoseconds max_time = std::chrono::milliseconds{100};
    const std::chrono::nanoseconds sleep_time{
        random.GetUniform(min_time.count(), max_time.count())};
    const std::chrono::nanoseconds spin_time_a{
        random.GetUniform(min_time.count(), max_time.count())};
    const std::chrono::nanoseconds spin_time_b{
        random.GetUniform(min_time.count(), max_time.count())};

    std::vector<std::unique_ptr<Action>> actions;
    actions.emplace_back(std::make_unique<SleepAction>(sleep_time));
    actions.emplace_back(std::make_unique<SpinAction>(spin_time_a));
    actions.emplace_back(std::make_unique<YieldAction>());
    actions.emplace_back(std::make_unique<SpinAction>(spin_time_b));

    workers.push_back(Worker::Create(std::move(actions)));
  }

  const int kSleepSeconds = 20;
  std::cout << "Sleeping for " << kSleepSeconds << " seconds..." << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds{kSleepSeconds});

  std::cout << "Terminating benchmark..." << std::endl;
  Worker::TerminateAll();

  for (auto& worker : workers) {
    worker.join();
  }

  std::cout << "Done!" << std::endl;
  return 0;
}
