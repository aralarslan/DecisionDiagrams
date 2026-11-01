# Auditing and Re-engineering libgraphrel: Exact K-Terminal Network Reliability with Frontier-Local Layer-by-Layer Decomposition

**Aral** — technical report and handoff, November 2026

Branch `feat/libgraphrel-optimisations` of the DecisionDiagrams (TeDDy) fork. Audit baseline: commit `39fe87d`.

---

## Abstract

libgraphrel computes the K-terminal reliability of networks with unreliable edges *and*
vertices. It offers two methods: the layer-by-layer decomposition (LBL) of Wu & Sun (2024), and
a boundary-set binary decision diagram (BS-BDD) whose structure-function view enables TeDDy's
importance analysis.

We audited the library against its four governing references, using a brute-force oracle,
differential fuzzing restricted to each method's stated assumptions, and reproduction of the
published benchmarks. The audit found:
- a semantic error in the BDD construction (edges incident to failed vertices contracted);
- unenforced preconditions that turned invalid inputs into silently wrong answers;
- a hidden probability cutoff that made results inexact and distorted every performance
  measurement taken on top of it;
- costs quadratic in graph size;
- a test suite that no longer compiled.

We then re-engineered the library in verified steps.
1. The preconditions are now enforced and the approximation is opt-in with a rigorous error bound.
2. A cut-width variable ordering replaces breadth-first search. On the paper's 10×10 mesh it cuts
   the peak number of states per layer from 387,481 to 65,858.
3. A **frontier-local state representation** replaces whole-graph states. It stores, per state,
   only the partition of the processed/unprocessed boundary, the terminal marks and the vertex
   failures: a few bytes instead of O(|V|+|E|).

Every result is unchanged: 0 mismatches against exhaustive enumeration on 40,000 random graphs,
and identical canonical BDD node counts.

Counting states the way Wu & Sun do (only those a layer's variable splits), we reproduce every
layer size they publish exactly, from the 4 states of the 2×20 ladder to the 5,460,095 of K₁₃.
The earlier 1.2–1.8× "gaps" were a counting convention, not a difference in state definition.
Further steps cut the work:
- the edges of each vertex are ordered departing-first (up to 2.2× fewer states);
- with perfect vertices, orderings are ranked by estimated work (up to 6.6× less, on graphs not
  used to design the rule);
- large layers are expanded in parallel, with results bit-identical for any thread count.

Run times against the paper's:
- 10×10 mesh: 4.9 s on one core and 2.2 s on four, against 42.9 s (145 s and 1 GB before the
  redesign).
- 13×13 mesh: 189 s on four cores, against 4,722 s.
- 1000×10 mesh (10,000 vertices), previously out of reach: the published reliability in 304 s
  and 33 MB, against 8,066 s and 540 MB.

Finally, `graph` became a class whose invariants make the audit's invalid inputs
unrepresentable.

---

## 1. Introduction

### 1.1 Problem

Given an undirected graph G = (V, E), independent working probabilities p(e) and p(v), and a
terminal set K ⊆ V, the **K-terminal reliability with imperfect vertices** (k-NR-IV) is the
probability that every terminal works and all terminals are connected by working edges through
working vertices. The problem is #P-complete, so exact methods trade exponential cost in a
structural width for polynomial cost in graph size.

### 1.2 The two methods in libgraphrel

| Entry point | Method | Governing reference |
|---|---|---|
| `calculate_reliability` | Layer-by-layer recursive decomposition (LBL) | Wu & Sun (2024) |
| `build_diagram` | Boundary-set BDD (a TeDDy `bss_manager` diagram) | Hardy, Lucet & Limnios (2007); used as baseline in Wu & Sun §2.3 |
| Shared machinery | Boundary sets, partitions, loop/coloop rules | Carlier & Lucet (1996); Imai, Sekine & Imai (1999) |
| Vertex-failure semantics, variable order | k-NR-IV, R_EV | Kawahara, Sonoda, Inoue & Kasahara (2019) |

### 1.3 Contributions

1. An audit methodology for exact reliability code. The oracle is brute force on small graphs,
   with fuzzing modes that mirror each method's assumptions. This separates genuine defects from
   inputs outside a method's scope.
2. A catalogue of defects with root causes, plus the corresponding fixes and regression tests.
3. Proof by measurement that the performance changes preserve behaviour: identical orderings,
   layer sizes and canonical BDD node counts.
4. A cut-width ordering heuristic, and the empirical finding that the **edge cut**, not the
   boundary size, predicts the state count.
5. A frontier-local engine shared by LBL and BDD. It has exact state keys, O(F) work per state,
   and no Bell-number ceiling.
6. Exact reproduction of every layer size Wu & Sun publish, after identifying what their B¹
   counts. The state definitions are the same; the earlier "gaps" were a counting convention.
7. A rule for the order of each vertex's edges, and an ordering score for perfect vertices
   that accounts for how long a layer stays wide, each validated on graphs not used to design it.
8. Parallel layer expansion whose results are bit-identical for every thread count.
9. A `graph` class whose invariants make the invalid inputs of the audit unrepresentable.
10. A traceable record of every decision, including the mistakes and how they were corrected.

---

## 2. Background

### 2.1 Decomposition and isomorphic subproblems

Processing the components in a fixed order and branching on each one (Shannon expansion,
R(G) = p·R(G | x works) + (1−p)·R(G | x fails)) yields a binary tree with exponentially many
leaves. Exact methods become practical by **merging** subproblems that are guaranteed to have the
same future:
- BDDs share isomorphic nodes;
- LBL merges isomorphic subgraphs within a layer and adds their probabilities (Wu & Sun 2024, Eq. 4).

### 2.2 Why the boundary is enough

At any point in the decomposition the graph splits into the processed part (variables already
decided) and the unprocessed part. Two kinds of information are involved:
- **Static information**, stored once and shared by all states: the graph, the probabilities,
  the ordering, and facts derived from them, such as the level at which each vertex sees its last
  edge. All states at the same layer have the same unprocessed part.
- **Dynamic information**, per state: the outcomes of the processed variables. Most of it can be
  forgotten.

The **front** (boundary set) at a level is the set of processed vertices that still have
unprocessed edges (Carlier & Lucet 1996 §4.1; Imai et al. 1999's elimination front).

**Proposition 1 (sufficiency of the front state).** Let x and y be two outcomes of the processed
variables that agree on:
- (a) which front vertices are up;
- (b) the partition of the up front vertices into blocks connected through the processed part;
- (c) which blocks contain a terminal.

Suppose also that neither x nor y has already failed or succeeded. Then for every outcome z of
the unprocessed variables, the system works under (x, z) if and only if it works under (y, z).

*Proof sketch.* Take any path between two terminals in the working subgraph under (x, z). Split
it into maximal segments that use only processed edges and segments that use only unprocessed
edges. A processed segment can leave the processed region only at a front vertex, because
interior processed vertices have no unprocessed edges. So each processed segment either connects
two front vertices of the same block, or connects a terminal to a front vertex of a terminal
block. Unprocessed segments depend only on z and on which front vertices are up. Hence (a)–(c)
determine every possible connection, and the same path structure exists under (y, z). Terminals
inside the processed region are represented by the marks (c). A terminal block that lost its
last front vertex was already resolved as success or failure, which the hypothesis excludes. ∎

So two states with equal (a)–(c) are interchangeable, and merging them with summed probabilities
is exact. This is the isomorphism rule in Carlier & Lucet (§4.4, §5.2), Imai et al. (Theorem 1)
and Wu & Sun (§2.3).

**Lemma 2 (dead ends).** A block with no terminal and at most one usable unprocessed edge cannot
lie on a path between terminals: such a path would have to enter and leave the block through two
different edges. Marking its vertices absent changes no future outcome. ∎

**Example.** The bridge of Wu & Sun Fig. 1: K = {v1, v4}, ordering v1, v2, e1, v3, e2, e3, v4,
e4, e5, all probabilities 0.9. After e3, v1 has no unprocessed edges, so the front is {v2, v3}.
The live states are:
- `[v2 v3]*` — both connected to v1;
- `[v2]* [v3]` — only v2 reaches v1;
- `[v2] [v3]*` — only v3 reaches v1;
- `[v2]*` with v3 down;
- `[v3]*` with v2 down.

By Lemma 2, `[v3]` in the second state has no terminal and a single remaining edge (e5), so it is
equivalent to v3 being down. The same holds for the third state. That leaves three states, which
is what the implementation produces at that layer.

### 2.3 Assumptions of the references

| Assumption | Wu & Sun | Carlier & Lucet | Imai et al. | Kawahara et al. |
|---|---|---|---|---|
| G connected | §2.1 (1) | §2 | §2.1 | §2.1 |
| Independent two-state components | §2.1 (2–3) | §4.3 | §2.1 | §2.1 |
| 2 ≤ \|K\| ≤ \|V\| | §2.1 | §2 | all-terminal | k = \|T\| |
| Vertex before its incident edges ("vertex priority") | §2.3, Fig. 4 | vertex numbering | edges only | §3.1 |
| Failed vertex ⇒ incident edges unusable | §2.3 | §5.2 | — | §3.1 (R_EV) |

---

## 3. Method

### 3.1 Oracle and differential fuzzing

For graphs with V + E ≤ 16, we enumerate all 2^(V+E) component states. On each random graph
(2–7 vertices, up to 9 edges) we compare:
- `calculate_reliability` with the default ordering, BFS, DFS, and random orderings that respect
  vertex priority;
- `calculate_reliability` with fully random orderings, which must be rejected;
- `build_diagram`.

There are four modes:

| Mode | Graphs generated |
|---|---|
| `PAPER` | Exactly the references' assumptions: connected simple graphs, 2 ≤ \|K\|, all p in (0, 1) |
| `PAPER PERFECT` | As above, plus edges with p = 1 |
| `KMIN2` | Anything (disconnected graphs, loops, multi-edges, p ∈ {0, 1}) with \|K\| ≥ 2 |
| default | Anything, including \|K\| = 1 |

The modes separate defects from out-of-scope inputs (§4).

### 3.2 Targeted reproduction and profiling

Each defect has a minimal reproducing graph. Scaling was measured on ladders, meshes, random
geometric graphs and complete graphs, with callgrind profiles on a 7×7 mesh.

### 3.3 Reference reproduction

We reproduced Wu & Sun Table 2: the bridge (Fig. 1–3), N6–N9 (identified as meshes from |V| and
|E|), N20–N26 and N27–N28. All edges and vertices have probability 0.9, and the two terminals are
the most distant pair (opposite corners). Layer sizes are compared with the paper's B¹ in the
paper's own convention, which counts only the states that a layer's variable splits (§5.7).
The previous engine's partition numbering was checked against Carlier & Lucet's Algorithms 1–2
over all 142,417 partitions with |F| ≤ 10; the frontier engine stores block labels directly, so
that check no longer applies.

### 3.4 Behaviour-preservation protocol

Any change meant to affect only cost had to reproduce these exactly, on fixed benchmark sets:
- reliability values;
- peak layer size G^max and largest front;
- canonical BDD node counts;
- generated orderings, byte for byte (3,036 graphs).

### 3.5 Measurement protocol

- Release builds (GCC 13, `-O2`).
- Timings are best of 5 where differences were small.
- Each long run is a separate process with time and memory limits, so a blow-up is recorded
  rather than crashing the experiment.
- Peak resident memory comes from `getrusage`.
- Sanitizer builds (ASan + UBSan, halt on error) are run for every change, and ThreadSanitizer
  for the parallel code.
- Machine: a 4-core cloud VM. Repeated timings vary by about ±10 %; layer sizes and results are
  deterministic.

---

## 4. Audit findings

| # | Finding | Evidence | Root cause | Class |
|---|---|---|---|---|
| A1 | Test suite did not compile | build error | `decomp_state::union_find` renamed to `uf_`; CI never built tests | infrastructure |
| A2 | BDD connected through failed vertices | 0.75 → 1.0 on a 3-vertex graph; 545/10,000 in `PAPER PERFECT` | p = 1 shortcut ran before the "edge removed by vertex failure" check; violates R_EV | **defect** |
| A3 | \|K\| = 1 inconsistent | BDD 1.0 vs exact 0.3 | "connected" assumed for \|K\| < 2 | outside scope; undefined |
| A4 | Unsorted terminals gave wrong answers | 0.855 vs 0.405 | `binary_search` on an unvalidated list | unenforced precondition |
| A5 | Arbitrary custom orderings accepted | 0.81 / 0.0 vs 0.405 | vertex priority not checked (8,068/10,000 wrong) | unenforced precondition |
| A6 | Probability cutoff 1e-15 | 1.2e-6 relative error in 1−R; K₁₂ silently lost ~97 % of its states | hidden approximation | **defect** (inexact) |
| A7 | Parser accepted `O.9` as 0, trailing junk, negative counts; writer lost precision | repros | `operator>>` failure semantics; 15 digits | defect |
| A8 | Validation after use; movable object with dangling reference; pool not exception-safe | review | member order; destroy + placement-new | defect (latent) |
| A9 | Quadratic memory and time | 2×4000 ladder: 70 s, 2.76 GB | Stirling table sized V+E; per-level tables; whole-graph checks per state | performance |
| A10 | Ordering helpers polynomially slow | 40×40: 73 s | full edge scans; O(L²E) estimator | performance |
| A11 | State identity by 64-bit hash; partition numbers overflow beyond \|F\| = 25 | review | hash-only keys; B(26) > 2⁶⁴ | soundness risk |
| A12 | More states per layer than Wu & Sun on meshes | N20: 387,481 vs 53,924 | ordering (§5.4) | performance |
| A13 | Dead code, duplicated operations, wrong attribution, 15 warnings | review | — | quality |
| A14 | Layer sizes compared with the paper in a different convention | N20 65,858 vs B¹ 53,924; K₁₂ 1.8× | we counted every state of a layer, the paper only the states that still contain the layer's variable | measurement |
| A15 | Edges of a vertex emitted in edge-id order | 5×5: 146 or 180 states depending on that order; geo40_4: 418,948 vs 219,252 | order left open by the references | performance |
| A16 | Ordering score blind to how long a layer stays wide | perfect-vertex 9×9: 2.6× the work of BFS at the same peak | edge cut is a poor proxy with perfect vertices | performance |
| A17 | `graph` could hold invalid states (README §3.1) | unsorted terminals (A4), NaN probabilities, inconsistent adjacency | public data members | design |
| A18 | `sweep.cpp` stopped compiling with the old engine's removal; `probe.cpp` aborted | build | harnesses are outside the build | infrastructure |

Within the references' assumptions (`PAPER`), both methods were already correct (0/10,000). Every
correctness failure needed either an input outside those assumptions or a p = 1 edge. A14–A18
were found during the second phase of the work (§5.6–§5.10).

---

## 5. Interventions

Each intervention lists the alternatives considered and why the chosen one won. Commits are on
`feat/libgraphrel-optimisations` (Appendix A).

### 5.1 Correctness and preconditions (`a29ca5e`, `1ef297d`)

- **A2.** Check for an edge removed by a vertex failure *before* the p = 1 shortcut. Rejected
  alternatives: dropping the shortcut (it saves BDD variables), and pre-contracting p = 1 edges
  (a larger change).
- **A3.** Define R = p(t) for |K| = 1 on both paths, and reject |K| = 0. The existing test showed
  |K| = 1 was meant to be supported. Rejecting it, as the references do, remains a one-line option.
- **A4.** `validate()` now checks that terminals are sorted, unique and non-empty, rejects NaN,
  and checks edge ids and that adjacency lists are complete. A new `set_terminals()` normalises the
  list. Silently sorting a `const graph&` would have required a copy.
- **A5.** Custom orderings must contain every variable exactly once and respect vertex priority,
  checked in O(V+E). Fuzzing showed this, together with |K| ≥ 2, is the exact precondition:
  BFS, DFS and random vertex-priority orderings all gave 0/10,000 mismatches.
- **A6.** Exact by default. The cutoff becomes `calculate_reliability_bounds(g, threshold)`, which
  adds up the probability it drops and returns an interval [lower, lower + dropped] guaranteed to
  contain R. Rejected: keeping the cutoff (inexact with no bound), and exact-only (dense graphs
  lose a large speed-up for no gain in practice).
- **A7.** Tokenised strict parsing (`from_chars`, classic-locale reals, full token consumption),
  and `max_digits10` output.
- **A8.** Validate in constructor initialiser lists; delete copy and move operations.
- **Tests and CI (`9771441`).** Brute-force fuzz, precondition, parser and paper suites (about
  6 s); Linux GCC and Clang jobs that run them; an ASan/UBSan job with `halt_on_error`. The
  sanitizer job exposed a harness defect: the header-only and shared Boost.Test were mixed, giving
  two framework instances.

### 5.2 Removing quadratic costs without changing results (`0a43644`)

- Size the Stirling table by the largest static front, capped at 25, and throw beyond it rather
  than overflow.
- Replace the per-level boundary tables by two flat arrays.
- Make the ordering helpers O(V+E).
- Remove the whole-graph connectivity checks that are provably redundant: deleting a non-coloop
  edge cannot disconnect the terminals, and contracting an edge never does (Imai et al. Lemma 1).
  Debug builds assert the invariant.

Verification: identical outputs (§3.4). Result: 2×2000 ladder 11.6 s / 693 MB → 2.3 s / 5 MB;
ordering helpers on 100×100: more than 600 s → under 10 ms.

### 5.3 Cleanup (`195280d`, `63bf4fb`)

Dead code, the three copies of each graph operation, attribution (the BDD builder credited to
Hardy et al.), and warnings (15 → 0). The `state_pool` now recycles states by assignment: this is
exception-safe and reuses capacity, 11–20 % faster. Behaviour unchanged (§3.4).

### 5.4 Cut-width ordering (`7a98b40`)

**Observation.** On the 10×10 mesh, BFS and a row-by-row sweep have the same largest front
(F = 11) but 387,481 vs 65,858 states.

**Hypothesis.** With a row sweep, each front vertex keeps one unprocessed edge, so blocks touching
the front once are dead ends (Lemma 2) and merge. With BFS's diagonal sweep each front vertex keeps
two. The governing quantity should be the **edge cut**, the number of unprocessed edges leaving the
processed part: 11 vs 18.

**Heuristic.**
- Greedily take the reached vertex that grows the cut least (deg − 2·processed neighbours),
  preferring to continue the current line.
- Run it from the seed terminal, freely and forced through each neighbour (up to 16). The forced
  runs decide the sweep direction: the long side of a rectangle is catastrophic (4×12: 705,376
  states).
- Include BFS as a candidate.
- Choose the smallest (largest cut, largest front, total cut).

**Evaluation** (`ordering_lab.cpp`). 12 meshes and 12 random geometric graphs, with perfect and
imperfect vertices. Measured on the previous engine:

| Graph (vertices 0.9) | BFS states | Cut-width states |
|---|---|---|
| 10×10 mesh (N20) | 387,481 | 65,858 |
| 6×10 mesh | 4,582 | 891 |
| geo40_1 | 162,075 | 1,683 |
| geo40_2 | out of memory (> 6 GB) | 8,758 |
| geo60_1 | 1,329,800 | 54,528 |
| geo40_4 | 329,038 | 478,760 (**worse**) |

With perfect vertices, square meshes do up to 2.2× more total work, because the peak is the same
but lasts longer. The edge cut does not capture how long a layer stays wide.

**Decision.** Make cut-width the default. BFS's failures reach 24–96× or run out of memory, while
cut-width's worst case observed is 2.2×. BFS stays available to reproduce the paper's setup exactly.

### 5.5 Frontier-local engine (`6c37164`, `7916203`, `b389dcb`)

**Motivation.** The key already used only front information (Proposition 1), but each state
*stored* a union-find over all V vertices and a list of all remaining edges. Every copy and check
cost Θ(V+E), and the key needed a hash of the boundary vertex set (A11).

**State.** Per front position, one byte: 0 = absent (failed or pruned), otherwise a block label,
canonical by first appearance. Plus a 64-bit mask of terminal blocks. Size: 8 + F bytes. Keys are
compared byte for byte, so no hash collision can merge two states, and there is no Bell-number
ceiling. The front limit is 64, compared with 25 before.

**Static plan**, computed once:
- the front at each level;
- for each edge, its endpoints' positions and whether each endpoint leaves afterwards;
- remaining-edge counts;
- unprocessed edges between front vertices;
- terminals not yet reached;
- a look-ahead table (below).

**Transition** (Algorithm 1). Applying `ordering[l]` to a state:

```
vertex v:  works  -> v joins the front as a new block (terminal-marked if v ∈ K)
           fails  -> v ∈ K ? FAILURE : v is absent (its edges become unusable)
edge (u,v): if u or v absent, or same block  -> single child, state unchanged
           works  -> merge the blocks of u and v
           fails  -> unchanged
then, for each child:
  1. departures: an endpoint with no edges left leaves; a terminal block losing its last
     front vertex is SUCCESS if it is the only terminal block and no terminal is
     unreached, else FAILURE
  2. usable exits: an edge to an absent front vertex is unusable; an edge inside a block
     is internal. Until stable:
       terminal block with no exits      -> SUCCESS / FAILURE as in 1
       non-terminal block, <= 1 exit     -> absent                      (Lemma 2)
       vertex with no exit               -> absent (its block keeps the others)
  3. canonical relabelling
  4. look-ahead (only on levels where the unprocessed graph is disconnected): FAILURE if
     terminal blocks and unreached terminals cannot be joined through blocks and future
     components
  5. SUCCESS if all terminals are reached and lie in one block
```

**Two refinements, each motivated by measurement.**
- *Usable exits (step 2).* The first version counted exits statically. With vertex failures it
  kept 1.6× more states than the old engine (8×8: 8,866 vs 5,434). The cause: a front vertex
  whose remaining edges all lead to failed vertices was still treated as live. Counting only
  usable exits restored the old counts exactly. On 6×40 it also went *below* them (621 vs 801),
  because internal edges are now recognised.
- *Look-ahead (step 4).* On meshes it removed no states and cost 30 % in time. On geo60_1 it
  removed 24 % at no time cost. It can only remove a state where the unprocessed graph is split
  into several components, so the plan enables it only on those levels: no cost on meshes, and
  the gain is kept.

**BDD.** The same engine drives a two-pass construction. The forward pass records each state's
successors, 16 bytes per state. The backward pass creates the TeDDy nodes from the last layer up.
There is no recursion, so the stack-depth risk on MSVC is gone. Because a reduced, ordered BDD is
canonical, node counts must be identical to the old builder's for the same ordering. They are, on
every case checked (§6.2).

**Removal.** The old engine and its unit tests were deleted (`b389dcb`). Two new tests cover what
only the frontier engine can do: a front of 31 vertices (R = 1 − 0.5³⁰ exactly) and rejection of
fronts above 64.

### 5.6 Edge order within each vertex (`dea84ec`)

**Observation.** Vertex priority fixes when an edge becomes ready, not the order of the edges
that become ready together, after one vertex. That order changes the layer sizes but not the
result. On the 5×5 mesh, edge-id order gives 146 states at the widest layer and the reverse
gives 180; on K₁₀ the reverse is better (35,016 vs 42,107).

**Hypothesis.** An edge whose other endpoint has no other unprocessed edge makes that endpoint
leave the front as soon as it is processed. Its block is then closed or pruned (Lemma 2) before
the next branching, so fewer distinct states are created.

**Rule.** Sort the ready edges by the other endpoint: fewest unemitted edges first, then the most
recently emitted endpoint first.

**Evaluation** (`edge_order_lab.cpp`). The main set had 36 cases: 5 meshes, 12 random geometric
graphs and K₁₁, each with perfect and with imperfect vertices. A first pass also covered 7
smaller meshes and K₈–K₁₀. Five orders were compared: edge id, reversed, most recent endpoint
first, and the departing-first rule with either tie-break.

| Case | Edge-id order: widest layer / total states | Departing-first, newest first |
|---|---|---|
| geo40_4, vertices 0.9 | 418,948 / 7.12 M | 219,252 / 5.67 M |
| geo40_5, vertices 0.9 | 100,369 / 1.02 M | 50,723 / 0.74 M |
| geo40_8, perfect vertices | 1,306 / 24,452 | 622 / 16,710 |
| K₁₁, vertices 0.9 | 239,481 / 2.88 M | 201,959 / 2.53 M |
| 10×10 mesh, vertices 0.9 | 65,858 / 12,089,259 | 65,858 / 12,091,807 |

**Decision.** Departing-first, newest first. It never enlarged the widest layer, and total work
grew by at most 0.07 % (meshes) while falling by up to 1.5× elsewhere. The oldest-first
tie-break is marginally better on some rectangles (6×10: 684 vs 711) but gains nothing on
complete graphs. The rule applies to the cut-width ordering only. BFS and DFS keep edge-id
order, which is the order that reproduces the paper's counts (§5.7).

### 5.7 What the paper's layer size counts (`9be6bbe`)

**Observation.** On the 2×20 ladder (N9) the paper reports B¹ = 4. Every ordering we tried
peaks at 5 states, and a dump of that layer shows five states with five different futures. No
exact method can merge them, so the difference had to be in what is counted.

**Explanation.** We counted every state of a layer. Table 2's B¹ counts only the states that the
layer's variable splits into two different children. The library now reports both:
`max_layer_size` and `max_branching_layer_size`. The paper does not state its convention, so it
was identified from the numbers, which match on every network (below). Two observations are
consistent with it:
- In Wu & Sun, "when the vertex variable fails, the edges connected to it are removed from the
  network" (§2.3), so such a subgraph no longer contains those edge variables.
- B¹ is compared with B², the node count of a reduced BDD. In a reduced BDD, a level holds only
  the nodes whose function depends on that level's variable, which is exactly the branching
  states.

Fig. 3, by contrast, draws every subgraph of each layer. In its layer 5, G³₅ sends both branches
to G²₆, and G⁵₅ both to G⁴₆. It shows 5 subgraphs there: our count of all states for the bridge
is 5, and of branching states 3.

**Result.** With the paper's ordering (vertices row by row, each vertex's edges by id) every
published B¹ is reproduced exactly.

| Net | Graph | Paper B¹ | Branching states, ours |
|---|---|---|---|
| — | bridge, Fig. 1 | (Fig. 3 draws 5 subgraphs, all states) | 3 (all states: 5) |
| N9 | 2×20 | 4 | 4 |
| N6 | 3×10 | 12 | 12 |
| N7 | 3×12 | 12 | 12 |
| N8 | 5×5 | 118 | 118 |
| N20 | 10×10 | 53,924 | 53,924 |
| N21 | 11×11 | 191,216 | 191,216 |
| N22 | 12×12 | 684,114 | 684,114 |
| N23 | 13×13 | 2,466,428 | 2,466,428 |
| N25 | 1000×10 | 53,924 | 53,924 |
| N27 | K₁₂ | 816,465 | 816,465 |
| N28 | K₁₃ | 5,460,095 | 5,460,095 |

Two details had to be identified:
- **Mesh ordering.** The counts match a row-by-row sweep. A breadth-first search from a corner
  sweeps anti-diagonals (10×10: 316,303 branching states). The library's cut-width ordering
  produces the row sweep, so its counts equal B¹ on N20–N25 as well (N23 and N25 above were
  measured with it).
- **Terminals of the complete graphs.** Every pair of vertices of K_n is at "maximum distance",
  so the instance is defined only up to relabelling, and R does not depend on the choice. The
  layer sizes do. With the two terminals first in the order, K₁₂ gives 816,465 (the paper); with
  terminals {0, 11}, our test instance, it gives 970,727.

**Consequence.** The state definitions are identical. The "1.22×" (meshes) and "1.7–1.8×"
(complete graphs) gaps recorded earlier were this counting convention plus, for complete
graphs, the terminal placement. Our time advantage at equal state counts comes from the
implementation (§5.5, §5.8).

### 5.8 Parallel layer expansion (`42c0e7d`)

**Requirement.** Results must be **bit-identical for every thread count**. Floating-point sums
depend on their order, so a naive parallel merge would make the last digits of R depend on the
machine and on scheduling.

**Design.**
- *Production order.* Parent g of a layer yields children at positions 2g (fails) and 2g + 1
  (works).
- *Serial layers* are one table whose entries appear in production order.
- *Parallel layers* (at least 16,384 states) are split into 64 shards by the top bits of the key
  hash. In each round, every thread expands a contiguous run of parents into per-shard buffers.
  Each shard then takes its children run by run, so every key accumulates its probability in
  production order. The position that created each new state is marked in a bitmap, and the
  layer's order is rebuilt from the bitmap ranks. Every state therefore has the same position as
  in a serial run.
- *Sums of resolved branches* (success, pruned probability) are accumulated in fixed blocks of
  4,096 parents and then added in block order.
- *Threads* are created on the first large layer, so small inputs never start one. The API takes
  a `threads` argument: 1 by default, 0 for all hardware threads.

**Alternatives rejected.**
- A concurrent table with locks: the summation order would vary.
- Per-thread private tables merged at the end: the merge is serial and becomes the bottleneck.
- Iterating the shards one after another instead of rebuilding the order: measured 1.5× slower
  even serially. Consecutive parents in creation order tend to produce the same children, so
  their merges hit entries that are still in cache. Hash order destroys that locality.

**State table layout.** Each entry is one record [hash | probability | key], so a lookup touches
one slot and one record instead of three arrays.

**Verification.**
- Fuzzing through the threaded path with a one-state grain: 600 graphs × 1–4 threads, with and
  without pruning. Results are bit-identical and match brute force.
- Larger layers with several grains.
- ThreadSanitizer, ASan and UBSan are clean.

**Results.** See §6.4. On 4 cores the speed-up over one thread is 2.1–3.2× (N20 4.9 s → 2.2 s,
N23 604 s → 189 s, K₁₃ 61.8 s → 21.0 s). It is limited by the merge phase and by layers too small
to split. The per-thread buffers raise peak memory by 1.3–1.9×.

### 5.9 Ordering score with perfect vertices (`9361a6a`)

**Observation.** With perfect vertices the cut-width ordering lost to BFS on square meshes: the
same widest layer, but 1.3× (5×5) to 2.6× (9×9) more total work. A row sweep stays wide for the
whole sweep, while BFS's diagonal sweep is wide only in the middle.

**Measurement** (`level_lab.cpp`, `fit_levels.py`). We recorded the front size F, edge cut C
and layer size at every level: 14,532 levels from 22 graphs (meshes, random geometric graphs and
complete graphs), under BFS, DFS and cut-width orderings, in both regimes. A regression pooled
over all graphs mostly measures differences *between* graphs, which is not the decision being
made. With a per-graph offset (fixed effects), which keeps only differences between orderings of
the same graph:

| Regime | Fitted model |
|---|---|
| Imperfect vertices (0.9) | log₂ states = 1.51 F + 0.10 C + c(graph) |
| Perfect vertices | log₂ states = 1.61 F − 0.015 C + c(graph) |

With perfect vertices a front vertex is absent only once it is pruned, so the state count
follows the partitions of the front, and the cut adds nothing. With imperfect vertices the cut
picks the best candidate in every case measured.

**Rule.** If every vertex is perfect, rank the candidates by estimated work Σ over levels of
2^(1.6·F). Otherwise keep (largest cut, largest front, total cut).

**Evaluation.**
- *In sample* (35 graph/regime pairs, BFS vs cut-width): the worst loss against the better of
  the two fell from 2.57× to 1.20×, and the mean loss from 9.4 % to 0.7 %. Exponents from 1 to 2
  give the same choices.
- *Out of sample* (23 graphs not used to design the rule, all vertices perfect): none got worse,
  and six improved. 10×10: 2.9× less work. 11×11: 3.2×. 9×6: 1.26×. geo40_9: 2.6× (widest
  layer 1.6 M → 266 k). geo80_2: 6.6× (1.22 M → 417 k). With imperfect vertices the chosen
  orderings are identical, as intended.

### 5.10 `graph` as a class (`0d0f22e`)

**Problem.** `graph` exposed its vectors, so it could hold states the algorithms cannot handle:
- unsorted terminals (A4);
- NaN probabilities;
- adjacency lists that disagree with the edge list;
- `num_edges` promising more `add_edge` calls than were made.

Every entry point had to re-validate the graph, and any check it missed became a wrong answer.

**Design.**
- All members are private.
- Mutators are checked and throw before changing anything: `add_edge`, `set_vertex_prob`,
  `set_all_vertex_probs`, `set_edge_prob`, `set_terminals`.
- Accessors are read-only: `num_vertices()`, `edges()`, `vertex_probs()`, `terminals()`,
  `get_incident_edges(v)`.
- In `graph(n, m)`, `m` now only reserves space.

The one precondition left is a non-empty terminal set.

**Alternatives.**
- Keeping the struct and `validate()`: errors surface late, at use, and every new entry point
  must remember to validate.
- A builder producing an immutable graph: cleaner, but a larger API change, and incremental
  edits would need copies.

**Migration.**

| Before | After |
|---|---|
| `g.num_vertices`, `g.num_edges` | `g.num_vertices()`, `g.num_edges()` |
| `g.edges`, `g.vertex_probs`, `g.terminals` | `g.edges()`, `g.vertex_probs()`, `g.terminals()` (read-only) |
| `g.adj[v]` | `g.get_incident_edges(v)` |
| `g.vertex_probs[v] = p` | `g.set_vertex_prob(v, p)` |
| `for (auto& p : g.vertex_probs) p = x;` | `g.set_all_vertex_probs(x)` |
| `g.edges[e].prob = p` | `g.set_edge_prob(e, p)` |
| `g.terminals = {...}` | `g.set_terminals({...})` |

The library, tests, audit harnesses, the example and the scaling experiment were migrated.
Tests that built invalid graphs on purpose now check that the API refuses them. Two harnesses
had rotted (A18). `sweep.cpp` loses its partition-numbering mode, which checked the removed
engine. `probe.cpp` now reports the audit-time defects as rejected instead of aborting.

---

## 6. Results

### 6.1 Correctness

Brute-force fuzzing, 10,000 graphs per mode:

| Mode | LBL, original | BDD, original | LBL, final | BDD, final |
|---|---|---|---|---|
| `PAPER` | 0 | 0 | 0 | 0 |
| `PAPER PERFECT` | 0 | 545 | 0 | 0 |
| `KMIN2` | 0 | 9 | 0 | 0 |
| default (incl. \|K\| = 1) | 0 | 963 | 0 | 0 |

In the final version, BFS, DFS and random vertex-priority orderings all give 0 mismatches, and
every ordering that violates vertex priority is rejected. Sanitizer runs are clean throughout.

### 6.2 Canonical BDD check

| Case | Old builder: nodes, time, memory | Frontier builder: nodes, time, memory |
|---|---|---|
| 2×500 ladder | 3,492, 0.14 s, 49 MB | 3,492, 0.03 s, 40 MB |
| 6×6 mesh, vertices 0.9 | 15,639, 0.05 s | 15,639, 0.01 s |
| 7×7 mesh, vertices 0.9 | 76,522, 0.48 s, 23 MB | 76,522, 0.07 s, 11 MB |
| 8×8 mesh | 180,574, 1.23 s, 57 MB | 180,574, 0.14 s, 15 MB |
| 3×20 mesh, vertices 0.9 | 1,288 | 1,288 |
| 5×12 mesh | 6,358 | 6,358 |

### 6.3 Performance progression (LBL, exact)

| Case | Audit baseline | After §5.2 | After §5.4 | Frontier (§5.5) | Final (§5.6–§5.10): 1 thread / 4 threads |
|---|---|---|---|---|---|
| 2×2000 ladder | 17.9 s, 692 MB | 2.3 s, 5 MB | 1.5 s | < 0.01 s, 4 MB | < 0.01 s, 5 MB |
| 6×40 mesh | 7.5 s (with cutoff) | 3.0 s | 1.3 s | 0.05 s (621 states) | 0.05 s (621 states) |
| 8×8 mesh, vertices 0.9 | — | 3.7 s (BFS) | 1.66 s | 0.22 s, 4 MB | 0.2 s, 4 MB (layers too small to split) |
| 10×10 mesh (N20) | 200.8 s, 882 MB (BFS, with cutoff) | 145 s, 1.0 GB (BFS) | 63 s | 4.8 s, 11 MB | **4.9 s, 14 MB / 2.2 s, 25 MB** |
| K₁₂ (N27) | 2.2 s with cutoff (inexact) | 75.5 s, 1.2 GB | 75.5 s | 8.7 s, 164 MB | **6.9 s, 197 MB / 2.8 s, 253 MB** |
| geo40_4 | — | 32 s (BFS) | 45 s | 3.2 s | 2.1 s (widest layer 418,948 → 219,252, §5.6) |

The ladder was the one case that the parallel commit first made slower (18–28 ms), through
per-level overhead from the 64 shards. The fix (`e71d745`) resets and scans only the shards in
use and never moves the layers.

### 6.4 Reproducing Wu & Sun, Table 2

All edges and vertices have probability 0.9, the terminals are at maximum distance, and the
computation is exact. Wu & Sun's times are from an i5-7300 laptop; ours are from a 4-core cloud
VM, with the library defaults (cut-width ordering). B¹ is compared in the paper's convention
(§5.7); G^max counts all states of a layer.

| Net | Graph | Paper R | Our R | Paper B¹ | Our B¹ | Our G^max | Paper time | Ours, 1 thread | Ours, 4 threads | Peak memory (4 threads) |
|---|---|---|---|---|---|---|---|---|---|---|
| — | bridge, Fig. 1 | 0.760078728 | 0.760078728 | — (Fig. 3: 5 drawn) | 3 | 5 | — | < 0.01 s | — | 3 MB |
| N9 | 2×20 mesh | 0.230160 | 0.230160 | 4 | 4 | 5 | 0.0 s | < 0.01 s | — | 3 MB |
| N6 | 3×10 mesh | 0.609632 | 0.609632 | 12 | 12 | 15 | 0.0 s | < 0.01 s | — | 3 MB |
| N7 | 3×12 mesh | 0.579399 | 0.579399 | 12 | 12 | 15 | 0.0 s | < 0.01 s | — | 3 MB |
| N8 | 5×5 mesh | 0.711988 | 0.711988 | 118 | 118 | 146 | 0.0 s | < 0.01 s | — | 3 MB |
| N20 | 10×10 mesh | 0.714299 | 0.714299 | 53,924 | 53,924 | 65,858 | 42.9 s | 4.9 s | 2.2 s | 25 MB |
| N21 | 11×11 mesh | 0.714418 | 0.714418 | 191,216 | 191,216 | 233,206 | 215.1 s | 21.6 s | 8.8 s | 66 MB |
| N22 | 12×12 mesh | 0.714482 | 0.714482 | 684,114 | 684,114 | 833,340 | 1,003.4 s | 123.6 s | 42.8 s | 199 MB |
| N23 | 13×13 mesh | 0.714515 | 0.714515 | 2,466,428 | 2,466,428 | 3,001,316 | 4,722.2 s | 604.4 s ᵃ | 188.6 s | 584 MB |
| N25 | 1000×10 mesh | 0.673028 | 0.673028 | 53,924 | 53,924 | 65,858 | 8,066.5 s (540 MB) | 663.5 s ᵃ | 304.2 s | **33 MB** |
| N26 | 1200×10 mesh | 0.664961 | 0.664961 | 53,924 | 53,924 | 65,858 | 9,082.6 s (540 MB) | 787.4 s ᵃ | 370.9 s | **34 MB** |
| N27 | K₁₂ | 0.810000 | 0.810000 | 816,465 | 816,465 ᵇ | 1,240,964 | 72.9 s | 6.9 s | 2.8 s | 253 MB |
| N28 | K₁₃ | 0.810000 | 0.810000 | 5,460,095 | 5,460,095 ᵇ | 8,045,429 | 555.8 s | 61.8 s | 21.0 s | 1.4 GB |

ᵃ Measured at `0a8ad2b`, before the edge-order and table-layout changes, which leave these
meshes' layer sizes unchanged. The other 1-thread times are from the final version.
ᵇ With the two terminals first in the order, the paper's instance (§5.7). The other columns
use terminals {0, n − 1}, where B¹ is 816,287 (K₁₂) and 5,442,750 (K₁₃) and R is the same.

- Every R matches the paper to all published digits, and every B¹ is reproduced exactly.
- On one core the runs are 8–12× faster than the paper's; on four cores, 20–27× faster.
  Four threads give 2.1–3.2× over one (N23: 3.2×). The merge phase and layers too small to split
  limit the speed-up. The thread buffers raise peak memory by 1.3–1.9× (N23: 584 MB with 4
  threads, 362 MB with 1; N21: 66 MB and 34 MB).
- Memory is bounded by the widest layer: the 1000×10 and 1200×10 meshes need 33–34 MB, against
  540 MB in the paper.

---

## 7. Discussion

### 7.1 What explained the gaps to the paper

Every gap is now explained, and none was in the state definition:
1. **Ordering** (§5.4). The paper's meshes are swept row by row; a breadth-first sweep from a
   corner produces 5.9× more branching states on 10×10.
2. **The counting convention** (§5.7). B¹ counts only the states a layer's variable splits.
   Counted that way, every published B¹ is reproduced exactly, from 4 (N9) to 5,460,095 (N28).
3. **Terminal placement on complete graphs** (§5.7). It does not change R but does change the
   layer sizes.
4. **The cutoff**, which had made earlier counts look *better* than the paper's (K₁₂ appeared
   19× below it).
5. **Per-state overhead**, which the frontier engine removes (§5.5).

Since the state counts agree exactly, the time and memory differences reflect the
implementation: exact byte keys over the front only, a flat table with one record per state,
and parallel expansion.

### 7.2 Threats to validity

- The brute-force oracle covers graphs with V + E ≤ 16. Larger graphs are covered only indirectly:
  by agreement between independent methods and orderings, and by reproducing published values.
- The look-ahead and usable-exit rules are proved informally (Proposition 1, Lemma 2). Their
  soundness rests on those arguments and on the fuzzing.
- The ordering heuristics (cut, edge order, work model) were designed and validated on meshes,
  random geometric graphs and complete graphs. The out-of-sample checks used the same families;
  other topologies may behave differently. The work model's exponent was fitted, but the choices
  it makes do not change for exponents between 1 and 2.
- Graphs that mix perfect and imperfect vertices use the imperfect-vertex rule; this regime was
  not measured separately.
- Timing comparisons with the paper cross hardware generations. Parallel timings come from a
  4-core VM and vary by about ±10 % between runs.
- The CI workflow has been dry-run locally but not yet executed on GitHub.
- Hardy, Lucet & Limnios (2007) was not available: its open-access copy (HAL, hal-00019249) is
  blocked by the network policy of the environment used for this work. The BDD builder was
  checked against Carlier & Lucet, Imai et al. and Kawahara et al., and against itself through
  canonical node counts.

### 7.3 Lessons

1. Establish which reference governs which code before judging it.
2. Agreement between two implementations is not an oracle. The original tests compared LBL with
   BDD and passed while the BDD was wrong.
3. Separate defects from unenforced preconditions. Fuzzing modes that mirror the assumptions make
   this measurable.
4. Hidden approximations corrupt every measurement built on them, including the auditor's.
5. Prove that refactors preserve outputs: identical orderings, layer sizes and canonical node
   counts.
6. Measure before explaining. The state-count gap was first attributed to the state definition;
   it was the ordering.
7. Validate proxy metrics across regimes. The edge cut predicts states well with imperfect
   vertices, less well with perfect ones.
8. Unrun tests stop compiling, and sanitizers check the harness as well as the code.
9. Keep corrections in the record (Appendix D).
10. Before comparing with a published metric, reproduce its smallest instance. The 2×20 ladder
    (4 states vs 5) settled what the large instances could not: the paper counts differently.
11. Iteration order is a performance property. The same states visited in hash order instead of
    creation order cost 1.5×, through cache locality alone.
12. Make determinism a requirement of parallel numeric code from the start. Fixed blocks and
    production positions cost little and keep results identical across machines and thread
    counts.
13. Fit the effect that drives the decision. The decision is between orderings of one graph, so
    the model needs per-graph offsets; a pooled fit understated the cut and chose wrongly.
14. Validate heuristics on data not used to design them (§5.9: 23 unseen graphs).
15. Harnesses outside the build rot like unrun tests (A18). Compile them with every change.
16. Re-measure the small and thin cases after a change aimed at large ones. The parallel commit
    was measured on large layers; only the final sweep showed it had made a 10,000-level ladder
    2–3× slower (`e71d745` fixed it).

---

## 8. Conclusion and future work

libgraphrel is now exact by default and enforces its preconditions; most invalid inputs can no
longer be represented. Both methods run on a frontier-local engine, which has exact keys and
O(F) work per state. Its memory is bounded by the widest layer (LBL) or by the diagram itself
(BDD). Large layers are expanded in parallel with bit-identical results.

The engine reproduces every reliability and every layer size published by Wu & Sun. It runs
8–12× faster than the paper's implementation on one core, and 20–27× faster on four (N20–N28).

Future work:
- review the BDD builder against Hardy et al. (2007), once the paper is available (HAL
  hal-00019249);
- run the CI workflow on GitHub, which also covers the Windows/MSVC build;
- parallelise the BDD builder's forward pass (the backward pass calls TeDDy, which is
  single-threaded);
- measure the ordering rules on graphs that mix perfect and imperfect vertices, and on other
  topologies;
- extend the parallel speed-up beyond the merge phase, for example by partitioning the parents
  of a round by shard of their children.

---

## References

1. B. Wu, L. Sun. A novel layer-by-layer recursive decomposition algorithm for calculation of
   network reliability. *Reliability Engineering & System Safety* 244 (2024) 109968.
2. J. Carlier, C. Lucet. A decomposition algorithm for network reliability evaluation. *Discrete
   Applied Mathematics* 65 (1996) 141–156.
3. H. Imai, K. Sekine, K. Imai. Computational investigations of all-terminal network reliability
   via BDDs. *IEICE Trans. Fundamentals* E82-A(5) (1999) 714–721.
4. J. Kawahara, K. Sonoda, T. Inoue, S. Kasahara. Efficient construction of binary decision
   diagrams for network reliability with imperfect vertices. *Reliability Engineering & System
   Safety* 188 (2019) 142–154.
5. G. Hardy, C. Lucet, N. Limnios. K-terminal network reliability measures with binary decision
   diagrams. *IEEE Trans. Reliability* 56(3) (2007) 506–515.

---

## Appendix A. Commit map

| Commit | Content |
|---|---|
| `bca017f` | Audit report, fuzz and probe harnesses |
| `0f4e3be` | Audit: Wu & Sun preconditions, ordering experiments, Table 2 reproduction |
| `2eb0e4e` | Audit: checks against Carlier & Lucet, Imai et al., Kawahara et al. |
| `1ef297d` | Fix test suite compilation (A1) |
| `a29ca5e` | Correctness and preconditions (A2–A8), exact LBL, bounds API, tests |
| `9771441` | CI: graphrel tests on GCC and Clang, ASan/UBSan job |
| `0a43644` | Remove quadratic costs, outputs identical (A9, A10) |
| `e00cead` | Audit report status table |
| `3940bcd` | First version of this log |
| `7a98b40` | Cut-width ordering as default (A12) |
| `24138b1` | Log: ordering work |
| `195280d` | Cleanup (A13) |
| `63bf4fb` | `state_pool` exception safety and reuse (A8) |
| `dc32779` | Log: cleanup |
| `6c37164` | Frontier engine; LBL on frontier states (A11) |
| `7916203` | BDD built layer by layer on frontier states |
| `b389dcb` | Remove the whole-graph engine |
| `7cd1325` | This report, first paper-style version |
| `0a8ad2b` | Report: N23 result |
| `dea84ec` | Cut-width ordering: departing-first edge order (A15) |
| `9be6bbe` | Branching-state statistic; exact B¹ reproduction (A14) |
| `42c0e7d` | Parallel layer expansion, bit-identical; record-based state table |
| `9361a6a` | Work-based ordering choice with perfect vertices (A16) |
| `0d0f22e` | `graph` as a class (A17); harnesses repaired (A18) |
| `5ba247b`, `eaa262c`, `979199e` | This report: second phase, B¹ convention, final Table 2 |
| `e71d745` | LBL: no per-level shard overhead on serial layers |

## Appendix B. Reproducing the evidence

```bash
# Build and run all graphrel suites (needs libboost-test-dev, libfmt-dev)
cmake --preset linux-release -DLIBTEDDY_BUILD_TESTS=ON
cmake --build --preset linux-release
ctest --test-dir build -R graphrel --output-on-failure
```

```bash
# Sanitizers
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLIBTEDDY_BUILD_TESTS=ON -DLIBTEDDY_BUILD_GRAPHREL=ON -DLIBTEDDY_USE_SANITIZERS=ON
cmake --build build/asan --target graphrel-test
UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/asan -R graphrel --output-on-failure
```

```bash
# ThreadSanitizer on the parallel tests
cmake -S . -B build/tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLIBTEDDY_BUILD_TESTS=ON -DLIBTEDDY_BUILD_GRAPHREL=ON "-DCMAKE_CXX_FLAGS=-fsanitize=thread" "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread"
cmake --build build/tsan --target graphrel-test
TSAN_OPTIONS=halt_on_error=1 build/tsan/tests/graphrel-test --run_test=graphrel_fuzz_test/fuzz_parallel_is_deterministic,parallel_large_layers_are_deterministic
```

```bash
# Large brute-force fuzz (from the repository root; link against build/libgraphrel/libgraphrel.a)
g++ -std=c++20 -O2 -I. -Ilibgraphrel/include -Ilibgraphrel/src libgraphrel/audit/fuzz.cpp build/libgraphrel/libgraphrel.a -o fuzz -pthread
PAPER=1 ./fuzz 10000
```

```bash
# Wu & Sun Table 2 (from the repository root)
g++ -std=c++20 -O2 -I. -Ilibgraphrel/include -Ilibgraphrel/src libgraphrel/audit/table2.cpp build/libgraphrel/libgraphrel.a -o table2 -pthread
./table2 mesh 10 10 cutwidth            # library default
THREADS=4 ./table2 mesh 13 13 cutwidth  # 4 threads
./table2 mesh 5 5 paper                 # the paper's ordering: B1 = 118
./table2 complete 12 paper 1            # terminals {0, 1}: B1 = 816,465
```

```bash
# Ordering studies (§5.6, §5.9)
g++ -std=c++20 -O2 -I. -Ilibgraphrel/include -Ilibgraphrel/src libgraphrel/audit/edge_order_lab.cpp build/libgraphrel/libgraphrel.a -o edge_order_lab -pthread
LAB_PV=0.9 ./edge_order_lab geo40_4 cutwidth id rev new rem_new rem_old
g++ -std=c++20 -O2 -I. -Ilibgraphrel/include -Ilibgraphrel/src libgraphrel/audit/level_lab.cpp build/libgraphrel/libgraphrel.a -o level_lab -pthread
# the report's data set: 22 graphs x 3 orderings x 2 regimes, each run capped at 60 s and 4 GB
for pv in 0.9 1.0; do for c in mesh5x5 mesh6x6 mesh7x7 mesh8x8 mesh9x9 mesh6x10 mesh4x12 mesh12x4 mesh3x10 mesh10x3 geo40_1 geo40_2 geo40_3 geo40_5 geo40_6 geo40_7 geo40_8 geo60_1 geo60_4 K8 K9 K10; do for st in bfs cutwidth dfs; do (ulimit -v 4000000; LAB_PV=$pv timeout 60 ./level_lab $c $st); done; done; done > levels.txt
python3 libgraphrel/audit/fit_levels.py levels.txt
```

| Harness | Purpose |
|---|---|
| `fuzz.cpp` | Brute force vs LBL and BDD; orderings; modes `PAPER`, `PAPER PERFECT`, `KMIN2`, default |
| `table2.cpp` | Wu & Sun Table 2 networks: R, B¹, G^max, front size, total work, time, memory; `paper` ordering; `THREADS` |
| `ordering_lab.cpp` | Ordering candidates: cheap metrics vs exact G^max and total work (`LAB_PV` sets vertex p) |
| `edge_order_lab.cpp` | Orders of each vertex's edges (§5.6) |
| `level_lab.cpp`, `fit_levels.py` | Per-level front, cut and layer size; regression and selector evaluation (§5.9) |
| `probe.cpp` | One probe per audit finding (all now report the fix) |
| `sweep.cpp`, `order_sens.cpp`, `paper.cpp` | Audit-time experiments, ported to the current API |

The audit report ([`README.md`](README.md)) documents the findings as found at `39fe87d`.

## Appendix C. Glossary

| Term | Meaning |
|---|---|
| K | Terminal set; all terminals must work and be connected |
| LBL | Layer-by-layer decomposition (Wu & Sun 2024) |
| BS-BDD | Boundary-set BDD (Hardy et al. 2007) |
| Front F | Processed vertices with unprocessed edges (boundary set, elimination front) |
| G^max | Most states in one layer (`max_layer_size`) |
| B¹ | Wu & Sun's layer size: most states in one layer that the layer's variable splits (`max_branching_layer_size`) |
| Departing-first | Edge order within a vertex: edges whose other endpoint leaves the front first (§5.6) |
| Vertex priority | Each vertex precedes its incident edges in the ordering |
| Edge cut | Unprocessed edges with exactly one processed endpoint |
| Dead end | Non-terminal block with at most one usable exit (Lemma 2) |
| k-NR-IV, R_EV | K-terminal reliability with imperfect vertices; failed vertices disable their edges (Kawahara et al.) |

## Appendix D. Errata to earlier versions of the audit

| Earlier statement | Correction |
|---|---|
| "The BDD is wrong on about 10 % of graphs" | Mostly inputs outside both methods' scope (\|K\| = 1). Within the assumptions plus p = 1 edges: 545/10,000. |
| "K₁₂ is 19× below the paper thanks to coloop detection" | An artefact of the 1e-15 cutoff. Exactly, K₁₂ has 1,459,298 states (edge-id order). In the paper's convention, with the paper's terminal placement, it has 816,465, equal to B¹ (§5.7). |
| "N20 has 340,599 states" | Measured with the cutoff. Exactly: 387,481 (BFS). |
| "Row sweep brings N20 to 1.08× the paper" | Measured with the cutoff. Exactly: 65,858 states, and 53,924 in the paper's convention, equal to B¹ (§5.7). |
| "`lbl_calculator` can be copied" | It could be moved, not copied; the move had the dangling-reference problem. |
| "The G^max gap needs the boundary definition from [20]/[33]" | Resolved: it was the ordering (§5.4) and the counting convention (§5.7). |
| "Cut-width stays within 1.2–1.25× of the paper's B¹"; "on K₁₂ and K₁₃ our layers are 1.7–1.8× wider; the paper's state definition may merge more" | Both are the counting convention (and, for K_n, terminal placement). The state definitions are identical, and every B¹ is reproduced exactly (§5.7). |
| Commit `9361a6a`'s message: the work model was "measured per level over 1,900 levels" | The data set has 14,532 levels from 22 graphs (114 runs). The code comment was corrected; the fitted coefficients are as stated. |
