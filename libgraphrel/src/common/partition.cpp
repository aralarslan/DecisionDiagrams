/**
 * @file partition.cpp
 * @brief Boundary set partition canonicalization (Carlier & Lucet algorithms)
 *
 * Implements partition canonicalization and numbering algorithms from
 * Carlier & Lucet (1996) for efficient isomorphism detection:
 * - partition_to_number: Maps partitions to canonical numbers (Algorithm 2)
 * - number_to_partition: Reconstructs partitions from numbers (Algorithm 1)
 * - canonicalize_partition: Creates canonical boundary set partitions from
 * Union-Find
 *
 * These algorithms enable efficient isomorphism detection by converting
 * partition structures into unique numbers for hash map lookups.
 *
 * @see Carlier & Lucet (1996) "A decomposition algorithm for network
 * reliability evaluation"
 */

#include "common/partition.hpp"
#include "common/union_find.hpp"
#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace teddy::graphrel {

namespace {
// Thread-local workspace for canonicalize_partition
// Eliminates repeated heap allocations in hot path
struct CanonicalizeWorkspace {
  // Replaces: std::map<std::size_t, std::vector<std::size_t>> components
  struct NodeEntry {
    std::size_t root;
    std::size_t vertex;
    bool operator<(const NodeEntry &other) const {
      return root < other.root; // Group by root for component identification
    }
  };

  // Replaces: std::vector<std::pair<std::size_t, std::vector<std::size_t>>>
  struct ComponentInfo {
    std::size_t min_vertex; // For canonical ordering
    std::size_t start_idx;  // Index into node_entries
    std::size_t count;      // Number of vertices in component

    bool operator<(const ComponentInfo &other) const {
      return min_vertex < other.min_vertex;
    }
  };

  // Reusable buffers (capacity preserved across calls)
  std::vector<std::size_t> sorted_boundary;
  std::vector<NodeEntry> node_entries;
  std::vector<ComponentInfo> components;

  void clear() {
    sorted_boundary.clear();
    node_entries.clear();
    components.clear();
  }
};
} // anonymous namespace

void boundary_partition::compute_canonical_number(
    const bell_cache &cache) {
  canonical_number = partition_to_number(block_assignment, num_blocks, cache);
}

// Carlier & Lucet (1996) §4.7, Algorithm 2 ("number from class"). Elements
// are processed in order; a restricted-growth block label equal to j+1 opens a
// new block, otherwise the number is refined within the current j blocks.
// Partitions with fewer blocks are numbered first. Verified exhaustively
// against a literal transcription (tests: carlier_lucet_class_numbering).
std::uint64_t partition_to_number(const std::vector<int> &block_assignment,
                                  int num_blocks,
                                  const bell_cache &cache) {
  const std::size_t F = block_assignment.size();
  if (F == 0)
    return 0;
  if (F == 1)
    return 1;

  std::uint64_t num = 1;
  std::size_t j = 1; // blocks seen so far (element 0 is always in block 1)
  for (std::size_t i = 1; i < F; ++i) {
    const auto block = static_cast<std::size_t>(block_assignment[i]);
    if (block == j + 1) {
      ++j;
      num += cache.stirling(i, j) * j;
    } else {
      num = (num - 1) * j + block;
    }
  }

  std::uint64_t fewer_blocks = 0;
  for (std::size_t h = 1; h < static_cast<std::size_t>(num_blocks); ++h)
    fewer_blocks += cache.stirling(F, h);
  return num + fewer_blocks;
}

// Carlier & Lucet (1996) §4.7, Algorithm 1 ("class from number"): the inverse
// of partition_to_number, used for testing and debugging.
std::vector<int> number_to_partition(std::uint64_t partition_num, std::size_t F,
                                     const bell_cache &cache) {
  if (F == 0)
    return {};
  if (F == 1)
    return {1};

  // Number of blocks j: the Stirling range S(F, j) containing the number.
  std::size_t j = 1;
  std::uint64_t cumulative = 0;
  while (cumulative + cache.stirling(F, j) < partition_num) {
    cumulative += cache.stirling(F, j);
    if (++j > F)
      throw std::invalid_argument(
          "number_to_partition: invalid partition number");
  }

  // Rebuild the labels from the last element backwards.
  std::vector<int> block_assignment(F);
  std::uint64_t num = partition_num - cumulative;
  for (std::size_t i = F - 1; i > 0; --i) {
    const std::uint64_t threshold = cache.stirling(i, j) * j;
    if (num <= threshold) { // element i joins an existing block
      block_assignment[i] = static_cast<int>((num - 1) % j + 1);
      num = (num - 1) / j + 1;
    } else { // element i opens block j
      num -= threshold;
      block_assignment[i] = static_cast<int>(j);
      --j;
    }
  }
  block_assignment[0] = 1;
  return block_assignment;
}

// Canonical form: boundary vertices sorted, blocks numbered by their smallest
// vertex, so isomorphic states (same partition of the same boundary) get the
// same block labels, K-marks and class number. Uses a thread-local workspace
// to avoid per-call allocations of the intermediate arrays.
boundary_partition
canonicalize_partition(const std::vector<std::size_t> &boundary_vertices,
                       const union_find &uf, const std::vector<std::size_t> &K,
                       const bell_cache &cache) {
  static thread_local CanonicalizeWorkspace ws;
  ws.clear();

  boundary_partition partition;

  // Step 1: Sort boundary vertices for canonical ordering
  ws.sorted_boundary = boundary_vertices;
  std::sort(ws.sorted_boundary.begin(), ws.sorted_boundary.end());
  partition.boundary_vertices = ws.sorted_boundary;

  // Partition numbers are only exact while B(F) fits in 64 bits and the
  // cache covers F; beyond that, distinct states could share a number.
  if (ws.sorted_boundary.size() > cache.max_n()) {
    throw std::length_error(
        "graphrel: boundary set of " +
        std::to_string(ws.sorted_boundary.size()) +
        " vertices exceeds the supported maximum of " +
        std::to_string(cache.max_n()) +
        " (Bell numbers beyond 25 overflow 64-bit partition numbers)");
  }

  // Handle empty boundary set
  if (ws.sorted_boundary.empty()) {
    partition.num_blocks = 0;
    partition.canonical_number = 0;
    return partition;
  }

  // Step 2: Group vertices by root (flat vector + sort instead of std::map)
  ws.node_entries.reserve(ws.sorted_boundary.size());
  for (std::size_t v : ws.sorted_boundary) {
    std::size_t root = uf.find_root(v);
    ws.node_entries.push_back({root, v});
  }
  std::sort(ws.node_entries.begin(), ws.node_entries.end()); // Group by root

  // Step 4: Build component info (linear scan through sorted entries)
  ws.components.reserve(ws.sorted_boundary.size() / 2); // Conservative estimate

  std::size_t i = 0;
  while (i < ws.node_entries.size()) {
    std::size_t root = ws.node_entries[i].root;
    std::size_t start = i;
    std::size_t min_vertex = ws.node_entries[i].vertex;

    // Scan all entries with same root
    while (i < ws.node_entries.size() && ws.node_entries[i].root == root) {
      min_vertex = std::min(min_vertex, ws.node_entries[i].vertex);
      ++i;
    }

    ws.components.push_back({min_vertex, start, i - start});
  }

  // Step 5: Sort components by min_vertex (canonical ordering)
  std::sort(ws.components.begin(), ws.components.end());

  // Step 6: Assign block numbers and K-membership
  partition.num_blocks = static_cast<int>(ws.components.size());
  partition.block_assignment.resize(ws.sorted_boundary.size());
  partition.K_membership.resize(ws.components.size(), false);

  // Precompute which Union-Find roots correspond to terminal components.
  // K-membership propagates through contractions: if terminal T merged with
  // non-terminal V via contract_edge, find_root(T) == find_root(V). So the
  // block containing V is correctly marked even after T leaves the boundary.
  std::unordered_set<std::size_t> terminal_roots;
  for (std::size_t t : K) {
    terminal_roots.insert(uf.find_root(t));
  }

  std::size_t block_index = 0; // block label is block_index + 1
  for (const auto &comp : ws.components) {
    // K-membership: does this component's root match any terminal's root?
    std::size_t comp_root = ws.node_entries[comp.start_idx].root;
    bool has_K = (terminal_roots.count(comp_root) > 0);

    // Assign block numbers to all vertices in this component
    for (std::size_t j = comp.start_idx; j < comp.start_idx + comp.count; ++j) {
      std::size_t v = ws.node_entries[j].vertex;

      // Binary search to find index in sorted_boundary
      auto it = std::lower_bound(ws.sorted_boundary.begin(),
                                 ws.sorted_boundary.end(), v);
      assert(it != ws.sorted_boundary.end() && *it == v &&
             "Boundary vertex unexpectedly missing from canonical list");
      std::size_t idx =
          static_cast<std::size_t>(it - ws.sorted_boundary.begin());

      partition.block_assignment[idx] = static_cast<int>(block_index + 1);
    }

    partition.K_membership[block_index] = has_K;
    ++block_index;
  }

  // Step 7: Compute canonical number for hash map lookup
  partition.compute_canonical_number(cache);

  return partition;
}

} // namespace teddy::graphrel
