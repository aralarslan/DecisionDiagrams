/**
 * @file lbl_calculator.cpp
 * @brief Layer-by-layer reliability (Wu & Sun 2024) on frontier-local states.
 *
 * Each layer is a table of states (see frontier.hpp) with the probability of
 * reaching them; processing one variable expands every state into at most
 * two children, merging children with identical keys (layer_engine.hpp).
 * Only two layers are alive at a time.
 */

#include <algorithm>
#include <stdexcept>
#include <string>

#include "common/validation.hpp"
#include "frontier/frontier.hpp"
#include "frontier/layer_engine.hpp"
#include "lbl/lbl_detail.hpp"
#include "teddy/graphrel/reliability.hpp"

namespace teddy::graphrel
{

namespace
{

constexpr char const* who = "analyze_reliability";

} // namespace

reliability_result detail::analyze_reliability(
    graph const& g,
    reliability_options const& options,
    parallel_grain const& grain
)
{
    detail::require_valid(g, who);
    if (! (options.prune_threshold >= 0.0 && options.prune_threshold < 1.0))
    {
        throw std::invalid_argument(
            std::string(who) + ": prune_threshold must be in [0, 1)"
        );
    }
    std::vector<variable> const ordering
        = options.ordering.empty()
            ? generate_variable_ordering(g)
            : detail::require_valid_ordering(g, options.ordering, who);

    reliability_result result;
    // |K| = 1: the terminal set is connected iff that vertex works.
    if (g.terminals().size() == 1)
    {
        result.reliability = g.vertex_probs()[g.terminals().front()];
        result.upper_bound = result.reliability;
        return result;
    }

    frontier_plan const plan(g, ordering, /*force_perfect_vertices=*/true);
    engine_options engine;
    engine.threads            = options.threads;
    engine.prune_threshold    = options.prune_threshold;
    engine.block_size         = grain.block_size;
    engine.min_parallel_layer = grain.min_parallel_layer;
    auto const probability    = [&] (std::size_t level)
    {
        variable const& var = ordering[level];
        return var.type == variable_type::edge ? g.edges()[var.id].prob
                                               : g.vertex_probs()[var.id];
    };
    engine_result const r      = layer_engine(plan, engine).run(probability);

    result.reliability         = r.success;
    result.upper_bound         = std::min(1.0, r.success + r.dropped);
    reliability_statistics& s  = result.statistics;
    s.num_layers               = plan.levels();
    s.max_front_size           = plan.max_front_size();
    s.max_layer_size           = r.statistics.max_layer_size;
    s.max_branching_layer_size = r.statistics.max_branching_layer_size;
    s.total_states             = r.statistics.total_states;
    s.peak_memory_bytes        = r.statistics.peak_memory_bytes;
    return result;
}

reliability_result analyze_reliability (
    graph const& g,
    reliability_options const& options
)
{
    return detail::analyze_reliability(g, options, detail::parallel_grain {});
}

double calculate_reliability (graph const& g)
{
    return analyze_reliability(g).reliability;
}

} // namespace teddy::graphrel
