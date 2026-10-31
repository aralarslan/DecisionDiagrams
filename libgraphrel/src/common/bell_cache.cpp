/**
 * @file bell_cache.cpp
 * @brief Precomputed Stirling and Bell numbers for partition canonicalization
 *
 * Precomputes Stirling numbers of the second kind S(n,k) and Bell numbers B(n)
 * for efficient partition numbering algorithms (Carlier & Lucet).
 *
 * Stirling numbers S(n,k): Number of ways to partition n elements into k
 * non-empty subsets.
 *
 * Bell numbers B(n): Total number of partitions of n elements.
 *
 * Recurrence relations:
 * - S(n,k) = k*S(n-1,k) + S(n-1,k-1)
 * - B(n) = sum_{k=1}^n S(n,k)
 */

#include "common/bell_cache.hpp"
#include <stdexcept>

namespace teddy::graphrel {

bell_cache::bell_cache(std::size_t max_n) : max_n_(max_n) {
  /**
   * @brief Precompute all Stirling and Bell numbers up to max_n
   *
   * Uses dynamic programming with recurrence relations to build the tables.
   * Typical max_n is 20, which covers boundary sets up to 20 vertices.
   */
  if (max_n == 0) {
    throw std::invalid_argument("bell_cache: max_n must be > 0");
  }
  if (max_n > max_supported_n) {
    throw std::invalid_argument(
        "bell_cache: max_n must be <= 25 (larger Bell numbers overflow 64 bits)");
  }

  // Initialize tables: stirling_[n][k] = S(n,k)
  // S(n,k) = number of ways to partition n elements into k non-empty subsets
  stirling_.resize(max_n + 1);
  bell_.resize(max_n + 1, 0);

  // Base cases for Stirling numbers:
  // S(0,0) = 1 (empty partition: one way to partition 0 elements into 0 blocks)
  // S(n,0) = 0 for n > 0 (cannot partition n > 0 elements into 0 blocks)
  // S(n,1) = 1 (all elements in one block: only one way)
  // S(n,n) = 1 (each element in its own block: only one way)

  for (std::size_t n = 0; n <= max_n; ++n) {
    stirling_[n].resize(n + 1, 0);

    if (n == 0) {
      // Base case: empty partition
      stirling_[0][0] = 1;
      bell_[0] = 1; // B(0) = 1 (one partition of empty set)
    } else {
      // Base cases for n > 0
      stirling_[n][1] = 1; // All elements in one block
      stirling_[n][n] = 1; // Each element in its own block

      // Recurrence relation: S(n,k) = k*S(n-1,k) + S(n-1,k-1)
      // - k*S(n-1,k): element n goes into one of k existing blocks
      // - S(n-1,k-1): element n starts a new block
      for (std::size_t k = 2; k < n; ++k) {
        stirling_[n][k] = k * stirling_[n - 1][k] + stirling_[n - 1][k - 1];
      }

      // Bell number: B(n) = sum_{k=1}^n S(n,k)
      // Total number of partitions of n elements (any number of blocks)
      std::uint64_t bell_sum = 0;
      for (std::size_t k = 1; k <= n; ++k) {
        bell_sum += stirling_[n][k];
      }
      bell_[n] = bell_sum;
    }
  }
}

std::uint64_t bell_cache::stirling(std::size_t n, std::size_t k) const {
  if (n > max_n_) {
    return 0; // Not precomputed
  }
  if (k > n) {
    return 0; // Invalid: cannot partition n elements into k > n blocks
  }
  if (k == 0) {
    return (n == 0) ? 1 : 0; // S(0,0) = 1, S(n,0) = 0 for n > 0
  }
  return stirling_[n][k];
}

std::uint64_t bell_cache::bell(std::size_t n) const {
  if (n > max_n_) {
    return 0; // Not precomputed
  }
  return bell_[n];
}

} // namespace teddy::graphrel
