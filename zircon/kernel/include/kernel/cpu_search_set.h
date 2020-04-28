// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_INCLUDE_KERNEL_CPU_SEARCH_SET_H_
#define ZIRCON_KERNEL_INCLUDE_KERNEL_CPU_SEARCH_SET_H_

#include <stddef.h>

#include <kernel/cpu.h>
#include <ktl/algorithm.h>
#include <ktl/array.h>
#include <ktl/span.h>
#include <ktl/unique_ptr.h>

// CpuSearchSet is a cache/cluster-aware search list that minimizes cache
// crossings and maximizes remote CPU access distribution when searching for a
// target CPU to place a task.
class CpuSearchSet {
 public:
  CpuSearchSet() = default;

  CpuSearchSet(const CpuSearchSet&) = delete;
  CpuSearchSet& operator=(const CpuSearchSet&) = delete;

  // Entry type for the list of CPUs.
  struct Entry {
    cpu_num_t cpu;
  };

  // Returns an iterator over the CPU search list. Forward iteration produces
  // entries in order of decreasing preference (i.e. earlier entries are more
  // optimal).
  auto iterator() const { return ktl::span{ordered_cpus_.begin(), cpu_count_}; }

  // Dumps the CPU search list for this set to the debug log.
  void Dump() const;

  // Dumps the CPU clusters to the debug log.
  static void DumpClusters();

 private:
  friend struct percpu;

  // Private non-const iterator and cluster iterator.
  auto iterator() { return ktl::span{ordered_cpus_.begin(), cpu_count_}; }
  static auto cluster_iterator() { return ktl::span{clusters_.get(), cluster_count_}; }

  // Called once at percpu secondary init to compute the logical clusters from
  // the topology-derived distance map.
  static void AutoCluster(size_t cpu_count);

  // Called once per CPU at percpu secondary init to compute the unique, cache-
  // aware CPU search order for the CPUs.
  void Initialize(cpu_num_t this_cpu, size_t cpu_count);

  // Each search set is initially populated by CPU 0 so that the boot processor
  // has a valid search set during early kernel init.
  // TODO(eieio): This depends on the assumption that the boot processor is
  // always logical CPU id 0. This assumption exists in other places and may
  // need to be addressed in the future.
  size_t cpu_count_{1};
  ktl::array<Entry, SMP_MAX_CPUS> ordered_cpus_{{Entry{0}}};

  // Type representing a logical CPU cluster and its members.
  struct Cluster {
    size_t id{0};
    size_t member_count{0};
    ktl::unique_ptr<cpu_num_t[]> members{nullptr};
  };

  // Entry type for the logical CPU id to cluster map.
  struct MapEntry {
    // The cluster the logical CPU id belongs to.
    Cluster* cluster;

    // The index of the logical CPU in the Cluster::members list.
    size_t index;
  };

  // The list of logical clusters computed by auto-clustering.
  inline static size_t cluster_count_{0};
  inline static ktl::unique_ptr<Cluster[]> clusters_{nullptr};


  // Map from logical CPU id to logical cluster.
  inline static ktl::array<MapEntry, SMP_MAX_CPUS> cpu_to_cluster_map_{};
};

#endif  // ZIRCON_KERNEL_INCLUDE_KERNEL_CPU_SEARCH_SET_H_
