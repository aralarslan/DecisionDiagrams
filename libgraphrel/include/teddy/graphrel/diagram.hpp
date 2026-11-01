#ifndef TEDDY_GRAPHREL_DIAGRAM_HPP
#define TEDDY_GRAPHREL_DIAGRAM_HPP

#include <libteddy/reliability.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "teddy/graphrel/graph.hpp"
#include "teddy/graphrel/variable_ordering.hpp"

/**
 * @file diagram.hpp
 * @brief BDD of the network structure function (boundary-set BDD).
 *
 * Builds a Binary Decision Diagram of the K-terminal structure function with
 * boundary-set partition isomorphism detection (Hardy, Lucet & Limnios 2007;
 * the baseline method of Wu & Sun 2024). The resulting reliability_diagram
 * owns a bss_manager and gives access to the full TeDDy analysis API:
 * calculate_probability, dpld, importance measures, MCVs, etc.
 */

namespace teddy::graphrel
{

struct reliability_diagram;

/**
 * @brief Settings of build_diagram().
 */
struct diagram_options
{
    /// Variable ordering. Empty (the default): generate_variable_ordering(g),
    /// i.e. cut-width.
    std::vector<variable> ordering;

    /// Threads for the forward pass over the boundary states: 1 (the
    /// default) runs serially, 0 uses std::thread::hardware_concurrency().
    /// The diagram is the same for every thread count; the node creation
    /// itself (TeDDy) is single-threaded.
    unsigned threads = 1;

    /// Initial TeDDy node pool size.
    teddy::int64 pool_size = 100'000;
};

/**
 * @brief Build a BDD of the K-terminal structure function.
 *
 * Uses the boundary-set BDD algorithm (Hardy, Lucet & Limnios 2007). Returns
 * a reliability_diagram owning the manager.
 *
 * @param g       Graph with edge/vertex reliabilities and terminal set.
 * @param options Ordering, threads and pool size (see diagram_options).
 * @return        Populated reliability_diagram ready for analysis.
 * @throws std::invalid_argument if @p g has no terminal set, or the ordering
 *         is not a permutation of all variables with every vertex before its
 *         edges
 */
reliability_diagram build_diagram (
    graph const& g,
    diagram_options const& options = {}
);

/**
 * @brief Move-only bundle: BDD manager + diagram + per-variable probabilities.
 *
 * Returned by build_diagram(). Owns the bss_manager so the BDD stays alive.
 */
struct reliability_diagram
{
    teddy::bss_manager manager;
    teddy::bss_manager::diagram_t diagram;
    std::vector<double> probs; ///< prob per TeDDy variable index

    /**
     * @brief Return the TeDDy variable index for a graph edge.
     *
     * Every edge has a variable, including edges with p = 1 (those never
     * appear as nodes in the diagram).
     * @throws std::out_of_range if @p edge_id is not an edge
     */
    teddy::int32 edge_var (std::size_t edge_id) const;

    /**
     * @brief Return the TeDDy variable index for a graph vertex.
     * @return -1 if no vertex has a variable, which happens when every vertex
     *         is perfect (p = 1). Otherwise every vertex has one.
     * @throws std::out_of_range if @p vertex_id is not a vertex
     */
    teddy::int32 vertex_var (std::size_t vertex_id) const;

    /**
     * @brief Create a var_change descriptor for dpld analysis on an edge.
     * @throws std::out_of_range if @p edge_id is not an edge
     */
    teddy::var_change edge_change (
        std::size_t edge_id,
        teddy::int32 from,
        teddy::int32 to
    ) const;

    // Move-constructible; move assignment deleted (bss_manager has no move
    // assign)
    reliability_diagram(reliability_diagram&&)                  = default;
    reliability_diagram& operator= (reliability_diagram&&)      = delete;
    reliability_diagram(reliability_diagram const&)             = delete;
    reliability_diagram& operator= (reliability_diagram const&) = delete;

private:
    friend reliability_diagram build_diagram (
        graph const& g,
        diagram_options const& options
    );

    // Only build_diagram() creates diagrams: the variable maps must match the
    // manager and the diagram.
    reliability_diagram(
        teddy::bss_manager mgr,
        teddy::bss_manager::diagram_t diag,
        std::vector<double> edge_probs,
        std::vector<teddy::int32> edge_to_var,
        std::vector<teddy::int32> vertex_to_var
    );

    std::vector<teddy::int32> edge_to_var_; ///< graph edge_id → TeDDy var index
    std::vector<teddy::int32>
        vertex_to_var_; ///< graph vertex_id → TeDDy var index
};

} // namespace teddy::graphrel

#endif // TEDDY_GRAPHREL_DIAGRAM_HPP
