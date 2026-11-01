#ifndef GRAPHREL_SRC_PARTITION_HPP
#define GRAPHREL_SRC_PARTITION_HPP

#include "common/bell_cache.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @file partition.hpp
 * @brief Boundary set partition representation and algorithms
 *
 * Implements partition canonicalization and numbering algorithms from
 * Carlier & Lucet for efficient isomorphism detection.
 */

namespace teddy::graphrel {

// Forward declaration
class union_find;
struct decomp_state;
struct graph;

/**
 * @struct boundary_partition
 * @brief Represents a partition of the boundary set
 *
 * Canonical representation of boundary set partition for isomorphism detection.
 * Partitions are identified by their canonical number and K-membership pattern.
 */
struct boundary_partition {
  std::vector<std::size_t>
      boundary_vertices;             ///< Boundary vertices (sorted, canonical)
  std::vector<int> block_assignment; ///< block_assignment[i] = block of
                                     ///< boundary_vertices[i] (1-indexed)
  int num_blocks;                    ///< Number of blocks in partition
  std::vector<bool>
      K_membership; ///< K_membership[block-1] = true if block contains terminal
  std::uint64_t canonical_number; ///< Unique partition number (computed)

  /**
   * @brief Default constructor
   */
  boundary_partition() : num_blocks(0), canonical_number(0) {}

  /**
   * @brief Compute canonical number from block assignment
   * @param cache Stirling-Bell cache for computation
   */
  void compute_canonical_number(const bell_cache &cache);
};

/**
 * @brief Compute boundary hash from sorted vertex IDs
 *
 * This hash is the only record of a state's boundary vertex set in
 * partition_key, so a 64-bit collision would merge two different states
 * (audit finding 1.6; removed only by the frontier-local redesign).
 *
 * Uses robust WyHash-style mixing to achieve near-zero collision rate.
 *
 * @param boundary_vertices Sorted list of boundary vertex IDs
 * @return 64-bit hash with strong avalanche properties
 */
inline std::uint64_t
compute_boundary_hash(const std::vector<std::size_t> &boundary_vertices) {
  // Seed with size to differentiate empty vs single-element
  uint64_t seed = static_cast<uint64_t>(boundary_vertices.size());

  for (std::size_t v : boundary_vertices) {
    // WyHash mixer: combines value with seed using rotation + XOR + prime
    seed ^= static_cast<uint64_t>(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);

    // Stronger avalanche: multiply by prime and mix bits
    seed *= 0xBF58476D1CE4E5B9ULL;
    seed ^= (seed >> 27);
  }

  // Final mix to ensure all bits are influenced
  seed *= 0x94D049BB133111EBULL;
  seed ^= (seed >> 31);

  return seed;
}

/**
 * @brief Convert partition to unique number (Carlier & Lucet Algorithm 2)
 * @param block_assignment Block assignment array (1-indexed blocks)
 * @param num_blocks Number of blocks
 * @param cache Stirling-Bell cache
 * @return Unique partition number
 */
std::uint64_t partition_to_number(const std::vector<int> &block_assignment,
                                  int num_blocks,
                                  const bell_cache &cache);

/**
 * @brief Convert number to partition (Carlier & Lucet Algorithm 1) - for
 * debugging
 * @param partition_num Partition number
 * @param F Size of boundary set
 * @param cache Stirling-Bell cache
 * @return Block assignment array
 */
std::vector<int> number_to_partition(std::uint64_t partition_num, std::size_t F,
                                     const bell_cache &cache);

/**
 * @brief Canonicalize partition from Union-Find
 * @param boundary_vertices Boundary vertices (unsorted)
 * @param uf Union-Find structure representing connectivity
 * @param K Terminal set
 * @param cache Stirling-Bell cache
 * @return Canonical partition
 */
boundary_partition
canonicalize_partition(const std::vector<std::size_t> &boundary_vertices,
                       const union_find &uf, const std::vector<std::size_t> &K,
                       const bell_cache &cache);

} // namespace teddy::graphrel

#endif // GRAPHREL_SRC_PARTITION_HPP
