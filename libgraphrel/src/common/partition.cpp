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

bool boundary_partition::is_success_terminal(
    const std::vector<std::size_t> &K) const {
  if (K.empty()) {
    return false;
  }

  int terminal_block = -1;
  for (std::size_t t : K) {
    auto it = std::find(boundary_vertices.begin(), boundary_vertices.end(), t);
    if (it == boundary_vertices.end()) {
      continue;
    }

    std::size_t idx = static_cast<std::size_t>(it - boundary_vertices.begin());
    int block = block_assignment[idx];

    if (terminal_block == -1) {
      terminal_block = block;
    } else if (terminal_block != block) {
      return false;
    }
  }

  return terminal_block != -1;
}

bool boundary_partition::is_failure_terminal(
    const std::vector<std::size_t> &K) const {
  if (K.empty()) {
    return false;
  }

  std::set<int> terminal_blocks;
  for (std::size_t t : K) {
    auto it = std::find(boundary_vertices.begin(), boundary_vertices.end(), t);
    if (it == boundary_vertices.end()) {
      continue;
    }

    std::size_t idx = static_cast<std::size_t>(it - boundary_vertices.begin());
    int block = block_assignment[idx];
    terminal_blocks.insert(block);
  }

  return terminal_blocks.size() > 1;
}

std::uint64_t partition_to_number(const std::vector<int> &block_assignment,
                                  int num_blocks,
                                  const bell_cache &cache) {
  /**
   * @brief Convert partition to unique number (Carlier & Lucet Algorithm 2)
   *
   * Maps a partition of F elements into num_blocks blocks to a unique number.
   * This enables efficient isomorphism detection by comparing numbers instead
   * of comparing partition structures.
   *
   * Algorithm (from Carlier & Lucet 1996):
   * 1. Process elements in order, tracking the current number of blocks (j)
   * 2. When a new block appears (block == current_j + 1), increment j and
   *    add Stirling number contribution
   * 3. Otherwise, update number based on current block assignment
   * 4. Add sum of partitions with fewer blocks to get final number
   *
   * @param block_assignment Partition array: block_assignment[i] = block of
   * element i (1-indexed)
   * @param num_blocks Number of blocks in partition
   * @param cache Precomputed Stirling numbers cache
   * @return Unique partition number
   */
  std::size_t F = block_assignment.size();
  if (F == 0)
    return 0;
  if (F == 1)
    return 1;

  std::uint64_t num = 1;
  int current_j = 1; // Current number of blocks seen so far

  // Process elements 1..F-1 (element 0 is always in block 1)
  for (std::size_t i = 1; i < F; ++i) {
    int block = block_assignment[i];

    if (block == current_j + 1) {
      // New block discovered: increment block count and add Stirling
      // contribution
      current_j++;
      std::uint64_t stirling_val = cache.stirling(i, current_j);
      num += stirling_val * static_cast<std::uint64_t>(current_j);
    } else {
      // Element goes into existing block: update number based on block
      // assignment
      num = (num - 1) * static_cast<std::uint64_t>(current_j) +
            static_cast<std::uint64_t>(block);
    }
  }

  // Add sum of all partitions with fewer blocks
  // This ensures partitions with different numbers of blocks get different
  // numbers
  std::uint64_t sum_less_blocks = 0;
  for (int h = 1; h < num_blocks; ++h) {
    sum_less_blocks += cache.stirling(F, h);
  }

  return num + sum_less_blocks;
}

std::vector<int> number_to_partition(std::uint64_t partition_num, std::size_t F,
                                     const bell_cache &cache) {
  /**
   * @brief Convert number to partition (Carlier & Lucet Algorithm 1)
   *
   * Inverse operation of partition_to_number. Reconstructs the partition
   * structure from its unique number. This is primarily used for debugging
   * and verification (round-trip testing).
   *
   * Algorithm (from Carlier & Lucet 1996):
   * 1. Determine number of blocks j by finding which Stirling number range
   *    contains the partition number
   * 2. Reconstruct block assignments by working backwards through elements
   * 3. Use Stirling numbers to determine when new blocks appear
   *
   * @param partition_num Unique partition number
   * @param F Size of boundary set (number of elements)
   * @param cache Precomputed Stirling numbers cache
   * @return Block assignment array (1-indexed blocks)
   */
  if (F == 0)
    return {};
  if (F == 1)
    return {1};

  std::vector<int> block_assignment(F);

  // Step 1: Determine number of blocks j
  // Find which Stirling number range contains the partition number
  int j = 1;
  std::uint64_t cumulative = 0;
  std::uint64_t num = partition_num;

  while (cumulative + cache.stirling(F, j) < num) {
    cumulative += cache.stirling(F, j);
    j++;
    if (j > static_cast<int>(F)) {
      throw std::invalid_argument(
          "number_to_partition: invalid partition number");
    }
  }

  // Step 2: Reconstruct block assignments (working backwards)
  std::uint64_t Num = num - cumulative;

  int i = static_cast<int>(F) - 1;
  while (i > 0) {
    std::uint64_t stirling_val = cache.stirling(i, j);
    std::uint64_t threshold = stirling_val * static_cast<std::uint64_t>(j);

    if (Num <= threshold) {
      // Element i goes into existing block
      block_assignment[i] =
          static_cast<int>((Num - 1) % static_cast<std::uint64_t>(j) + 1);
      Num = (Num - 1) / static_cast<std::uint64_t>(j) + 1;
    } else {
      // Element i starts a new block
      Num -= threshold;
      block_assignment[i] = j;
      j--;
    }
    i--;
  }

  // Element 0 is always in block 1
  block_assignment[0] = 1;

  return block_assignment;
}

boundary_partition
canonicalize_partition(const std::vector<std::size_t> &boundary_vertices,
                       const union_find &uf, const std::vector<std::size_t> &K,
                       const bell_cache &cache) {
  /**
   * @brief Canonicalize boundary set partition from Union-Find structure
   *
   * OPTIMIZED VERSION: Uses thread-local workspace to eliminate heap
   * allocations. Replaces std::map and std::unordered_map with flat vectors +
   * sorting.
   *
   * Creates a canonical boundary set partition from Union-Find connectivity
   * information. This is the core isomorphism detection mechanism:
   * - Groups boundary vertices by their Union-Find roots (connected components)
   * - Assigns canonical block numbers (sorted by minimum vertex in each
   * component)
   * - Marks which blocks contain terminals (K-membership pattern)
   * - Computes canonical number for hash map lookup
   *
   * The canonical form ensures that isomorphic subgraphs (same connectivity
   * pattern) produce identical partition keys, enabling efficient merging.
   *
   * @param boundary_vertices Boundary vertices (unsorted, may contain
   * duplicates)
   * @param uf Union-Find structure representing vertex connectivity
   * @param K Terminal set (for K-membership marking)
   * @param cache Stirling-Bell cache for canonical number computation
   * @return Canonical boundary set partition
   */
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
  partition.K_membership.resize(partition.num_blocks, false);

  // Precompute which Union-Find roots correspond to terminal components.
  // K-membership propagates through contractions: if terminal T merged with
  // non-terminal V via contractEdge, find_root(T) == find_root(V). So the
  // block containing V is correctly marked even after T leaves the boundary.
  std::unordered_set<std::size_t> terminal_roots;
  for (std::size_t t : K) {
    terminal_roots.insert(uf.find_root(t));
  }

  int block_num = 1;
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

      partition.block_assignment[idx] = block_num;
    }

    partition.K_membership[block_num - 1] = has_K;
    block_num++;
  }

  // Step 7: Compute canonical number for hash map lookup
  partition.compute_canonical_number(cache);

  return partition;
}

} // namespace teddy::graphrel
