#ifndef GRAPHREL_SRC_UNION_FIND_HPP
#define GRAPHREL_SRC_UNION_FIND_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @file union_find.hpp
 * @brief Union-Find (Disjoint Set Union) data structure
 *
 * Implements union-find with path compression and union by rank
 * for efficient connected component tracking during edge contractions.
 * Time complexity: O(alpha(n)) amortized per operation (alpha = inverse Ackermann).
 *
 * Uses compact storage: uint16_t parent (max 65535 elements) and uint8_t rank
 * (max rank 16 for 65535 elements). This reduces copy cost by ~5x compared to
 * size_t storage (3V bytes vs 16V bytes).
 */

namespace teddy::graphrel {

/**
 * @class union_find
 * @brief Union-Find data structure for tracking connected components
 *
 * Used during edge contractions to merge vertices and track connectivity.
 * Essential for computing boundary set partitions.
 */
class union_find {
private:
    std::vector<std::uint16_t> parent_;  ///< parent[i] = parent of i (or i if root)
    std::vector<std::uint8_t> rank_;     ///< rank[i] = approximate tree height

public:
    /**
     * @brief Constructor
     * @param n Number of elements (vertices)
     */
    explicit union_find(std::size_t n);

    /**
     * @brief Find root with path compression
     * @param x Element index
     * @return Root of x's component
     *
     * Path compression: sets parent[x] = find(parent[x]) for amortized O(alpha(n))
     */
    std::size_t find(std::size_t x);

    /**
     * @brief Find root without modification (const version)
     * @param x Element index
     * @return Root of x's component
     *
     * Does not perform path compression, suitable for const contexts.
     * Time complexity: O(log n) worst case.
     */
    std::size_t find_root(std::size_t x) const;

    /**
     * @brief Union two components by rank
     * @param x First element
     * @param y Second element
     *
     * Union by rank: always attaches smaller tree to larger tree root
     */
    void unite(std::size_t x, std::size_t y);

    /**
     * @brief Check if two elements are in the same component
     * @param x First element
     * @param y Second element
     * @return true if x and y are connected
     */
    bool connected(std::size_t x, std::size_t y) {
        return find(x) == find(y);
    }

    /**
     * @brief Check if two elements are in the same component (const version)
     * @param x First element
     * @param y Second element
     * @return true if x and y are connected
     */
    bool connected_const(std::size_t x, std::size_t y) const {
        return find_root(x) == find_root(y);
    }

    /**
     * @brief Get all equivalence classes (for partition extraction)
     * @return Vector of vectors, each inner vector contains indices in same class
     */
    std::vector<std::vector<std::size_t>> get_classes() const;

    /**
     * @brief Reset to initial state (all elements separate)
     */
    void reset();

    /**
     * @brief Get number of elements
     * @return Size of union-find structure
     */
    std::size_t size() const { return parent_.size(); }

    // Equality operator for tests
    bool operator==(const union_find& other) const {
        return parent_ == other.parent_ && rank_ == other.rank_;
    }
};

} // namespace teddy::graphrel

#endif // GRAPHREL_SRC_UNION_FIND_HPP
