/**
 * @file lbl_calculator.cpp
 * @brief Layer-by-layer reliability (Wu & Sun 2024) on frontier-local states.
 */

#include <algorithm>
#include <stdexcept>

#include "common/validation.hpp"
#include "frontier/layer_engine.hpp"
#include "lbl/lbl_detail.hpp"
#include "teddy/graphrel/reliability.hpp"

namespace teddy::graphrel
{

namespace
{

constexpr char const* who = "calculate_reliability";

} // namespace

lbl_calculator::lbl_calculator(graph const& g) :
    lbl_calculator(
        g,
        generate_variable_ordering(
            detail::require_valid(g, who),
            ordering_strategy::cutwidth
        )
    )
{
}

// The graph and ordering are validated before anything indexes into them.
lbl_calculator::lbl_calculator(
    graph const& g,
    std::vector<variable> const& ordering
) :
    net_(detail::require_valid(g, who)),
    ordering_(detail::require_valid_ordering(g, ordering, who))
{
}

double lbl_calculator::calculate()
{
    stats_               = statistics {};
    dropped_probability_ = 0.0;

    // |K| = 1: the terminal set is connected iff that vertex works.
    if (net_.terminals().size() == 1)
    {
        return net_.vertex_probs()[net_.terminals().front()];
    }

    detail::frontier_plan const plan(
        net_,
        ordering_,
        /*force_perfect_vertices=*/true
    );
    stats_.num_layers        = plan.levels();
    stats_.max_boundary_size = plan.max_front_size();

    detail::engine_options options;
    options.threads            = threads_;
    options.prune_threshold    = prune_threshold_;
    options.block_size         = block_size_;
    options.min_parallel_layer = min_parallel_layer_;
    detail::engine_result const r
        = detail::layer_engine(plan, options)
              .run(
                  [this] (std::size_t level)
                  {
                      variable const& var = ordering_[level];
                      return var.type == variable_type::edge
                               ? net_.edges()[var.id].prob
                               : net_.vertex_probs()[var.id];
                  }
              );

    stats_.max_layer_size           = r.statistics.max_layer_size;
    stats_.max_branching_layer_size = r.statistics.max_branching_layer_size;
    stats_.total_decompositions     = r.statistics.total_states;
    stats_.peak_table_bytes         = r.statistics.peak_memory_bytes;
    dropped_probability_            = r.dropped;
    return r.success;
}

// ── Public free functions ────────────────────────────────────────────────────

double calculate_reliability (graph const& g, unsigned threads)
{
    lbl_calculator calc(g);
    calc.set_threads(threads);
    return calc.calculate();
}

double calculate_reliability (
    graph const& g,
    std::vector<variable> const& ordering,
    unsigned threads
)
{
    lbl_calculator calc(g, ordering);
    calc.set_threads(threads);
    return calc.calculate();
}

namespace
{
reliability_bounds bounds_from (
    lbl_calculator& calc,
    double prune_threshold,
    unsigned threads
)
{
    calc.set_prune_threshold(prune_threshold);
    calc.set_threads(threads);
    double const lower = calc.calculate();
    return {lower, std::min(1.0, lower + calc.dropped_probability())};
}

void check_threshold (double prune_threshold)
{
    if (! (prune_threshold >= 0.0 && prune_threshold < 1.0))
        throw std::invalid_argument(
            "calculate_reliability_bounds: prune_threshold must be in [0, 1)"
        );
}
} // namespace

reliability_bounds calculate_reliability_bounds (
    graph const& g,
    double prune_threshold,
    unsigned threads
)
{
    check_threshold(prune_threshold);
    lbl_calculator calc(g);
    return bounds_from(calc, prune_threshold, threads);
}

reliability_bounds calculate_reliability_bounds (
    graph const& g,
    std::vector<variable> const& ordering,
    double prune_threshold,
    unsigned threads
)
{
    check_threshold(prune_threshold);
    lbl_calculator calc(g, ordering);
    return bounds_from(calc, prune_threshold, threads);
}

} // namespace teddy::graphrel
