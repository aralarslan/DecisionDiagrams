#ifndef TEDDY_GRAPHREL_GRAPH_IO_HPP
#define TEDDY_GRAPHREL_GRAPH_IO_HPP

#include <iosfwd>
#include <stdexcept>
#include <string>
#include <string_view>

#include "teddy/graphrel/graph.hpp"

/**
 * @file graph_io.hpp
 * @brief DIMACS-style file I/O for reliability graphs
 *
 * Format specification:
 *   c <comment>
 *   p reliability <vertices> <edges> [default_edge_rel] [default_vertex_rel]
 *   t <v0> <v1> ...          (0-indexed terminal vertices)
 *   e <from> <to> [reliability]
 *   v <vertex> <reliability>  (override vertex reliability)
 */

namespace teddy::graphrel
{

/// Exception for parse errors with line number context.
class ParseError : public std::runtime_error
{
public:
    ParseError(std::size_t line, std::string const& message);

    [[nodiscard]] std::size_t line_number () const noexcept
    {
        return line_;
    }

private:
    std::size_t line_;
};

/// Parse a graph from DIMACS-style reliability format.
/// @throws ParseError on malformed input (message includes line number)
[[nodiscard]] graph read_graph (std::istream& input);

/// Parse a graph from a file path.
/// @throws std::runtime_error if the file cannot be opened
/// @throws ParseError on malformed input (message includes line number)
[[nodiscard]] graph read_graph (std::string_view path);

/// Write a graph in DIMACS-style reliability format.
void write_graph (
    std::ostream& output,
    graph const& g,
    std::string const& comment = ""
);

/// Write a graph to a file path.
/// @throws std::runtime_error if the file cannot be opened
void write_graph (
    std::string_view path,
    graph const& g,
    std::string const& comment = ""
);

} // namespace teddy::graphrel

#endif // TEDDY_GRAPHREL_GRAPH_IO_HPP
