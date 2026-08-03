# R-PREDA Phase E/F Parallel Certificate Implementation

## 1. Scope and checkpoint status

This change implements the compiler and Native-runtime plumbing for:

```text
Phase A-D owning CFG / effects / Relay ICFG
        |
        v
Phase E: module-bound parallel relay certificates
        |
        v
schema-v4/v5 additive parallel_certificate extension
        |
        v
Phase F: observe/strict runtime certificate validation
```

The implementation is deliberately observational. It does **not** add a new
scheduler, reorder or fuse relays, change `prlrt::relay*` lowering, modify
`SimuTxn`, pre-route targets, change queue insertion, invoke Z3 at runtime, or
feed a certificate result into consensus/runtime execution semantics.

The source implementation is present. Section 15 records the executed build,
CTest, generated-C++ golden, and real-workload acceptance evidence.

## 2. Frozen certificate semantics

### 2.1 Relevant microtransaction

A pair is compared only when both relay occurrences belong to one runtime
invocation/current transaction context and have the same parent trace
transaction ID. A relay emitted by an ordinary synchronous helper is composed
into its caller's relevant microtransaction. An asynchronous relay handler
starts a new microtransaction and is not placed in its parent's direct
co-emission set.

Static pair construction therefore uses a root function plus its supported
synchronous closure. It does not fold the asynchronous handler closure into
that root's direct pair set.

### 2.2 Dynamic occurrence

A loop can produce more than one dynamic occurrence of one static relay site.
Phase E does not invent an occurrence correspondence. A pair whose relevant
context contains a loop is conservative `Unknown`. At runtime, repeated
occurrences remain valid for a `MutuallyExclusive` check (observing both sites
is still a mismatch). Occurrence-correspondence-dependent precedence and
target relations return `SkippedUnsupported` when a purported loop-free
certificate nevertheless has repeated runtime occurrences.

### 2.3 Pair relations

The emitted relation meanings are:

| Relation | Meaning |
| --- | --- |
| `MutuallyExclusive` | `Path(A) && Path(B)` is unsatisfiable for one relevant microtransaction. |
| `MustPrecedeAB` / `MustPrecedeBA` | In a complete, unique, loop-free CFG/ICFG context, the first site strictly dominates the second. |
| `CoEmissionIndependent` | Co-emission is satisfiable and equal targets under the joint path are unsatisfiable, subject to the effect/scope gate. |
| `ProvedMayAlias` | Z3 found a joint-path model in which the two target formulas are equal. |
| `PotentialConflict` | A conservative effect/scope rule prevents an independence proof. |
| `Unknown` | CFG, path, target, call-context, effect, loop, or solver evidence is insufficient. |

Mutual exclusion is not reported as a parallel opportunity. Target
independence is attempted only after the mutual-exclusion goal is `Disproved`
on an exact path formula, which is the required SAT witness for the joint
path. For a conservative path over-approximation, UNSAT remains a sound
mutual-exclusion proof, but SAT is not treated as feasible co-emission and
alias analysis stops conservatively.

### 2.4 Work and depth

The certificate keeps four separate quantities:

```text
direct_logical_work
    logical relay sites emitted by the current relevant microtransaction,
    including supported synchronous-helper composition

transitive_logical_work
    logical relay emissions in the asynchronous handler tree rooted here

physical_route_work
    concrete routed copies after fanout

relay_tree_depth
    longest asynchronous relay-spawn chain
```

For `relay@shards`, one static/dynamic logical emission contributes one unit of
direct logical work. Physical work contributes
`active_shard_count`. A broadcast handler that can itself emit a relay is
currently conservative `Unknown` for transitive logical work because collapse
of physically cloned handler subtrees into one logical subtree is not modeled.

## 3. Compiler integration and owning IR

`PredaRealListener::exitContractDefinition()` in
`oxd_preda/transpiler/PredaRealListener.cpp` completes the compiler passes in
this order:

```text
DefinePendingRelayLambdas and semantic call recording
  -> RelayProtocolCollector::BuildControlFlow()
  -> RelayProtocolCollector::Finalize()
  -> RelayProtocolCollector::BuildSummaries()
  -> RelayProtocolCollector::BuildRefinement()
  -> RelayProtocolCollector::BuildParallelCertificate()
```

This order ensures Phase E consumes the owning Phase A-D CFG/ICFG/effect facts,
resolved relay handlers, typed target formulas, refinement symbols, and
solver-independent constraints. It does not rediscover control flow from
listener order or source offsets.

`oxd_preda/transpiler/relay_protocol/RelayProtocolIR.h` owns the result as:

```cpp
certificate::ParallelRelayCertificate parallelCertificate;
```

The new owning certificate types are defined in
`relay_protocol/certificate/ParallelRelayCertificate.h`:

- `RelayPairCertificate`;
- `RelayLogicalWorkBound`;
- `RelayPhysicalWorkBound`;
- `RelayTreeDepthBound`;
- `FunctionParallelRelayCertificate`;
- `ParallelRelayCertificate`.

All function, pair, work, and proof IDs are stable strings derived from owning
function/site IDs. Pair and function arrays are sorted before emission.
Evidence arrays are sorted and deduplicated.

## 4. Pair candidate generation

The implementation is in:

```text
oxd_preda/transpiler/relay_protocol/certificate/
  RelayPairCandidate.h
  RelayPairCandidateBuilder.h/.cpp
```

`RelayPairCandidateBuilder::Build()` performs the following for every Phase-D
CFG root:

1. Loads `relayIcfg.synchronousReachableFunctions[root]` and forms the relevant
   synchronous closure.
2. Selects relay sites owned by that closure; asynchronous handler-only sites
   are excluded.
3. Sorts by stable relay-site ID and emits each unordered pair exactly once.
4. Reuses `relayIcfg.relaySiteRelations`, per-function analyses, or exact
   synchronous-composition analyses.
5. Records CFG node IDs, source locations, scope kinds, relay kinds, handler
   effect summaries, and target-formula support.
6. Sets `exactContext=false` when Phase A-D did not prove one unique context
   with complete dominance/reachability facts. A conservative status caused
   solely by explicitly represented relay-failure exits remains usable by the
   later path proof and is marked there as exact or over-approximated.
7. Sets `loopFreeContext=false` if either site or any member of the synchronous
   closure contains a loop.

Multi-call contexts, recursion, missing nodes, incomplete Phase A-D analyses,
and ambiguous loop occurrences are retained as candidates but classified
conservatively rather than silently discarded.

## 5. Path feasibility and mutual exclusion

### 5.1 Path construction and exactness

`RelayPathFormulaBuilder::Build()` in
`RelayPathFormulaBuilder.cpp` builds a composed, acyclic graph from:

- Phase-A function CFG nodes and edges;
- Phase-D `SyncCall` and `SyncReturn` edges;
- the root's supported synchronous closure;
- exact Boolean conditions and true/false edge polarity.

For an exact call, the local call-node fallthrough edge is removed and replaced
by the call/normal-return path. The builder rejects Unknown/Unsupported CFGs,
unresolved calls, opaque nodes, unsupported relevant edges, unknown branch
formulas, unmodeled call effects, and cyclic relevant slices. A Conservative
CFG is retained as a path over-approximation; it is flagged exact only when its
sole incompleteness reason is the explicitly represented PREDA relay-failure
exit. It computes the relay-site path with a deterministic topological
dataflow:

```text
Path(entry) = true
Path(node) = OR over predecessors(
    Path(predecessor) AND edge_predicate
)
```

`RelayPathFormula::feasibilityExact` and
`RelayPairProofPlan::coEmissionFeasibilityExact` distinguish exact paths from
over-approximations. `supportingCfgFactIds` records the nodes, edges, functions,
and interprocedural facts used.

### 5.2 Solver goal

`RelayCertificateConstraintGenerator::BuildMutualExclusion()` creates a
`RelayProofObligationKind::RelayMutualExclusion` solver goal:

```text
G = !(Path(A) && Path(B))
```

No Phase-E path constraint is selected as a solver assumption. Under the
backend's standard `SAT(A)` then `SAT(A && !G)` algorithm:

- `Proved` means the joint path is UNSAT and emits `MutuallyExclusive`, even
  for an over-approximation because UNSAT is monotone under over-approximation;
- `Disproved` supplies a co-emission SAT witness and enables alias analysis
  only when `coEmissionFeasibilityExact=true`;
- `Unknown`, `Unsupported`, timeout, or encoding failure remains conservative.

The new proof-obligation kinds are added in
`refinement/RelayProofObligation.h`, and
`refinement/solver/RelayProofRunner.cpp` explicitly supports them without
turning a goal into its own assumption.

## 6. Must-precede analysis

`RelayPrecedenceAnalyzer::Analyze()` consumes Phase-D analysis for the local
root or exact synchronous composition. This analysis is independent of the
optional Z3 backend, so a stock build can emit `MustPrecede`. It emits a result
only if:

- the relevant context is exact and unique;
- the pair is loop-free;
- Phase D says the sites are structurally co-reachable;
- reachability and dominance are complete (a Conservative status caused only
  by modeled relay-failure/non-terminating post-dominance exits is accepted,
  because post-dominance is not used by this proof);
- exactly one site strictly dominates the other.

It uses `PredaFunctionGraphAnalysis::dominators`; it never uses listener
discovery order, source offsets, site ordinal, or vector order. Early failure,
unknown control flow, recursion, multi-context composition, and loop ambiguity
prevent a proved precedence relation upstream through Phase A-D completeness
gates.

At runtime, the relation is checked against the parent-local
`RelayEmitTraceEvent::emissionSequence`, not against static ordinals.

## 7. Co-emission independence and alias evidence

### 7.1 Conservative effect gate

`RelayIndependenceAnalyzer::PassesIndependenceGate()` requires:

- complete handler-region effect summaries;
- neither relay is `global` nor `shards`;
- equal target scope kinds;
- no global state, unresolved external call, or unknown external effect;
- no unproved transitive relay emission by either handler;
- known, compatible target-formula sorts;
- both targets are in the root function until formal/actual parameter binding
  is implemented.

A failed gate yields `PotentialConflict`, `Unknown`, or `Unsupported` with an
explicit reason; it never yields an independence certificate.

### 7.2 Alias query

Only after the mutual-exclusion goal is `Disproved`,
`RelayCertificateConstraintGenerator::BuildTargetIndependence()` creates:

```text
G = (Path(A) && Path(B)) => target(A) != target(B)
```

Phase-E goals are self-contained and run without unrelated refinement
assumptions. Therefore:

- `Proved` means joint path plus equal targets is UNSAT and yields
  `CoEmissionIndependent`;
- `Disproved` means joint path plus equal targets is SAT and yields
  `ProvedMayAlias`;
- other statuses remain conservative.

`RelayIndependenceAnalyzer::ClassifyTargetIndependence()` preserves the Z3
projected counterexample. If the backend reports SAT without projected symbol
values, the certificate retains an explicit diagnostic witness instead of an
empty alias-evidence field.

Cross-function target proofs currently remain unsupported because a helper's
formal target formula cannot be equated to the caller's actual expression
without owning formal/actual binding.

## 8. Work and depth certificates

### 8.1 Direct logical work

`RelayWorkBoundAnalyzer::Analyze()` and its internal `WorkState::BuildDirect()`
reuse each `FunctionProtocol::summary` for local relay counts and compose
ordinary synchronous callees from Phase-D `SyncCall` edges.

- A complete local CFG plus exact summary may retain a symbolic exact count.
- An unconditional, loop-free synchronous call whose call node dominates the
  normal exit may contribute an exact callee count.
- A conditional helper call retains a finite upper bound but makes the exact
  value `Unknown` until path-cardinality composition exists.
- Recursive/unresolved/loop-indexed synchronous calls have no fabricated
  finite proof.

### 8.2 Transitive logical work

`WorkState::BuildTransitive()` recursively follows resolved asynchronous relay
handlers and computes a conservative form of:

```text
TW(f) = sum over direct site occurrences r (
    Count(r) * (1 + TW(handler(r)))
)
```

Finite literal loop trip counts contribute to the occurrence upper bound.
Async recursion, unresolved handlers, unknown loop bounds, and unsupported
sync composition produce `Unknown`. Logical work is never multiplied by active
shard count. As noted above, an all-shards handler that may relay is currently
`Unknown` rather than using an unsound physical-clone sum.

### 8.3 Physical route work

`WorkState::BuildPhysical()` emits the owning affine representation:

```text
constant_term + active_shard_count_coefficient * active_shard_count
```

Ordinary custom/global/next relays contribute constant routes.
`relay@shards` contributes the shard-count coefficient. A nested broadcast
whose descendants would require a nonlinear term such as
`active_shard_count^2` is `Unknown`, not a false constant.

### 8.4 Relay-tree depth

`RelayDepthBoundAnalyzer::Analyze()` recursively computes:

```text
Depth(f) = max over direct sites (1 + Depth(handler(site)))
```

It composes synchronous closures, rejects synchronous/async recursion without
a finite proof, and uses Phase-D complete CFG/effect facts plus dominance of
synchronous call occurrences to distinguish exact depth from a conservative
upper bound. Conditional relays and conditional helper calls do not receive a
false exact depth.

## 9. Certificate construction and manifest emission

`ParallelRelayCertificateBuilder::Build()` orchestrates:

```text
RelayPairCandidateBuilder
  -> RelayPathFormulaBuilder
  -> RelayCertificateConstraintGenerator
  -> RelayProofRunner / optional Z3
  -> RelayPrecedenceAnalyzer
  -> RelayIndependenceAnalyzer
  -> RelayWorkBoundAnalyzer
  -> RelayDepthBoundAnalyzer
```

`ParallelRelayCertificateEmitter::Emit()` serializes the owning IR.
`RelayManifestEmitter::Emit()` adds it without changing the root manifest
schema:

```json
{
  "schema_version": 5,
  "parallel_certificate": {
    "extension_schema_version": 1,
    "functions": [
      {
        "source_function_id": "Dapp.Contract::send(uint32,uint32)",
        "certificate_status": "Conservative",
        "reason": "some properties are proved while others remain conservative or unknown",
        "pair_relations": [
          {
            "certificate_id": "parallel.pair.<root>.<site-a>.<site-b>.conflict",
            "site_a": "<site-a>",
            "site_b": "<site-b>",
            "relation": "CoEmissionIndependent",
            "status": "Proved",
            "supporting_cfg_fact_ids": ["<cfg-node-or-edge-id>"],
            "supporting_constraint_ids": ["<target-relation-id>"],
            "supporting_solver_result_ids": [
              "<mutual-exclusion-obligation-id>",
              "<target-independence-obligation-id>"
            ],
            "solver_results": {
              "co_emission": {"backend": "z3", "status": "Disproved"},
              "relation": {"backend": "z3", "status": "Proved"}
            },
            "counterexample": [],
            "location": {
              "site_a": {"line": 10, "column": 8},
              "site_b": {"line": 11, "column": 8}
            }
          }
        ],
        "direct_logical_work": {
          "certificate_id": "parallel.bound.<root>.direct_logical_work",
          "status": "Complete",
          "exact": {"kind": "constant", "value": 2},
          "upper_bound": {"kind": "constant", "value": 2}
        },
        "transitive_logical_work": {
          "certificate_id": "parallel.bound.<root>.transitive_logical_work",
          "status": "Conservative",
          "exact": {"kind": "unknown"},
          "upper_bound": {"kind": "constant", "value": 4}
        },
        "physical_route_work": {
          "certificate_id": "parallel.bound.<root>.physical_route_work",
          "status": "Conservative",
          "bound_kind": "ParameterizedUpperBound",
          "constant_term": 1,
          "active_shard_count_coefficient": 1,
          "expression": "1 + 1 * active_shard_count"
        },
        "relay_tree_depth": {
          "certificate_id": "parallel.bound.<root>.relay_tree_depth",
          "status": "Conservative",
          "exact": {"kind": "unknown"},
          "upper_bound": {"kind": "constant", "value": 2}
        }
      }
    ]
  }
}
```

`direct_work_bound`, `transitive_work_bound`, and `depth_bound` are emitted as
stable aliases for compatibility with the compiler-side naming used during
development.

## 10. Loader, artifact binding, and backward compatibility

The runtime-owned records are defined in:

- `oxd_preda/runtime/relay_plan/BoundRelayManifest.h`;
- `oxd_preda/runtime/relay_plan/RelayPlanLoader.cpp`.

`ParseParallelCertificate()` accepts an absent extension for older manifests,
but if the extension exists it requires `extension_schema_version == 1` and
validates:

- known function and relay-site IDs;
- unique certificate IDs and evidence IDs;
- referenced CFG, constraint, and solver-obligation IDs;
- valid pair/status combinations;
- `MutuallyExclusive`: a `RelayMutualExclusion/Proved` result;
- `CoEmissionIndependent`: `RelayMutualExclusion/Disproved` plus
  `RelayTargetIndependence/Proved`;
- `ProvedMayAlias`: both goals `Disproved` plus non-empty counterexample;
- complete/conservative work records and machine-readable physical bounds;
- equality of the preferred work fields and their compatibility aliases.

The extension remains inside the existing manifest self-hash. Normal schema-v5
artifact-binding and module-identity verification therefore protect the new
certificate together with the rest of the manifest. The root schema remains
v4 for stock compiler-only output and v5 for bound runtime-manifest builds; no
root schema-v6 migration is introduced.

The existing optimizer does not consume `parallel_certificate`. The records are
immutable inputs to the trace validator only.

## 11. Phase F runtime validation

### 11.1 Runtime identity and grouping

The trace implementation uses:

- `RelayTraceContext::NextEmissionSequence()` for a lock-protected,
  parent-local logical emission order;
- `RelayTraceCollector::RegisterRelayCreation()` /
  `FinalizeRelayEmission()` to
  record owning site, function, target, occurrence, parent, and sequence data;
- `RelayTraceCollector::ExecutionSlice()` to select direct emissions/routes by
  parent trace transaction ID;
- `ChainSimulator::ValidateRelayTraceExecution()` to validate each completed
  relevant microtransaction;
- `ChainSimulator::_FinalizeRelayTraceDepthValidation()` to validate completed
  asynchronous trace forests.

A synchronous helper keeps its static site/function identity, while the
certificate is selected by the executing root function. A relay handler is a
new traced execution with a new transaction context.

### 11.2 Pair checks

`RelayTraceValidator::ValidateExecution()` implements:

| Check kind | Runtime rule |
| --- | --- |
| `certificate_mutual_exclusion` | Mismatch if both proved-exclusive sites appear under one parent. |
| `certificate_must_precede` | When the consequent occurs, the required predecessor must have a smaller parent-local emission sequence. |
| `certificate_coemission_independence` | If both sites occur, their owning scope targets must differ; one absent is `NotApplicable`. |
| `certificate_proved_may_alias` | Equal observed targets are recorded as `Passed`; unequal targets are `NotApplicable`, not a contradiction. |

Unknown/unproved relations are `SkippedUnsupported`. Repeated occurrences are
skipped for precedence and target relations because Phase E has no
occurrence-indexed proof; mutual exclusion is still checked.

Before a target-based pair is compared, `TargetObservationComplete()` requires
known and matching manifest/runtime scope kinds and the complete keyed target
width (`address` 36 bytes; integer scopes 4 through 64 bytes). Missing or
truncated target evidence is `SkippedUnsupported`, never an accidental
non-alias pass.

### 11.3 Work and depth checks

The validator reports:

```text
certificate_direct_work
certificate_transitive_work
certificate_physical_work
certificate_depth
```

Direct work counts logical emission events under one parent. Final trace-forest
validation walks asynchronous descendants and computes transitive logical
events, physical route events, and maximum relative depth. It requires every
descendant execution to be complete, successful, and independently resolved
against the bound module/opcode identity. Incomplete/failed/unknown trees are
`SkippedUnsupported`.

Parameterized physical bounds are evaluated as:

```text
constant_term + active_shard_count_coefficient * current_active_shard_count
```

with checked `uint64` arithmetic. Unknown or unbound values are never treated
as unconstrained finite bounds.

Legacy summary checks (`DirectCount`, `CountUpperBound`, and legacy `Depth`)
are skipped when `hasUnmodeledRelayReachableCall=true`; the composed Phase-E
direct/transitive/physical/depth checks still run. This prevents a synchronous
helper's relay from producing a false local-only `0 == 0` pass.

### 11.4 Observe and strict behavior

`observe` mode appends validation records to the existing JSON/JSONL report and
does not change execution. `strict` mode uses the existing
`RelayTraceCollector::StrictFailureLatched()` mechanism and reports failure
only at the simulator safe point in `simu_script.cpp`. No validation method
modifies routing, buffers, queues, worker scheduling, or contract state.

`RelayTraceReport::BuildJson()` includes certificate/property IDs, pair site
IDs, relation, both source locations, expected/actual values, module/manifest
identity, status, and diagnostic reason.

## 12. Changed files and real functions

### 12.1 New Phase-E compiler module

| File | Main types/functions |
| --- | --- |
| `certificate/RelayPairCandidate.h` | `RelayPairCandidate`, `RelayPairCandidatesByFunction` |
| `certificate/RelayPairCandidateBuilder.h/.cpp` | `RelayPairCandidateBuilder::Build()` |
| `certificate/RelayPathFormulaBuilder.h/.cpp` | `RelayPathFormula`, `RelayPathFormulaBuilder::Build()` |
| `certificate/RelayCertificateConstraintGenerator.h/.cpp` | `BuildMutualExclusion()`, `BuildTargetIndependence()` |
| `certificate/RelayPrecedenceAnalyzer.h/.cpp` | `RelayPrecedenceAnalyzer::Analyze()` |
| `certificate/RelayIndependenceAnalyzer.h/.cpp` | `PassesIndependenceGate()`, `ClassifyMutualExclusion()`, `ClassifyTargetIndependence()` |
| `certificate/RelayWorkBoundAnalyzer.h/.cpp` | `RelayWorkBoundAnalyzer::Analyze()`, direct/transitive/physical internal analyzers |
| `certificate/RelayDepthBoundAnalyzer.h/.cpp` | `RelayDepthBoundAnalyzer::Analyze()` |
| `certificate/ParallelRelayCertificate.h` | Owning pair/function/work/depth certificate IR |
| `certificate/ParallelRelayCertificateBuilder.h/.cpp` | `ParallelRelayCertificateBuilder::Build()` |
| `certificate/ParallelRelayCertificateEmitter.h/.cpp` | `ParallelRelayCertificateEmitter::Emit()` |

The table paths are relative to
`oxd_preda/transpiler/relay_protocol/`.

### 12.2 Compiler integration

| File | Main change |
| --- | --- |
| `oxd_preda/transpiler/CMakeLists.txt` | Compiles all certificate sources. |
| `oxd_preda/transpiler/PredaRealListener.cpp` | Runs certificate construction after CFG, summaries, and refinement. |
| `relay_protocol/RelayProtocolIR.h` | Owns `parallelCertificate`. |
| `relay_protocol/RelayProtocolCollector.h/.cpp` | Adds `BuildParallelCertificate()` and optional Z3 backend selection. |
| `relay_protocol/RelayManifestEmitter.cpp` | Emits additive `parallel_certificate`. |
| `relay_protocol/refinement/RelayProofObligation.h` | Adds `RelayMutualExclusion` and `RelayTargetIndependence`. |
| `relay_protocol/refinement/RelayRefinementEmitter.cpp` | Serializes the new obligation kinds and results. |
| `relay_protocol/refinement/solver/RelayProofRunner.cpp` | Runs self-contained Phase-E goals without circular assumptions. |

### 12.3 Runtime manifest and validation

| File | Main change |
| --- | --- |
| `oxd_preda/runtime/relay_plan/BoundRelayManifest.h` | Runtime-owned pair/work/function certificate records. |
| `oxd_preda/runtime/relay_plan/RelayPlanLoader.cpp` | `ParseParallelCertificate()` plus evidence/trust validation. |
| `oxd_preda/runtime/relay_plan/RelayPlanTests.cpp` | Extension parse, evidence, malformed-input, and compatibility tests. |
| `oxd_preda/simulator/relay_trace/RelayTraceTypes.h/.cpp` | Certificate check kinds/details and `emissionSequence`. |
| `oxd_preda/simulator/relay_trace/RelayTraceContext.h/.cpp` | `NextEmissionSequence()`. |
| `oxd_preda/simulator/relay_trace/RelayTraceCollector.h/.cpp` | Parent-local sequence capture and certificate result collection. |
| `oxd_preda/simulator/relay_trace/RelayTraceValidator.h/.cpp` | Pair, direct, transitive, physical, and depth validation. |
| `oxd_preda/simulator/relay_trace/RelayTraceReport.cpp` | Certificate diagnostics in JSON/JSONL. |
| `oxd_preda/simulator/relay_trace/RelayRuntimeTraceTests.cpp` | Injected pair/work/strict/isolation validation tests. |
| `oxd_preda/simulator/chain_simu.h/.cpp` | Per-execution validation and completed trace-forest aggregation. |
| `oxd_preda/simulator/simu_script.cpp` | Existing strict safe-point failure path. |
| `oxd_preda/simulator/CMakeLists.txt` | Builds the extended trace/runtime tests under existing feature gates. |

### 12.4 New PREDA certificate fixtures

```text
oxd_preda/transpiler/testcase/relay_protocol/certificate/
  pair_relations.prd
  control_flow.prd
  sync_contexts.prd
  work_depth.prd
```

The fixtures cover straight-line pairs, arithmetic distinct/same targets,
if/else, early return, bounded/unbounded loops, unique/repeated/overloaded and
recursive helpers, handler chains, async recursion, all-shards, and nested
broadcast.

## 13. Explicit skipped and unknown cases

The first certificate version intentionally withholds strong conclusions for:

- loops without occurrence-indexed path/target semantics;
- any loop in the relevant synchronous closure for pair certification;
- recursive or multi-call-context synchronous composition;
- unresolved/opaque/unsupported CFG nodes or edges;
- unknown Boolean path predicates or cyclic relevant ICFG slices;
- unknown/external/global/transitively relaying handler effects;
- global and all-shards target independence;
- cross-scope target comparisons;
- helper target proofs without formal/actual argument binding;
- incompatible or `Unknown` Formula IR sorts;
- Z3 disabled, timeout, `unknown`, or encoding error;
- recursive async handler depth/work without a finite bound;
- nested broadcast physical work requiring nonlinear shard-count terms;
- all-shards handlers with relay descendants for logical-subtree collapse;
- incomplete, failed, identity-ambiguous, or unfinished runtime trace forests;
- repeated runtime site occurrences under a loop-free certificate;
- runtime target/argument/guard Formula-IR replay, which still lacks complete
  independent PREDA ABI symbol binding.

Each case carries a reason and becomes `Unknown`, `Unsupported`,
`PotentialConflict`, `NotApplicable`, or `SkippedUnsupported`; it is not
silently converted into `Proved` or a finite value.

## 14. Boundary to later work

This checkpoint does not implement:

- a mutation framework;
- coverage-guided or large-scale scalability campaigns;
- occurrence-indexed loop certificates;
- formal/actual binding across synchronous helpers;
- nonlinear symbolic physical-work expressions;
- runtime Formula IR interpretation or runtime Z3;
- certificate-driven scheduling, parallel materialization, reordering, relay
  fusion, bulk relay ABI changes, target pre-routing, or cross-transaction
  batching.

Those are separate follow-on tasks. The existing third-tier optimizer also
does not consume these certificates.

## 15. Tests, commands, and acceptance evidence

### 15.1 Static certificate tests

Planned/implemented test entry:

```text
oxd_preda/transpiler/test/RelayProtocolIRTests.cpp
```

The compiler suite now compiles the real certificate fixtures and asserts:

- straight-line `MustPrecedeAB` with Z3 disabled;
- if/else `MutuallyExclusive`;
- `base` versus `base + 1u32` as `CoEmissionIndependent`;
- same-target `ProvedMayAlias` with a projected counterexample;
- unique, deterministic pair/bound evidence arrays;
- ambiguous synchronous call contexts and nested broadcast work remain
  conservative;
- early-return, bounded-loop, all-shards, recursive and composed-helper
  work/depth boundaries.

Executed commands and results:

```bash
env LD_LIBRARY_PATH=bin/bin_release \
  ctest \
  --test-dir build-phase-ad-stock \
  -R relay_protocol_ir --output-on-failure
# 1/1 CTest passed; relay_protocol_ir reported 45/45 cases.

env LD_LIBRARY_PATH=/tmp/rpreda-z3/z3/lib:bin/bin_release \
  /tmp/preda-phase-ef-z3-build/oxd_preda/transpiler/relay_protocol_ir_tests \
  oxd_preda/transpiler/testcase/relay_protocol
# 56/56 cases passed (Trace ON, Z3 ON).
```

### 15.2 Runtime manifest/validator tests

Test entries:

```text
oxd_preda/runtime/relay_plan/RelayPlanTests.cpp
oxd_preda/simulator/relay_trace/RelayRuntimeTraceTests.cpp
```

Runtime fixtures include mutual-exclusion injection, precedence order and
reversal, repeated occurrence rejection, independent equal/different targets,
may-alias observation, direct/transitive/physical/depth bounds, unknown bounds,
strict safe-point latching, identity incompleteness, reporting, and existing
multithreaded trace isolation.

Executed regression matrix:

| Configuration | Command/result |
| --- | --- |
| Trace OFF, optimization OFF, Z3 OFF | `ctest --test-dir build-phase-ad-stock -R relay_protocol_ir --output-on-failure`: 1/1 passed, 45/45 internal cases. |
| Trace ON, optimization OFF, Z3 OFF | `ctest --test-dir build-phase-ef-trace --output-on-failure`: 3/3 passed (`relay_protocol_ir`, `relay_runtime_trace_tests`, `relay_plan_tests`). |
| Trace ON, optimization OFF, Z3 ON | direct `relay_protocol_ir_tests` in `/tmp/preda-phase-ef-z3-build`: 56/56 passed. |
| Trace OFF, optimization ON, Z3 OFF | `ctest --test-dir build-gcc12 --output-on-failure`: 4/4 passed (`relay_protocol_ir`, plan, optimization, pending-txn batch). |
| Benchmark harness unit tests | `python3 -m unittest -v .../test_run_native_ab.py`: 8/8 passed. |

### 15.3 Recommended build/CTest matrix

```bash
# Stock compiler/runtime, no Z3.
cmake -S . -B build-phase-ad-stock -G Ninja \
  -DDOWNLOAD_3RDPARTY=OFF \
  -DDOWNLOAD_IPP=OFF \
  -DRPREDA_ENABLE_Z3=OFF \
  -DRPREDA_ENABLE_RUNTIME_TRACE=OFF
cmake --build build-phase-ad-stock -j1
ctest --test-dir build-phase-ad-stock --output-on-failure

# Trace validation build.
cmake -S . -B build-phase-ef-trace -G Ninja \
  -DDOWNLOAD_3RDPARTY=OFF \
  -DDOWNLOAD_IPP=OFF \
  -DRPREDA_ENABLE_Z3=OFF \
  -DRPREDA_ENABLE_RUNTIME_TRACE=ON
cmake --build build-phase-ef-trace -j1
ctest --test-dir build-phase-ef-trace --output-on-failure

# Z3-enabled compiler certificate build, using an existing local Z3_ROOT.
cmake -S . -B /tmp/preda-phase-ef-z3-build -G Ninja \
  -DDOWNLOAD_3RDPARTY=OFF \
  -DDOWNLOAD_IPP=OFF \
  -DRPREDA_ENABLE_Z3=ON \
  -DZ3_ROOT=/tmp/rpreda-z3/z3 \
  -DRPREDA_ENABLE_RUNTIME_TRACE=ON
cmake --build /tmp/preda-phase-ef-z3-build -j1
ctest --test-dir /tmp/preda-phase-ef-z3-build --output-on-failure
```

No normal build downloads Z3 as part of this feature, and no runtime target
links or invokes Z3.

### 15.4 Generated C++ invariance

The trace-disabled `relay_protocol_ir` suite executes nine exact generated-C++
goldens. Each fixture checks both byte size and FNV-1a-64 digest; all nine
passed in `build-phase-ad-stock` and again in the optimization-enabled,
trace-disabled `build-gcc12` configuration. Phase-E certificate construction
therefore does not change stock `prlrt::relay*` lowering output.

The certificate builder and emitter are sidecar-only; none of their results are
read by relay lowering.

### 15.5 Real PREDA workload validation

The required Native workload validation is:

```text
MillionPixel
AirDrop
```

Both fixtures were run with a fresh `HOME` in `off`, `observe`, and `strict`
modes. The common command shape was:

```bash
cd <repo-root>/bin/bin_release
env \
  HOME=/tmp/rpreda-phase-ef-workloads-final-viz/<workload>/<mode> \
  PATH=/path/to/gcc-12/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  ./chsimu <deterministic-fixture> \
  -count:<count> -addresses:128 -seed:88 -order:2 \
  -rpreda_trace:<off|observe|strict> \
  -rpreda_trace_report:<mode-directory>/trace.json \
  -viz:<mode-directory>/viz.html \
  -viz_templ:<repo-root>/oxd_preda/simulator/relay_optimization/benchmarks/viz_template.html \
  -stdout
```

`-rpreda_trace_report` was omitted for `off`. MillionPixel used
`MillionPixelDeterministic.prdts`, `count=128`; AirDrop used
`AirDropDeterministic.prdts`, `count=16` (16 source calls, 100 recipients per
call).

| Evidence | MillionPixel | AirDrop / `Token.transfer_n` |
| --- | ---: | ---: |
| source transactions | 128 | 16 |
| microtransactions | 256 | 1,616 |
| logical relay emissions | 128 | 1,600 |
| physical routes | 128 | 1,600 |
| relay executions | 128 | 1,600 |
| maximum observed depth | 1 | 1 |
| checks passed | 2,561 | 20,833 |
| checks mismatched | 0 | 0 |
| manifest load/binding/instrumentation failures | 0 / 0 / 0 | 0 / 0 / 0 |

Observe and strict counters were byte-for-byte equal for each workload.
Strict completed with exit code 0 and no latched mismatch.

Bound manifest evidence:

- MillionPixel module
  `ej3fszqtyc038s2b30ccy5nbsm3ypv5wttm7b3ae9fgmh045ybeg`, manifest SHA-256
  `8c3a7a2cf485a462ebf1233ac64bac78ce615ada76502d9db908e64e3d25e584`;
- AirDrop/Token module
  `7crek8w7nbmdqf4ktv1txjxzyf31xvx40zhtxdb9qw6ksszme750`, manifest SHA-256
  `85884325c4fe6be2317d36fb6ebd43f75399e68c768fa5009f857a12ab827d23`.

Both schema-v5 manifests had `binding_complete=true` and were accepted by the
runtime loader. MillionPixel emitted finite direct/transitive/physical/depth
upper bounds of one for `occupy` and exact zero bounds for its handler. The
AirDrop source loop is bounded by runtime `recipients.length()`, so Phase E
correctly leaves its direct/transitive/physical work Unknown and the 16 source
work checks are `SkippedUnsupported`; its depth upper bound is one and all
1,600 handler executions validate their zero-work/depth properties.

The existing benchmark normalizer compared state, the confirmed semantic
transaction multiset, and per-producer dependency shape across all three
modes. Off, observe, and strict matched exactly:

| Workload | state SHA-256 | semantic transaction SHA-256 | dependency-shape SHA-256 |
| --- | --- | --- | --- |
| MillionPixel | `b0a99bd04bc055cf3c0aa2f9124ce88ed3bd1043a67375941a444f4e9f980340` | `873a6efa2fb48beab82a04417e1cd18cc39781cbf4f4f3a7b3bba4df9ccd8719` | `448dee9c75e2ae7e03a07177333e61033e452d9afda982d78dac14fb7958ac2b` |
| AirDrop | `cb67915deffe9db658a7fa6778fc87345d6b95d96f2d74d7a41f726661613440` | `b487a1b5a4f365c296ddca72bd4d3c2a41a7aada6f68adca86a8546dcb8c0f7c` | `bf51d0ba59bad750b6b42d1ad294ecc6fdc99155596aa490af4689cc57c4503c` |

Archived reports for this run are:

```text
/tmp/rpreda-phase-ef-workloads-final-viz/millionpixel/observe/trace.json
/tmp/rpreda-phase-ef-workloads-final-viz/millionpixel/strict/trace.json
/tmp/rpreda-phase-ef-workloads-final-viz/airdrop/observe/trace.json
/tmp/rpreda-phase-ef-workloads-final-viz/airdrop/strict/trace.json
```

## 16. Current acceptance statement

The implementation contains the required Phase-E certificate IR/builders,
optional Z3 goal integration, additive manifest schema, runtime-owned loader,
and Phase-F observe/strict checks. It preserves the frozen conservative
boundaries and does not add a new execution path.

The required Phase-E/F implementation, regression matrix, generated-C++ golden
checks, and MillionPixel/AirDrop Native workload validation are complete. The
conservative cases listed in Sections 13 and 15.5 remain explicit follow-on
limits rather than silently proved properties.
