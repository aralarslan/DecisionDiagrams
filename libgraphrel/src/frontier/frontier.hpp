#ifndef GRAPHREL_SRC_FRONTIER_HPP
#define GRAPHREL_SRC_FRONTIER_HPP

#include "teddy/graphrel/graph.hpp"
#include "teddy/graphrel/variable_ordering.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

/**
 * @file frontier.hpp
 * @brief Frontier-local decomposition states shared by LBL and the BDD builder.
 *
 * A decomposition state only has to describe what the processed part of the
 * graph means for the unprocessed part, and the two parts touch only at the
 * front: processed vertices that still have unprocessed edges (Carlier &
 * Lucet 1996, §4.1; Imai et al. 1999, "elimination front"). A state is
 * therefore, for each front position at its level,
 *   - 0 if that vertex is absent (failed, or pruned as a dead end), or
 *   - the label of its block: front vertices connected through the
 *     processed part share a label (labels canonical by first appearance),
 * plus a bitmask of the blocks that contain a terminal (Carlier & Lucet
 * 1996, §5.2). Interior processed vertices can never gain a connection, so
 * nothing else about the past influences the future. The graph itself and
 * everything derived from the ordering (front membership per level, when a
 * vertex leaves, remaining edge counts) is static and stored once in
 * frontier_plan.
 *
 * Key layout (frontier_plan::key_size(level) bytes):
 *   [ 8 bytes: K-mask, bit b-1 = block b holds a terminal ]
 *   [ F bytes: label per front position, 0 = absent ]
 */

namespace teddy::graphrel::detail {

/// What a branch of a variable leads to.
enum class outcome : std::uint8_t {
    failure, ///< the terminals can no longer be connected
    success, ///< all terminals are connected
    state    ///< a live state, written to the provided key buffer
};

/// Result of expanding one state by one variable.
struct expansion {
    /// True if the variable cannot influence this state's future (or is
    /// forced to work): only `high` is used and its probability factor is 1.
    bool single = false;
    outcome low = outcome::failure;  ///< component fails (factor 1 - p)
    outcome high = outcome::failure; ///< component works (factor p)
};

class frontier_plan {
public:
    /// Largest supported front: block labels fit a byte, K-marks a 64-bit mask.
    static constexpr std::size_t max_front = 64;

    /**
     * @param g        valid graph with |K| >= 2
     * @param ordering valid ordering (every variable once, vertex priority)
     * @param force_perfect_vertices treat vertices with p = 1 as variables
     *        that cannot fail (LBL: always; BDD: only when every vertex is
     *        perfect, so the diagram has no vertex variables)
     * @throws std::length_error if some front exceeds max_front
     */
    frontier_plan(const graph& g, const std::vector<variable>& ordering,
                  bool force_perfect_vertices);

    std::size_t levels() const { return steps_.size(); }
    /// Front size before processing ordering[level] (level <= levels()).
    std::size_t front_size(std::size_t level) const { return front_size_[level]; }
    std::size_t key_size(std::size_t level) const { return 8 + front_size_[level]; }
    std::size_t max_front_size() const { return max_front_size_; }

    /// Key of the initial state (empty front, no blocks).
    std::vector<std::uint8_t> initial_key() const { return std::vector<std::uint8_t>(8, 0); }

    /// Expand the state @p key at @p level by ordering[level]. Live children
    /// are written to @p low_key / @p high_key (key_size(level + 1) bytes).
    expansion expand(std::size_t level, const std::uint8_t* key,
                     std::uint8_t* low_key, std::uint8_t* high_key) const;

private:
    struct step {
        bool is_vertex = false;
        bool forced = false;     ///< only the "works" branch exists
        bool terminal = false;   ///< vertex step: the vertex is a terminal
        bool enters = false;     ///< vertex step: the vertex joins the front
        bool loop = false;       ///< edge step: self-loop
        bool depart_u = false;   ///< edge step: endpoint u leaves afterwards
        bool depart_v = false;   ///< edge step: endpoint v leaves afterwards
        std::uint32_t pos_u = 0; ///< edge step: positions in the front
        std::uint32_t pos_v = 0;
        std::uint32_t unreached_after = 0; ///< terminals not yet reached after this step
        std::size_t exits_offset = 0;      ///< into exits_/component_: next front
        std::size_t inner_offset = 0;      ///< into inner_edges_: next front
        std::size_t inner_count = 0;
        /// Look-ahead (next front): bit i = the future component of position i
        /// contains a terminal that has not been reached yet.
        std::uint64_t unreached_component_mask = 0;
        /// Future components holding unreached terminals but no front vertex.
        std::uint32_t floating_components = 0;
        /// The look-ahead can only fail if the next front spans several future
        /// components or a floating component exists; otherwise it is skipped.
        bool lookahead = false;
    };

    /// Finish a child: remove departing vertices (closing blocks), prune dead
    /// non-terminal blocks, canonicalise labels, detect success and failure.
    outcome finish(const step& s, std::size_t level, std::uint64_t kmask,
                   std::uint8_t* codes, std::size_t f, std::uint8_t* out) const;

    /// False if the terminal blocks and the unreached terminals can no longer
    /// all be joined, through blocks and future (unprocessed) edges.
    bool can_still_connect(const step& s, const std::uint8_t* codes, std::size_t n,
                           std::uint64_t kmask) const;

    void compute_lookahead(const graph& g, const std::vector<variable>& ordering);

    std::vector<step> steps_;
    std::vector<std::size_t> front_size_;
    /// Remaining unprocessed edge count of each position of front(level + 1).
    std::vector<std::uint32_t> exits_;
    /// Unprocessed edges between two positions of front(level + 1), as
    /// position pairs. Such an edge is no exit if either end is absent, and
    /// is internal (no exit) if both ends are in the same block.
    std::vector<std::pair<std::uint8_t, std::uint8_t>> inner_edges_;
    /// Vertex at each position of front(level + 1) (same offsets as exits_).
    std::vector<std::uint32_t> front_after_;
    /// For each position of front(level + 1): index of the first position in
    /// the same future component (same offsets as exits_).
    std::vector<std::uint8_t> component_;
    std::size_t max_front_size_ = 0;
};

/**
 * @brief Open-addressing table of states of one layer with exact keys.
 *
 * Entries are stored back to back in an arena, each as one record
 * [hash | probability | key, zero-padded to 8 bytes], so a lookup touches
 * one slot and one record. Keys are compared byte for byte, so two different
 * states can never be merged by a hash collision.
 */
class state_table {
public:
    /// The hash add() uses (64 well-mixed bits).
    static std::uint64_t hash(const std::uint8_t* key, std::size_t key_size);

    void reset(std::size_t key_size, std::size_t expected);

    /// Insert a key with @p probability or add to an existing entry.
    /// Returns the entry index.
    std::size_t add(const std::uint8_t* key, double probability) {
        return add(key, hash(key, key_size_), probability);
    }
    /// Same, with @p hash == hash(key, key_size()) already computed.
    std::size_t add(const std::uint8_t* key, std::uint64_t hash, double probability);

    std::size_t size() const { return size_; }
    const std::uint8_t* key(std::size_t i) const {
        return reinterpret_cast<const std::uint8_t*>(arena_.data() + i * stride_ + 2);
    }
    double probability(std::size_t i) const {
        return std::bit_cast<double>(arena_[i * stride_ + 1]);
    }
    std::size_t key_size() const { return key_size_; }
    std::size_t memory_bytes() const;

private:
    void grow();
    std::size_t key_size_ = 0;
    std::size_t stride_ = 2;  ///< record size in 8-byte words
    std::size_t size_ = 0;
    std::vector<std::uint64_t> arena_;
    std::vector<std::uint32_t> slots_; ///< entry index + 1, 0 = empty
};

} // namespace teddy::graphrel::detail

#endif // GRAPHREL_SRC_FRONTIER_HPP
