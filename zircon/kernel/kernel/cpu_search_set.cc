// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "kernel/cpu_search_set.h"

#include <assert.h>
#include <debug.h>
#include <inttypes.h>
#include <stddef.h>

#include <kernel/cpu_distance_map.h>
#include <ktl/algorithm.h>
#include <ktl/span.h>
#include <ktl/unique_ptr.h>

namespace {

// Utility type compute CPU clusters using a disjoint set structure.
class ClusterMap {
 public:
  ClusterMap() = default;

  ClusterMap(const ClusterMap&) = delete;
  ClusterMap& operator=(const ClusterMap&) = delete;
  ClusterMap(ClusterMap&&) = default;
  ClusterMap& operator=(ClusterMap&&) = default;

  explicit operator bool() const { return bool{elements_}; }

  // Creates a ClusterMap with the given number of CPUs, with each CPU initially
  // in its own cluster.
  static ClusterMap Create(size_t element_count) {
    fbl::AllocChecker checker;
    ktl::unique_ptr<cpu_num_t[]> elements{new (&checker) cpu_num_t[element_count]};
    if (!checker.check()) {
      dprintf(ALWAYS, "Failed to allocate cluster map!\n");
      return {};
    }

    for (cpu_num_t i = 0; i < element_count; i++) {
      elements[i] = i;
    }
    return {element_count, ktl::move(elements)};
  }

  // Returns an iterator over the elements of the disjoint set structure.
  auto iterator() { return ktl::span{elements_.get(), element_count_}; }
  auto begin() { return iterator().begin(); }
  auto end() { return iterator().end(); }

  cpu_num_t operator[](size_t index) const {
    DEBUG_ASSERT(index < element_count_);
    return elements_[index];
  }

  cpu_num_t FindSet(cpu_num_t node) {
    DEBUG_ASSERT(node < element_count_);
    while (true) {
      cpu_num_t parent = elements_[node];
      cpu_num_t grandparent = elements_[parent];
      if (parent == grandparent) {
        return parent;
      }
      elements_[node] = grandparent;
      node = parent;
    }
  }

  void UnionSets(cpu_num_t a, cpu_num_t b) {
    DEBUG_ASSERT(a < element_count_);
    DEBUG_ASSERT(b < element_count_);
    while (true) {
      cpu_num_t root_a = FindSet(a);
      cpu_num_t root_b = FindSet(b);

      if (root_a < root_b) {
        elements_[root_b] = root_a;
      } else if (root_a > root_b) {
        elements_[root_a] = root_b;
      } else {
        return;
      }
    }
  }

  size_t ClusterCount() const {
    size_t count = 0;
    for (size_t i = 0; i < element_count_; i++) {
      if (elements_[i] == i) {
        count++;
      }
    }
    return count;
  }

  size_t MemberCount(cpu_num_t root) {
    size_t count = 0;
    for (cpu_num_t i = 0; i < element_count_; i++) {
      if (FindSet(i) == root) {
        count++;
      }
    }
    return count;
  }

 private:
  ClusterMap(size_t element_count, ktl::unique_ptr<cpu_num_t[]> elements)
      : element_count_{element_count}, elements_{ktl::move(elements)} {}

  size_t element_count_;
  ktl::unique_ptr<cpu_num_t[]> elements_;
};

}  // anonymous namespace

void CpuSearchSet::AutoCluster(size_t cpu_count) {
  ClusterMap cluster_map = ClusterMap::Create(cpu_count);
  ASSERT(cluster_map);

  // For each CPU, perform a single level of agglomerative clustering, joining
  // CPUs with the same minimum distances.
  const auto& map = CpuDistanceMap::Get();
  for (cpu_num_t i = 0; i < cpu_count; i++) {
    // Comparator to determine the CPU with the minimum distance that is not the
    // same CPU.
    const auto comparator = [i, &map](cpu_num_t a, cpu_num_t b) {
      if (a == i) {
        return false;
      }
      if (b == i) {
        return true;
      }
      return map[{i, a}] < map[{i, b}];
    };
    const cpu_num_t j = *ktl::min_element(cluster_map.begin(), cluster_map.end(), comparator);
    cluster_map.UnionSets(i, j);
  }

  // Allocate an array of Cluster structures for each of the computed clusters.
  fbl::AllocChecker checker;
  cluster_count_ = cluster_map.ClusterCount();
  clusters_.reset(new (&checker) Cluster[cluster_count_]);
  ASSERT(checker.check());

  // Fill in the Cluster structures and CPU-to-cluster map.
  size_t cluster_index = 0;
  for (cpu_num_t i = 0; i < cpu_count; i++) {
    const size_t member_count = cluster_map.MemberCount(i);
    if (member_count != 0) {
      Cluster& cluster = clusters_[cluster_index];
      cluster.id = cluster_index;
      cluster.member_count = member_count;
      cluster.members.reset(new (&checker) cpu_num_t[member_count]);
      ASSERT(checker.check());

      size_t member_index = 0;
      for (cpu_num_t j = 0; j < cpu_count; j++) {
        if (cluster_map[j] == i) {
          cluster.members[member_index] = j;
          cpu_to_cluster_map_[j] = {&cluster, member_index};
          member_index++;
        }
      }
      cluster_index++;
    }
  }
}

void CpuSearchSet::DumpClusters() {
  dprintf(INFO, "CPU clusters:\n");
  for (const Cluster& cluster : cluster_iterator()) {
    dprintf(INFO, "Cluster %2zu: ", cluster.id);
    for (size_t j = 0; j < cluster.member_count; j++) {
      dprintf(INFO, "%" PRIu32 "%s", cluster.members[j], j < cluster.member_count - 1 ? ", " : "");
    }
    dprintf(INFO, "\n");
  }
}

// Initializes the search set with a unique CPU order that minimizes cache level
// crossings while attempting to maximize distribution across CPUs.
void CpuSearchSet::Initialize(cpu_num_t this_cpu, size_t cpu_count) {
  // Initialize the search set in increasing ordinal order.
  cpu_count_ = cpu_count;
  for (cpu_num_t i = 0; i < cpu_count; i++) {
    ordered_cpus_[i] = {i};
  }

  // Sort the search set by three criteria in priority order:
  //   1. Cache distance from this CPU.
  //   2. Modular cluster order, offset by this CPU's cluster id.
  //   3. Modular cluster member order, offset by this CPU's id.
  //
  // These criteria result in a (sometimes) approximate Latin Square (across
  // all CPUs) with the following properties.
  //   * A CPU is always at the front of its own search list (distance is 0).
  //   * The search list is ordered by increasing cache distance.
  //   * The search order is reasonably unique compared to other CPUs.
  //
  const auto& map = CpuDistanceMap::Get();
  const auto comparator = [this_cpu, &map](const Entry& a, const Entry& b) {
    const auto distance_a = map[{this_cpu, a.cpu}];
    const auto distance_b = map[{this_cpu, b.cpu}];
    if (distance_a != distance_b) {
      return distance_a < distance_b;
    }
    const auto this_cluster = cpu_to_cluster_map_[this_cpu].cluster->id;
    const auto [a_cluster, a_index] = cpu_to_cluster_map_[a.cpu];
    const auto [b_cluster, b_index] = cpu_to_cluster_map_[b.cpu];

    const auto a_cluster_prime = (this_cluster + cluster_count_ - a_cluster->id) % cluster_count_;
    const auto b_cluster_prime = (this_cluster + cluster_count_ - b_cluster->id) % cluster_count_;
    if (a_cluster_prime != b_cluster_prime) {
      return a_cluster_prime < b_cluster_prime;
    }

    const auto a_count = a_cluster->member_count;
    const auto b_count = b_cluster->member_count;
    const size_t a_prime = a_cluster->members[(this_cpu + a_count - a_index) % a_count];
    const size_t b_prime = b_cluster->members[(this_cpu + b_count - b_index) % b_count];
    return a_prime < b_prime;
  };

  ktl::stable_sort(iterator().begin(), iterator().end(), comparator);
}

void CpuSearchSet::Dump() const {
  dprintf(INFO, "CPU %2" PRIu32 ": ", ordered_cpus_[0].cpu);
  for (cpu_num_t i = 0; i < cpu_count_; i++) {
    dprintf(INFO, "%2" PRIu32 "%s", ordered_cpus_[i].cpu, i < cpu_count_ - 1 ? ", " : "");
  }
  dprintf(INFO, "\n");
}
