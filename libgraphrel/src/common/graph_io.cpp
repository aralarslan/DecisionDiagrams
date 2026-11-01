#include "teddy/graphrel/graph_io.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace teddy::graphrel {

// ---------------------------------------------------------------------------
// ParseError
// ---------------------------------------------------------------------------

ParseError::ParseError(std::size_t line, const std::string &message)
    : std::runtime_error("line " + std::to_string(line) + ": " + message),
      line_(line) {}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

constexpr std::string_view whitespace = " \t\r";

/// Split a line into whitespace-separated tokens.
std::vector<std::string_view> tokenize(std::string_view line) {
  std::vector<std::string_view> tokens;
  std::size_t pos = line.find_first_not_of(whitespace);
  while (pos != std::string_view::npos) {
    std::size_t end = line.find_first_of(whitespace, pos);
    if (end == std::string_view::npos)
      end = line.size();
    tokens.push_back(line.substr(pos, end - pos));
    pos = line.find_first_not_of(whitespace, end);
  }
  return tokens;
}

/// Parse a non-negative integer; the whole token must be consumed.
std::size_t parse_index(std::string_view token, std::size_t line,
                        const char *what) {
  std::size_t value = 0;
  const char *first = token.data();
  const char *last = token.data() + token.size();
  auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc{} || ptr != last)
    throw ParseError(line, std::string("invalid ") + what + " '" +
                               std::string(token) +
                               "' (expected a non-negative integer)");
  return value;
}

/// Parse a probability in [0, 1]; the whole token must be consumed.
/// Uses the classic locale so the decimal separator is always '.'.
double parse_probability(std::string_view token, std::size_t line,
                         const char *what) {
  std::istringstream iss{std::string(token)};
  iss.imbue(std::locale::classic());
  double value = 0.0;
  iss >> value;
  if (!iss || iss.peek() != std::char_traits<char>::eof())
    throw ParseError(line, std::string("invalid ") + what + " '" +
                               std::string(token) + "' (expected a number)");
  if (!(value >= 0.0 && value <= 1.0)) // also rejects NaN
    throw ParseError(line, std::string(what) + " '" + std::string(token) +
                               "' out of range [0, 1]");
  return value;
}

void expect_token_count(const std::vector<std::string_view> &tokens,
                        std::size_t min, std::size_t max, std::size_t line,
                        const char *syntax) {
  if (tokens.size() < min || tokens.size() > max)
    throw ParseError(line, std::string("expected '") + syntax + "'");
}

std::string range_message(const char *what, std::size_t value,
                          std::size_t bound) {
  return std::string(what) + " " + std::to_string(value) +
         " out of range [0, " + std::to_string(bound) + ")";
}

} // namespace

// ---------------------------------------------------------------------------
// read_graph (stream)
// ---------------------------------------------------------------------------

graph read_graph(std::istream &input) {
  bool saw_problem = false;
  bool saw_terminals = false;
  std::size_t num_vertices = 0;
  std::size_t num_edges = 0;
  double default_edge_rel = 1.0;
  double default_vertex_rel = 1.0;

  struct RawEdge {
    std::size_t from, to;
    double prob;
  };
  std::vector<RawEdge> raw_edges;
  std::vector<std::size_t> terminals;
  std::vector<std::pair<std::size_t, double>> vertex_overrides;

  std::string line;
  std::size_t line_number = 0;

  while (std::getline(input, line)) {
    ++line_number;

    const std::vector<std::string_view> tokens = tokenize(line);
    if (tokens.empty() || tokens.front().front() == 'c')
      continue; // blank line or comment

    if (tokens.front().size() != 1)
      throw ParseError(line_number, "unknown line type '" +
                                        std::string(tokens.front()) + "'");

    switch (tokens.front().front()) {

    case 'p': {
      if (saw_problem)
        throw ParseError(line_number, "duplicate 'p' line");
      const char *syntax =
          "p reliability <V> <E> [default_edge_prob [default_vertex_prob]]";
      expect_token_count(tokens, 4, 6, line_number, syntax);
      if (tokens[1] != "reliability")
        throw ParseError(line_number, "expected 'p reliability ...', got 'p " +
                                          std::string(tokens[1]) + "'");

      num_vertices = parse_index(tokens[2], line_number, "vertex count");
      num_edges = parse_index(tokens[3], line_number, "edge count");
      if (num_vertices > graph::max_vertices)
        throw ParseError(line_number,
                         "vertex count " + std::to_string(num_vertices) +
                             " exceeds the supported maximum " +
                             std::to_string(graph::max_vertices));
      if (tokens.size() >= 5)
        default_edge_rel =
            parse_probability(tokens[4], line_number, "default edge probability");
      if (tokens.size() >= 6)
        default_vertex_rel = parse_probability(tokens[5], line_number,
                                               "default vertex probability");

      // Don't trust the declared count for a large up-front allocation.
      raw_edges.reserve(std::min<std::size_t>(num_edges, 1u << 16));
      saw_problem = true;
      break;
    }

    case 't': {
      if (!saw_problem)
        throw ParseError(line_number, "'t' line before 'p' line");
      if (saw_terminals)
        throw ParseError(line_number, "duplicate 't' line");
      if (tokens.size() < 2)
        throw ParseError(line_number, "terminal set must not be empty");

      for (std::size_t i = 1; i < tokens.size(); ++i) {
        std::size_t v = parse_index(tokens[i], line_number, "terminal vertex");
        if (v >= num_vertices)
          throw ParseError(line_number,
                           range_message("terminal vertex", v, num_vertices));
        terminals.push_back(v);
      }
      saw_terminals = true;
      break;
    }

    case 'e': {
      if (!saw_problem)
        throw ParseError(line_number, "'e' line before 'p' line");
      expect_token_count(tokens, 3, 4, line_number, "e <from> <to> [prob]");

      std::size_t from = parse_index(tokens[1], line_number, "edge endpoint");
      std::size_t to = parse_index(tokens[2], line_number, "edge endpoint");
      if (from >= num_vertices)
        throw ParseError(line_number,
                         range_message("edge endpoint", from, num_vertices));
      if (to >= num_vertices)
        throw ParseError(line_number,
                         range_message("edge endpoint", to, num_vertices));

      double rel = tokens.size() == 4
                       ? parse_probability(tokens[3], line_number,
                                           "edge probability")
                       : default_edge_rel;
      raw_edges.push_back({from, to, rel});
      break;
    }

    case 'v': {
      if (!saw_problem)
        throw ParseError(line_number, "'v' line before 'p' line");
      expect_token_count(tokens, 3, 3, line_number, "v <vertex> <prob>");

      std::size_t vertex = parse_index(tokens[1], line_number, "vertex");
      if (vertex >= num_vertices)
        throw ParseError(line_number,
                         range_message("vertex", vertex, num_vertices));
      double rel =
          parse_probability(tokens[2], line_number, "vertex probability");
      vertex_overrides.emplace_back(vertex, rel);
      break;
    }

    default:
      throw ParseError(line_number, "unknown line type '" +
                                        std::string(tokens.front()) + "'");
    }
  }

  // Post-parse validation
  if (!saw_problem)
    throw ParseError(line_number, "missing required 'p reliability' line");

  if (!saw_terminals)
    throw ParseError(line_number, "missing required 't' terminal line");

  if (raw_edges.size() != num_edges)
    throw ParseError(line_number,
                     "expected " + std::to_string(num_edges) + " edges, got " +
                         std::to_string(raw_edges.size()));

  graph g(num_vertices, num_edges);
  g.set_all_vertex_probs(default_vertex_rel);
  for (auto &[vertex, rel] : vertex_overrides)
    g.set_vertex_prob(vertex, rel);
  for (auto &[from, to, rel] : raw_edges)
    g.add_edge(from, to, rel);
  g.set_terminals(std::move(terminals)); // sorts and removes duplicates

  return g;
}

// ---------------------------------------------------------------------------
// read_graph (file path)
// ---------------------------------------------------------------------------

graph read_graph(std::string_view path) {
  std::string path_str(path);
  std::ifstream file(path_str);
  if (!file.is_open()) {
    throw std::runtime_error("Cannot open file: " + path_str);
  }
  return read_graph(file);
}

// ---------------------------------------------------------------------------
// write_graph (stream)
// ---------------------------------------------------------------------------

void write_graph(std::ostream &output, const graph &g,
                 const std::string &comment) {
  // Comments
  if (!comment.empty()) {
    std::istringstream iss(comment);
    std::string line;
    while (std::getline(iss, line))
      output << "c " << line << "\n";
  }

  // Problem line — always write defaults as 1.0
  output << "p reliability " << g.num_vertices() << " " << g.edges().size()
         << "\n";

  // Terminals
  output << "t";
  for (auto v : g.terminals())
    output << " " << v;
  output << "\n";

  // Probabilities are written with max_digits10 digits so that reading the
  // file back reproduces every double exactly. Restore the caller's stream
  // state afterwards.
  const std::ios_base::fmtflags old_flags = output.flags();
  const std::streamsize old_precision = output.precision();
  const std::locale old_locale = output.imbue(std::locale::classic());
  output << std::defaultfloat
         << std::setprecision(std::numeric_limits<double>::max_digits10);

  for (const auto &e : g.edges())
    output << "e " << e.from << " " << e.to << " " << e.prob << "\n";

  // Vertex probabilities — only write non-1.0 vertices
  for (std::size_t i = 0; i < g.num_vertices(); ++i) {
    if (g.vertex_probs()[i] != 1.0)
      output << "v " << i << " " << g.vertex_probs()[i] << "\n";
  }

  output.imbue(old_locale);
  output.precision(old_precision);
  output.flags(old_flags);
}

// ---------------------------------------------------------------------------
// write_graph (file path)
// ---------------------------------------------------------------------------

void write_graph(std::string_view path, const graph &g,
                 const std::string &comment) {
  std::string path_str(path);
  std::ofstream file(path_str);
  if (!file.is_open()) {
    throw std::runtime_error("Cannot open file: " + path_str);
  }
  write_graph(file, g, comment);
  if (!file) {
    throw std::runtime_error("Error while writing file: " + path_str);
  }
}

} // namespace teddy::graphrel
