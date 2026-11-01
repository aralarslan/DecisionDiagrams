#ifndef TEDDY_GRAPHREL_GRAPH_HPP
#define TEDDY_GRAPHREL_GRAPH_HPP

#include <cstddef>
#include <vector>

/**
 * @file graph.hpp
 * @brief Graph representation for reliability analysis
 *
 * An undirected multigraph with a working probability per vertex and per
 * edge and a terminal set K. Every mutation is checked, so a graph is always
 * well-formed; the only precondition left to the algorithms is a non-empty
 * terminal set.
 */

namespace teddy::graphrel
{

/**
 * @struct edge
 * @brief Represents a single edge in the graph
 */
struct edge
{
    std::size_t id;   ///< Unique edge identifier (0-indexed)
    std::size_t from; ///< Source vertex (0-indexed)
    std::size_t to;   ///< Target vertex (0-indexed)
    double prob;      ///< Probability that the edge works

    /// The endpoint opposite @p v (v itself for a self-loop).
    std::size_t other (std::size_t v) const
    {
        return from == v ? to : from;
    }

    /// True for a self-loop; it never connects anything.
    bool is_loop () const
    {
        return from == to;
    }

    edge(std::size_t id_, std::size_t from_, std::size_t to_, double prob_) :
        id(id_),
        from(from_),
        to(to_),
        prob(prob_)
    {
    }
};

/**
 * @class graph
 * @brief Network for reliability analysis: structure, probabilities, terminals.
 *
 * Invariants (kept by every member function):
 *   - num_vertices() <= max_vertices; vertices are 0 .. num_vertices() - 1;
 *   - edges()[i].id == i, endpoints are vertices, probabilities lie in [0, 1];
 *   - get_incident_edges(v) lists each edge incident to v (a self-loop once);
 *   - terminals() is sorted, duplicate-free and holds vertices (it is empty
 *     until set_terminals() is called).
 */
class graph
{
public:
    /// Largest supported number of vertices (a sanity bound for inputs; the
    /// algorithms store vertex ids in 32 bits).
    static constexpr std::size_t max_vertices = std::size_t {1} << 24;

    /// An empty graph (no vertices).
    graph() = default;

    /**
     * @brief A graph with @p num_vertices perfect vertices (p = 1) and no
     * edges.
     * @param expected_edges Only reserves space for that many add_edge() calls.
     * @throws std::length_error if @p num_vertices > max_vertices
     */
    explicit graph(std::size_t num_vertices, std::size_t expected_edges = 0);

    // ── Construction ─────────────────────────────────────────────────────────

    /**
     * @brief Add an edge; returns its id (edge ids are 0, 1, 2, ... in order).
     * @throws std::out_of_range if an endpoint is not a vertex
     * @throws std::invalid_argument if @p prob is not in [0, 1] (or is NaN)
     */
    std::size_t add_edge (std::size_t from, std::size_t to, double prob);

    /// @throws std::out_of_range / std::invalid_argument as for add_edge()
    void set_vertex_prob (std::size_t v, double prob);
    /// Set every vertex probability. @throws std::invalid_argument
    void set_all_vertex_probs (double prob);
    /// @throws std::out_of_range / std::invalid_argument as for add_edge()
    void set_edge_prob (std::size_t e, double prob);

    /**
     * @brief Set the terminal set K (sorted and de-duplicated).
     * @throws std::out_of_range if a vertex index is out of range
     * @throws std::invalid_argument if @p k is empty
     */
    void set_terminals (std::vector<std::size_t> k);

    // ── Queries ──────────────────────────────────────────────────────────────

    std::size_t num_vertices () const
    {
        return vertex_probs_.size();
    }

    std::size_t num_edges () const
    {
        return edges_.size();
    }

    /// All edges; edges()[i].id == i.
    std::vector<edge> const& edges () const
    {
        return edges_;
    }

    /// Working probability of each vertex.
    std::vector<double> const& vertex_probs () const
    {
        return vertex_probs_;
    }

    /// The terminal set K: sorted, duplicate-free (empty until set).
    std::vector<std::size_t> const& terminals () const
    {
        return terminals_;
    }

    /// True if @p v is in K.
    bool is_terminal (std::size_t v) const;

    /// True if every vertex works with probability 1.
    bool all_vertices_perfect () const;

    /// Number of non-loop edges incident to each vertex.
    std::vector<std::size_t> loop_free_degrees () const;

    /// Ids of the edges incident to @p v (a self-loop listed once).
    /// @throws std::out_of_range if @p v is not a vertex
    std::vector<std::size_t> const& get_incident_edges (std::size_t v) const;

    /// Neighbours of @p v, one per incident edge (v itself for a self-loop).
    /// @throws std::out_of_range if @p v is not a vertex
    std::vector<std::size_t> get_neighbors (std::size_t v) const;

    /**
     * @brief True if the graph can be analysed, i.e. a terminal set was set.
     *
     * Everything else is guaranteed by construction. calculate_reliability()
     * and build_diagram() throw std::invalid_argument when this is false.
     */
    bool validate () const;

private:
    std::vector<double> vertex_probs_;
    std::vector<edge> edges_;
    std::vector<std::size_t> terminals_;
    std::vector<std::vector<std::size_t>> adj_;
};

} // namespace teddy::graphrel

#endif // TEDDY_GRAPHREL_GRAPH_HPP
