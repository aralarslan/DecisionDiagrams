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

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "common/validation.hpp"
#include "frontier/frontier.hpp"
#include "frontier/layer_engine.hpp"
#include "teddy/graphrel/variable_ordering.hpp"

namespace teddy::graphrel
{

// ── reliability_diagram implementation
// ────────────────────────────────────────────────

reliability_diagram::reliability_diagram(
    teddy::bss_manager mgr,
    teddy::bss_manager::diagram_t diag,
    std::vector<double> edge_probs,
    std::vector<teddy::int32> edge_to_var,
    std::vector<teddy::int32> vertex_to_var
) :
    manager(std::move(mgr)),
    diagram(std::move(diag)),
    probs(std::move(edge_probs)),
    edge_to_var_(std::move(edge_to_var)),
    vertex_to_var_(std::move(vertex_to_var))
{
}

teddy::int32 reliability_diagram::edge_var(std::size_t edge_id) const
{
    return edge_to_var_.at(edge_id);
}

teddy::int32 reliability_diagram::vertex_var(std::size_t vertex_id) const
{
    return vertex_to_var_.at(vertex_id);
}

teddy::var_change reliability_diagram::edge_change(
    std::size_t edge_id,
    teddy::int32 from,
    teddy::int32 to
) const
{
    return teddy::var_change {edge_to_var_.at(edge_id), from, to};
}

// ── Internal BDD builder ─────────────────────────────────────────────────────

namespace
{

using bdd_t = teddy::bss_manager::diagram_t;

teddy::int32 count_bdd_variables (
    std::vector<variable> const& ordering,
    bool has_imperfect
)
{
    teddy::int32 count = 0;
    for (auto const& var : ordering)
    {
        if (var.type == variable_type::edge)
            ++count;
        else if (var.type == variable_type::vertex && has_imperfect)
            ++count;
    }
    return count;
}

class diagram_builder
{
public:
    diagram_builder(
        graph const& g,
        std::vector<variable> const& ordering,
        teddy::bss_manager& mgr,
        unsigned threads
    ) :
        net_(g),
        ordering_(ordering),
        mgr_(mgr),
        threads_(threads),
        edge_to_var_(g.num_edges(), -1),
        vertex_to_var_(g.num_vertices(), -1),
        has_imperfect_(! g.all_vertices_perfect())
    {
        init_variable_index_mappings();
    }

    // Two passes over the same frontier states as the LBL calculator:
    //  1. forward, layer by layer: expand every state and record where its
    //     two branches lead (a state of the next layer, or the 0/1 sink);
    //  2. backward: create the TeDDy nodes from the last layer up.
    // Only the per-state links of all layers are kept (8 bytes per state);
    // state keys are needed for two layers at a time. No recursion, so the
    // depth of the diagram is not limited by the call stack. The forward
    // pass can use several threads; the backward pass calls TeDDy, which is
    // single-threaded.
    bdd_t build ()
    {
        // |K| = 1: the function is just "that vertex works".
        if (net_.terminals().size() == 1)
        {
            teddy::int32 const var = vertex_to_var_[net_.terminals().front()];
            return var >= 0
                     ? mgr_.make_ite(var, mgr_.constant(1), mgr_.constant(0))
                     : mgr_.constant(1);
        }
        // Vertices with p = 1 are variables of the diagram whenever any
        // vertex is imperfect (one variable per vertex, as before).
        detail::frontier_plan const plan(
            net_,
            ordering_,
            /*force_perfect_vertices=*/! has_imperfect_
        );
        if (plan.levels() == 0)
            return mgr_.constant(0);

        // Forward pass on the shared engine: all branches are kept (the
        // structure must not depend on probabilities), and every state
        // records where its two branches lead.
        detail::engine_options options;
        options.threads           = threads_;
        options.keep_all_branches = true;
        std::vector<std::vector<detail::state_links>> layers;
        detail::layer_engine(plan, options)
            .run([] (std::size_t) { return 1.0; }, &layers);

        std::vector<bdd_t> below, above;
        for (std::size_t level = plan.levels(); level-- > 0;)
        {
            auto resolve = [&] (std::uint32_t t)
            {
                if (t == detail::state_links::to_false)
                    return mgr_.constant(0);
                if (t == detail::state_links::to_true)
                    return mgr_.constant(1);
                return below[t];
            };
            above.clear();
            above.reserve(layers[level].size());
            for (detail::state_links const& k : layers[level])
            {
                // low == high: the variable does not matter here, no node.
                if (k.low == k.high)
                    above.push_back(resolve(k.high));
                else
                    above.push_back(mgr_.make_ite(
                        variable_index(level),
                        resolve(k.high),
                        resolve(k.low)
                    ));
            }
            below.swap(above);
            // release this layer
            std::vector<detail::state_links>().swap(layers[level]);
        }
        return below.front(); // layer 0 holds exactly the initial state
    }

    std::vector<teddy::int32> const& edge_to_var () const
    {
        return edge_to_var_;
    }

    std::vector<teddy::int32> const& vertex_to_var () const
    {
        return vertex_to_var_;
    }

    std::vector<teddy::int32> take_edge_to_var ()
    {
        return std::move(edge_to_var_);
    }

    std::vector<teddy::int32> take_vertex_to_var ()
    {
        return std::move(vertex_to_var_);
    }

private:
    void init_variable_index_mappings ()
    {
        teddy::int32 idx = 0;
        for (auto const& var : ordering_)
        {
            if (var.type == variable_type::edge)
            {
                edge_to_var_[var.id] = idx++;
            }
            else if (has_imperfect_)
            {
                vertex_to_var_[var.id] = idx++;
            }
        }
    }

    teddy::int32 variable_index (std::size_t level) const
    {
        variable const& var = ordering_[level];
        return var.type == variable_type::edge ? edge_to_var_[var.id]
                                               : vertex_to_var_[var.id];
    }

    graph const& net_;
    std::vector<variable> const& ordering_;
    teddy::bss_manager& mgr_;
    unsigned threads_;
    std::vector<teddy::int32> edge_to_var_;
    std::vector<teddy::int32> vertex_to_var_;
    bool has_imperfect_;
};

} // anonymous namespace

// ── Public build_diagram
// ─────────────────────────────────────────────────────────

reliability_diagram build_diagram (graph const& g, teddy::int64 pool_size)
{
    detail::require_valid(g, "build_diagram");
    auto ordering = generate_variable_ordering(g, ordering_strategy::cutwidth);
    bool const has_imperfect = ! g.all_vertices_perfect();
    teddy::int32 var_count   = count_bdd_variables(ordering, has_imperfect);

    teddy::bss_manager mgr(var_count, pool_size);
    diagram_builder builder(g, ordering, mgr, /*threads=*/1);
    bdd_t diagram = builder.build();

    // Build probs vector indexed by TeDDy variable
    std::vector<double> probs(static_cast<std::size_t>(var_count), 0.0);
    auto const& e2v = builder.edge_to_var();
    auto const& v2v = builder.vertex_to_var();

    for (std::size_t edge_id = 0; edge_id < g.num_edges(); ++edge_id)
    {
        teddy::int32 idx = e2v[edge_id];
        if (idx >= 0)
            probs[static_cast<std::size_t>(idx)] = g.edges()[edge_id].prob;
    }
    if (has_imperfect)
    {
        for (std::size_t v = 0; v < g.num_vertices(); ++v)
        {
            teddy::int32 idx = v2v[v];
            if (idx >= 0)
                probs[static_cast<std::size_t>(idx)] = g.vertex_probs()[v];
        }
    }

    return reliability_diagram(
        std::move(mgr),
        std::move(diagram),
        std::move(probs),
        builder.take_edge_to_var(),
        builder.take_vertex_to_var()
    );
}

} // namespace teddy::graphrel
