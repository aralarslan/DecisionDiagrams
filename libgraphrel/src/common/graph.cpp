/**
 * @file graph.cpp
 * @brief Graph structure implementation
 */

#include "teddy/graphrel/graph.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace teddy::graphrel
{

namespace
{

void check_probability (double prob, char const* what)
{
    if (! (prob >= 0.0 && prob <= 1.0))
    { // also rejects NaN
        throw std::invalid_argument(
            std::string(what) + " probability must be in [0, 1]"
        );
    }
}

} // namespace

graph::graph(std::size_t num_vertices, std::size_t expected_edges)
{
    if (num_vertices > max_vertices)
    {
        throw std::length_error(
            "graph: " + std::to_string(num_vertices)
            + " vertices exceed graph::max_vertices ("
            + std::to_string(max_vertices) + ")"
        );
    }
    vertex_probs_.assign(num_vertices, 1.0); // perfect vertices by default
    adj_.resize(num_vertices);
    edges_.reserve(expected_edges);
}

std::size_t graph::add_edge(std::size_t from, std::size_t to, double prob)
{
    if (from >= num_vertices() || to >= num_vertices())
    {
        throw std::out_of_range("Vertex index out of range");
    }
    check_probability(prob, "Edge");

    // Edge IDs are sequential and equal to the index in `edges_`.
    std::size_t const edge_id = edges_.size();
    edges_.emplace_back(edge_id, from, to, prob);

    adj_[from].push_back(edge_id);
    if (from != to)
    { // a self-loop is listed once
        adj_[to].push_back(edge_id);
    }

    return edge_id;
}

void graph::set_vertex_prob(std::size_t v, double prob)
{
    if (v >= num_vertices())
    {
        throw std::out_of_range("Vertex index out of range");
    }
    check_probability(prob, "Vertex");
    vertex_probs_[v] = prob;
}

void graph::set_all_vertex_probs(double prob)
{
    check_probability(prob, "Vertex");
    std::fill(vertex_probs_.begin(), vertex_probs_.end(), prob);
}

void graph::set_edge_prob(std::size_t e, double prob)
{
    if (e >= num_edges())
    {
        throw std::out_of_range("Edge index out of range");
    }
    check_probability(prob, "Edge");
    edges_[e].prob = prob;
}

void graph::set_terminals(std::vector<std::size_t> k)
{
    if (k.empty())
    {
        throw std::invalid_argument("Terminal set must not be empty");
    }
    for (std::size_t v : k)
    {
        if (v >= num_vertices())
        {
            throw std::out_of_range("Terminal vertex index out of range");
        }
    }
    std::sort(k.begin(), k.end());
    k.erase(std::unique(k.begin(), k.end()), k.end());
    terminals_ = std::move(k);
}

bool graph::is_terminal(std::size_t v) const
{
    return std::binary_search(terminals_.begin(), terminals_.end(), v);
}

std::vector<std::size_t> const& graph::get_incident_edges(std::size_t v) const
{
    if (v >= num_vertices())
    {
        throw std::out_of_range("Vertex index out of range");
    }
    return adj_[v];
}

std::vector<std::size_t> graph::get_neighbors(std::size_t v) const
{
    std::vector<std::size_t> const& incident = get_incident_edges(v);
    std::vector<std::size_t> neighbors;
    neighbors.reserve(incident.size());
    for (std::size_t edge_id : incident)
    {
        edge const& e = edges_[edge_id];
        neighbors.push_back(e.other(v));
    }
    return neighbors;
}

bool graph::all_vertices_perfect() const
{
    return std::all_of(
        vertex_probs_.begin(),
        vertex_probs_.end(),
        [] (double p) { return p >= 1.0; }
    );
}

std::vector<std::size_t> graph::loop_free_degrees() const
{
    std::vector<std::size_t> degree(num_vertices(), 0);
    for (edge const& e : edges_)
    {
        if (! e.is_loop())
        {
            ++degree[e.from];
            ++degree[e.to];
        }
    }
    return degree;
}

bool graph::validate() const
{
    return ! terminals_.empty();
}

} // namespace teddy::graphrel
