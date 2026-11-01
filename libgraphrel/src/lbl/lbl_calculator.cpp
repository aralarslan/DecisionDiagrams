/**
 * @file lbl_calculator.cpp
 * @brief Layer-by-layer reliability (Wu & Sun 2024) on frontier-local states.
 */

#include "lbl/lbl_detail.hpp"
#include "common/validation.hpp"
#include "teddy/graphrel/reliability.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace teddy::graphrel {

namespace {
constexpr const char* who = "calculate_reliability";
}

lbl_calculator::lbl_calculator(const graph& g)
    : lbl_calculator(g, generate_variable_ordering(detail::require_valid(g, who),
                                                   ordering_strategy::cutwidth))
{}

// The graph and ordering are validated before anything indexes into them.
lbl_calculator::lbl_calculator(const graph& g, const std::vector<variable>& ordering)
    : net_(detail::require_valid(g, who))
    , ordering_(detail::require_valid_ordering(g, ordering, who))
{}

double lbl_calculator::calculate() {
    stats_ = statistics{};
    dropped_probability_ = 0.0;

    // |K| = 1: the terminal set is connected iff that vertex works.
    if (net_.terminals.size() == 1) {
        return net_.vertex_probs[net_.terminals.front()];
    }

    const detail::frontier_plan plan(net_, ordering_, /*force_perfect_vertices=*/true);
    stats_.num_layers = plan.levels();
    stats_.max_boundary_size = plan.max_front_size();

    detail::state_table current, next;
    current.reset(plan.key_size(0), 1);
    const std::vector<std::uint8_t> initial = plan.initial_key();
    current.add(initial.data(), 1.0);

    std::vector<std::uint8_t> low(8 + detail::frontier_plan::max_front);
    std::vector<std::uint8_t> high(low.size());
    double success = 0.0;

    // A branch with probability exactly 0 is skipped; with a prune threshold,
    // branches at or below it are dropped and their probability recorded,
    // so R lies in [result, result + dropped_probability()].
    auto route = [&](detail::outcome o, double probability, const std::vector<std::uint8_t>& key) {
        if (probability <= 0.0 || o == detail::outcome::failure)
            return;
        if (probability <= prune_threshold_) {
            dropped_probability_ += probability;
            return;
        }
        if (o == detail::outcome::success)
            success += probability;
        else
            next.add(key.data(), probability);
    };

    for (std::size_t level = 0; level < plan.levels(); ++level) {
        stats_.max_layer_size = std::max(stats_.max_layer_size, current.size());
        stats_.total_decompositions += current.size();
        next.reset(plan.key_size(level + 1), current.size());

        const variable& var = ordering_[level];
        const double p = var.type == variable_type::edge ? net_.edges[var.id].prob
                                                         : net_.vertex_probs[var.id];
        std::size_t branching = 0;
        for (std::size_t i = 0; i < current.size(); ++i) {
            const double parent = current.probability(i);
            const detail::expansion ex = plan.expand(level, current.key(i), low.data(), high.data());
            if (ex.single) {
                route(ex.high, parent, high);
            } else {
                ++branching;
                route(ex.low, parent * (1.0 - p), low);
                route(ex.high, parent * p, high);
            }
        }
        stats_.max_branching_layer_size = std::max(stats_.max_branching_layer_size, branching);
        stats_.peak_table_bytes = std::max(stats_.peak_table_bytes,
                                           current.memory_bytes() + next.memory_bytes());
        std::swap(current, next);
    }
    // Every state is resolved by the last layer (the final front is empty).
    return success;
}

// ── Public free functions ────────────────────────────────────────────────────

double calculate_reliability(const graph& g) {
    lbl_calculator calc(g);
    return calc.calculate();
}

double calculate_reliability(const graph& g, const std::vector<variable>& ordering) {
    lbl_calculator calc(g, ordering);
    return calc.calculate();
}

namespace {
reliability_bounds bounds_from(lbl_calculator& calc, double prune_threshold) {
    calc.set_prune_threshold(prune_threshold);
    const double lower = calc.calculate();
    return {lower, std::min(1.0, lower + calc.dropped_probability())};
}

void check_threshold(double prune_threshold) {
    if (!(prune_threshold >= 0.0 && prune_threshold < 1.0))
        throw std::invalid_argument(
            "calculate_reliability_bounds: prune_threshold must be in [0, 1)");
}
} // namespace

reliability_bounds calculate_reliability_bounds(const graph& g, double prune_threshold) {
    check_threshold(prune_threshold);
    lbl_calculator calc(g);
    return bounds_from(calc, prune_threshold);
}

reliability_bounds calculate_reliability_bounds(const graph& g,
                                                const std::vector<variable>& ordering,
                                                double prune_threshold) {
    check_threshold(prune_threshold);
    lbl_calculator calc(g, ordering);
    return bounds_from(calc, prune_threshold);
}

} // namespace teddy::graphrel
