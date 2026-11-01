// Private header — not installed.

#ifndef GRAPHREL_SRC_FRONTIER_LAYER_ENGINE_HPP
#define GRAPHREL_SRC_FRONTIER_LAYER_ENGINE_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "frontier/frontier.hpp"

/**
 * @file layer_engine.hpp
 * @brief Level-by-level expansion of frontier states, shared by the
 *        layer-by-layer calculator and the BDD builder.
 *
 * Starting from the initial state, every level expands each state of the
 * current layer by that level's variable (frontier_plan::expand) and merges
 * the children into the next layer. Each state carries a probability: the
 * total probability of reaching it. Branches that end in success are summed;
 * failures are dropped.
 *
 * The order of a layer is the order in which its states were first produced
 * (parent g yields positions 2g, fails, and 2g + 1, works). Large layers can
 * be expanded by several threads; every state then still gets the same
 * position and every sum is accumulated in the same order, so results and
 * links are identical for every thread count (see layer_engine.cpp).
 */

namespace teddy::graphrel::detail
{

struct engine_options
{
    /// Threads for large layers (1 = serial; 0 = hardware concurrency).
    unsigned threads = 1;
    /// Drop branches with probability at or below this, and sum what was
    /// dropped (0 = exact). Requires keep_all_branches == false.
    double prune_threshold = 0.0;
    /// Keep branches whatever their probability (diagram construction: the
    /// structure must not depend on the probabilities). Otherwise branches of
    /// probability 0 are skipped.
    bool keep_all_branches = false;
    /// Parents per summation block; the grouping of floating-point sums.
    std::size_t block_size = 4096;
    /// Smallest layer expanded in parallel.
    std::size_t min_parallel_layer = 16384;
};

struct engine_statistics
{
    std::size_t max_layer_size = 0; ///< most states in one layer
    std::size_t max_branching_layer_size
        = 0;                           ///< most states split in one layer
    std::size_t total_states      = 0; ///< states over all layers
    std::size_t peak_memory_bytes = 0; ///< two live layers + scratch
};

struct engine_result
{
    double success = 0.0; ///< probability of the branches that end in success
    double dropped = 0.0; ///< probability of the pruned branches
    engine_statistics statistics;
};

/// Children of one state: indices into the next layer, or a sink.
/// low == high when the variable does not matter for the state.
struct state_links
{
    static constexpr std::uint32_t to_false = 0xFFFFFFFFu;
    static constexpr std::uint32_t to_true  = 0xFFFFFFFEu;
    /// Largest layer whose states can be referenced.
    static constexpr std::size_t max_states = 0xFFFFFFF0u;
    std::uint32_t low                       = to_false;
    std::uint32_t high                      = to_false;
};

class layer_engine
{
public:
    layer_engine(frontier_plan const& plan, engine_options const& options);
    /// The engine keeps a reference to the plan: a temporary would dangle.
    layer_engine(frontier_plan&&, engine_options const&) = delete;

    /**
     * @brief Expand every level of the plan.
     * @param probability probability(level): working probability of the
     *        variable at that level.
     * @param links if not null, (*links)[level][i] receives the children of
     *        the i-th state of that level (8 bytes per state).
     * @throws std::length_error if a recorded layer exceeds
     *         state_links::max_states, or a table exceeds 2^32 states
     */
    engine_result run (
        std::function<double(std::size_t)> const& probability,
        std::vector<std::vector<state_links>>* links = nullptr
    ) const;

private:
    frontier_plan const& plan_;
    engine_options options_;
};

} // namespace teddy::graphrel::detail

#endif // GRAPHREL_SRC_FRONTIER_LAYER_ENGINE_HPP
