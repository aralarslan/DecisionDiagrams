// Private implementation header of the layer-by-layer calculator — not
// installed. Used by lbl_calculator.cpp and tests.

#ifndef GRAPHREL_SRC_LBL_DETAIL_HPP
#define GRAPHREL_SRC_LBL_DETAIL_HPP

#include "frontier/frontier.hpp"
#include "teddy/graphrel/graph.hpp"
#include "teddy/graphrel/variable_ordering.hpp"

#include <cstddef>
#include <vector>

namespace teddy::graphrel {

/**
 * @brief Layer-by-layer reliability (Wu & Sun 2024) on frontier-local states.
 *
 * Each layer is a table of states (see frontier.hpp) with the probability of
 * reaching them; processing one variable expands every state into at most
 * two children, merging children with identical keys. Only two layers are
 * alive at a time.
 */
class lbl_calculator {
public:
    /// Uses ordering_strategy::cutwidth.
    explicit lbl_calculator(const graph& g);
    lbl_calculator(const graph& g, const std::vector<variable>& ordering);

    // Holds a reference to the graph.
    lbl_calculator(const lbl_calculator&) = delete;
    lbl_calculator& operator=(const lbl_calculator&) = delete;

    double calculate();

    /// Drop branches with probability <= @p threshold (0 = exact, the default).
    void set_prune_threshold(double threshold) { prune_threshold_ = threshold; }

    /// Total probability of branches dropped by the last calculate() call.
    double dropped_probability() const { return dropped_probability_; }

    struct statistics {
        std::size_t max_layer_size = 0;       ///< G^max: most states in one layer
        std::size_t total_decompositions = 0; ///< states summed over all layers
        std::size_t max_boundary_size = 0;    ///< largest front
        std::size_t num_layers = 0;
        std::size_t peak_table_bytes = 0;     ///< largest memory of the two live layers
    };

    statistics get_statistics() const { return stats_; }

private:
    const graph& net_;
    std::vector<variable> ordering_;
    statistics stats_;
    double prune_threshold_ = 0.0;
    double dropped_probability_ = 0.0;
};

} // namespace teddy::graphrel

#endif // GRAPHREL_SRC_LBL_DETAIL_HPP
