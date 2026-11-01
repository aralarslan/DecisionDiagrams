/**
 * @file diagram.cpp
 * @brief Boundary-set BDD builder (Hardy, Lucet & Limnios 2007).
 *
 * Builds the BDD layer by layer on frontier-local states (see
 * frontier/frontier.hpp): states with the same boundary partition share one
 * node (Carlier & Lucet 1996; Imai et al. 1999). Vertex failures follow the
 * k-NR-IV semantics of Kawahara et al. (2019): every edge incident to a
 * failed vertex is unusable. Wu & Sun (2024) use this method as the
 * baseline for their layer-by-layer algorithm.
 */

#include "teddy/graphrel/diagram.hpp"
#include "common/validation.hpp"
#include "frontier/frontier.hpp"
#include "teddy/graphrel/variable_ordering.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace teddy::graphrel {

// ── reliability_diagram implementation ────────────────────────────────────────────────

reliability_diagram::reliability_diagram(teddy::bss_manager mgr,
                     teddy::bss_manager::diagram_t diag,
                     std::vector<double> edge_probs,
                     std::vector<teddy::int32> edge_to_var,
                     std::vector<teddy::int32> vertex_to_var)
    : manager(std::move(mgr))
    , diagram(std::move(diag))
    , probs(std::move(edge_probs))
    , edge_to_var_(std::move(edge_to_var))
    , vertex_to_var_(std::move(vertex_to_var))
{}

teddy::int32 reliability_diagram::edge_var(std::size_t edge_id) const {
    return edge_to_var_[edge_id];
}

teddy::int32 reliability_diagram::vertex_var(std::size_t vertex_id) const {
    return vertex_to_var_[vertex_id];
}

teddy::var_change reliability_diagram::edge_change(std::size_t edge_id,
                                          teddy::int32 from,
                                          teddy::int32 to) const {
    return teddy::var_change{edge_to_var_[edge_id], from, to};
}

// ── Internal BDD builder ─────────────────────────────────────────────────────

namespace {

using bdd_t = teddy::bss_manager::diagram_t;

bool has_imperfect_vertices(const graph& g) {
    for (double rel : g.vertex_probs) {
        if (rel < 1.0)
            return true;
    }
    return false;
}

teddy::int32 count_bdd_variables(const std::vector<variable>& ordering,
                                        bool has_imperfect) {
    teddy::int32 count = 0;
    for (const auto& var : ordering) {
        if (var.type == variable_type::edge)
            ++count;
        else if (var.type == variable_type::vertex && has_imperfect)
            ++count;
    }
    return count;
}

class diagram_builder {
public:
    diagram_builder(const graph& g, const std::vector<variable>& ordering,
                    teddy::bss_manager& mgr)
        : net_(g)
        , ordering_(ordering)
        , mgr_(mgr)
        , edge_to_var_(g.num_edges, -1)
        , vertex_to_var_(g.num_vertices, -1)
        , has_imperfect_(has_imperfect_vertices(g))
    {
        init_variable_index_mappings();
    }

    // Two passes over the same frontier states as the LBL calculator:
    //  1. forward, layer by layer: expand every state and record where its
    //     two branches lead (a state of the next layer, or the 0/1 sink);
    //  2. backward: create the TeDDy nodes from the last layer up.
    // Only the per-state links of all layers are kept (16 bytes per state);
    // state keys are needed for two layers at a time. No recursion, so the
    // depth of the diagram is not limited by the call stack.
    bdd_t build() {
        // |K| = 1: the function is just "that vertex works".
        if (net_.terminals.size() == 1) {
            const teddy::int32 var = vertex_to_var_[net_.terminals.front()];
            return var >= 0 ? mgr_.make_ite(var, mgr_.constant(1), mgr_.constant(0))
                            : mgr_.constant(1);
        }
        // Vertices with p = 1 are variables of the diagram whenever any
        // vertex is imperfect (one variable per vertex, as before).
        const detail::frontier_plan plan(net_, ordering_,
                                         /*force_perfect_vertices=*/!has_imperfect_);
        if (plan.levels() == 0)
            return mgr_.constant(0);

        constexpr std::int64_t to_false = -1;
        constexpr std::int64_t to_true = -2;
        struct links {
            std::int64_t low;
            std::int64_t high;
            bool single; ///< the variable does not matter here: no node
        };
        std::vector<std::vector<links>> layers(plan.levels());

        detail::state_table current, next;
        current.reset(plan.key_size(0), 1);
        const std::vector<std::uint8_t> initial = plan.initial_key();
        current.add(initial.data(), 0.0);
        std::vector<std::uint8_t> low(8 + detail::frontier_plan::max_front);
        std::vector<std::uint8_t> high(low.size());

        auto target = [&](detail::outcome o, const std::vector<std::uint8_t>& key) {
            if (o == detail::outcome::failure)
                return to_false;
            if (o == detail::outcome::success)
                return to_true;
            return static_cast<std::int64_t>(next.add(key.data(), 0.0));
        };

        for (std::size_t level = 0; level < plan.levels(); ++level) {
            next.reset(plan.key_size(level + 1), current.size());
            std::vector<links>& out = layers[level];
            out.resize(current.size());
            for (std::size_t i = 0; i < current.size(); ++i) {
                const detail::expansion ex =
                    plan.expand(level, current.key(i), low.data(), high.data());
                links& k = out[i];
                k.single = ex.single;
                k.high = target(ex.high, high);
                k.low = ex.single ? k.high : target(ex.low, low);
            }
            std::swap(current, next);
        }

        std::vector<bdd_t> below, above;
        for (std::size_t level = plan.levels(); level-- > 0;) {
            auto resolve = [&](std::int64_t t) {
                if (t == to_false)
                    return mgr_.constant(0);
                if (t == to_true)
                    return mgr_.constant(1);
                return below[static_cast<std::size_t>(t)];
            };
            above.clear();
            above.reserve(layers[level].size());
            for (const links& k : layers[level]) {
                if (k.single)
                    above.push_back(resolve(k.high));
                else
                    above.push_back(mgr_.make_ite(variable_index(level),
                                                  resolve(k.high), resolve(k.low)));
            }
            below.swap(above);
            std::vector<links>().swap(layers[level]); // release this layer
        }
        return below.front(); // layer 0 holds exactly the initial state
    }

    const std::vector<teddy::int32>& edge_to_var() const { return edge_to_var_; }
    const std::vector<teddy::int32>& vertex_to_var() const { return vertex_to_var_; }

private:
    void init_variable_index_mappings() {
        teddy::int32 idx = 0;
        for (const auto& var : ordering_) {
            if (var.type == variable_type::edge) {
                edge_to_var_[var.id] = idx++;
            } else if (has_imperfect_) {
                vertex_to_var_[var.id] = idx++;
            }
        }
    }

    teddy::int32 variable_index(std::size_t level) const {
        const variable& var = ordering_[level];
        return var.type == variable_type::edge ? edge_to_var_[var.id]
                                               : vertex_to_var_[var.id];
    }

    const graph& net_;
    const std::vector<variable>& ordering_;
    teddy::bss_manager& mgr_;
    std::vector<teddy::int32> edge_to_var_;
    std::vector<teddy::int32> vertex_to_var_;
    bool has_imperfect_;
};

} // anonymous namespace

// ── Public build_diagram ─────────────────────────────────────────────────────────

reliability_diagram build_diagram(const graph& g, teddy::int64 pool_size) {
    detail::require_valid(g, "build_diagram");
    auto ordering = generate_variable_ordering(g, ordering_strategy::cutwidth);
    const bool has_imperfect = has_imperfect_vertices(g);
    teddy::int32 var_count = count_bdd_variables(ordering, has_imperfect);

    teddy::bss_manager mgr(var_count, pool_size);
    diagram_builder builder(g, ordering, mgr);
    bdd_t diagram = builder.build();

    // Build probs vector indexed by TeDDy variable
    std::vector<double> probs(static_cast<std::size_t>(var_count), 0.0);
    const auto& e2v = builder.edge_to_var();
    const auto& v2v = builder.vertex_to_var();

    for (std::size_t edge_id = 0; edge_id < g.num_edges; ++edge_id) {
        teddy::int32 idx = e2v[edge_id];
        if (idx >= 0)
            probs[static_cast<std::size_t>(idx)] = g.edges[edge_id].prob;
    }
    if (has_imperfect) {
        for (std::size_t v = 0; v < g.num_vertices; ++v) {
            teddy::int32 idx = v2v[v];
            if (idx >= 0)
                probs[static_cast<std::size_t>(idx)] = g.vertex_probs[v];
        }
    }

    return reliability_diagram(std::move(mgr), std::move(diagram), std::move(probs),
                     std::vector<teddy::int32>(e2v),
                     std::vector<teddy::int32>(v2v));
}

} // namespace teddy::graphrel
