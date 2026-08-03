# R-PREDA Phase A-D Intermediate Implementation Report

## 1. Checkpoint status

This checkpoint implements Phase A through Phase D of the R-PREDA third-tier task:

```text
PREDA parse tree + semantic resolution
        |
        v
Phase A: owning PREDA-native function CFG
        |
        +-------------------------------+
        |                               |
        v                               v
Phase B: synchronous call graph   persistent relay protocol IR
        |                               |
        +---------------+---------------+
                        |
                        v
              Phase C: effect summaries
                        |
                        v
                 Phase D: Relay ICFG
                        |
                        v
          additive manifest control_flow section
```

Phase E has **not** been started. In particular, this checkpoint does not emit a parallel-safety certificate, does not add certificate proof obligations, and does not let the scheduler or runtime consume any Phase A-D result.

The implementation is observational compiler metadata only. It does not change PREDA contract semantics, generated `prlrt::relay*` calls, `SimuTxn`, relay serialization, target evaluation, scope-key hashing, shard routing, queue order, worker scheduling, block construction, or runtime success/failure behavior.

## 2. Implementation boundary

The new owning IR is stored in:

```cpp
RelayProtocolIR::controlFlow
```

Collection is finalized in `PredaRealListener::exitContractDefinition()` after `DefinePendingRelayLambdas()`, while both of the following are still alive:

- PREDA ANTLR parse-tree nodes;
- exact semantic `FunctionRef` and overload-resolution results.

The integration copies parse-tree and semantic facts into owning data structures, invokes `RelayProtocolCollector::BuildControlFlow()`, and only then performs the existing protocol finalization. No analysis result is fed back into C++ lowering.

## 3. Phase A: PREDA-native function CFG

### 3.1 Owning IR

`oxd_preda/transpiler/relay_protocol/cfg/PredaCFG.h` defines:

- `PredaControlFlowIR`;
- `PredaFunctionCFG`;
- `PredaCFGNode` and `PredaCFGEdge`;
- `PredaCFGRegion`;
- `PredaFunctionGraphAnalysis`;
- shared `EffectSummary`, call-graph, and ICFG records.

The implemented node kinds are:

```text
Entry, Exit, Basic, Branch, LoopHeader, LoopLatch,
Break, Continue, Return, SynchronousCall, RelayEmit,
AbortOrFailure, Opaque
```

The implemented edge kinds are:

```text
Fallthrough, TrueBranch, FalseBranch, LoopBack,
BreakExit, ContinueBack, ReturnExit, CallToCallee,
ReturnToCaller, ExceptionalOrFailure, Unknown
```

Every source node owns a stable ID, source-function ID, source location, relay/callee identity where applicable, predecessors, successors, enclosing-loop IDs, direct effects, support status, and an explicit reason for unsupported behavior.

### 3.2 CFG construction

`PredaCFGBuilder::Build()` directly consumes PREDA parse-tree statements plus compiler-resolved semantic facts. `FunctionBuilder` in `PredaCFGBuilder.cpp` models:

- straight-line sequences;
- `if / else if / else`;
- `for / while / do while`;
- loop headers, latches, `break`, and `continue` targets;
- `return`, including return inside a loop;
- ordinary synchronous calls;
- relay statements and generated relay-lambda bodies;
- local and state assignments;
- relay target, named-argument, and lambda-capture expression effects;
- opaque or unsupported constructs.

For a `for` loop, calls in the update expression are materialized in the CFG. Normal body completion and `continue` both flow through that update subgraph before the latch/condition.

Nested expression calls use source-span containment to prove inner-before-outer evaluation. Disjoint sibling calls are not ordered using listener or source discovery order: the CFG emits an unsupported `Unknown` edge and becomes conservative.

### 3.3 Failure paths and completeness

Expressions with possible implicit runtime failure, including indexing and division/modulo operations, mark the source node `mayAbortOrFail`. Runtime/external helpers and relay emission are also conservatively failure-capable.

Each failure-capable node has an `ExceptionalOrFailure` edge to a separate terminal `AbortOrFailure` sink. The sink is not connected to the normal function `Exit`. Consequently, an exceptional callee path cannot incorrectly return to a caller continuation through a `SyncReturn` edge.

Per-function analysis records separate completeness for:

- dominance;
- post-dominance;
- reachability.

An entry-reachable node that cannot reach the normal exit makes post-dominance incomplete. An entry-reachable cycle without a compiler-proved termination fact also makes post-dominance incomplete, and control-dependence results that require complete post-dominance are withheld.

The persisted function status is one of:

```text
Complete, Conservative, Unsupported, Unknown
```

Incomplete analysis always carries a machine-readable reason; it is never silently promoted to `Complete`.

## 4. Phase B: PREDA synchronous call graph

### 4.1 Exact semantic call facts

`FunctionCallGraph` now owns `ResolvedFunctionCall` and `ResolvedIdentifierUse` events. A function identity is the exact pair:

```text
(DefinedIdentifier*, overloadIndex)
```

`ExpressionParser` records a call only after the existing PREDA overload-resolution path has selected a real `FunctionRef`. Group/member expression handling preserves callable provenance. No callee is guessed from a function-name string.

The call categories are:

```text
Synchronous
Relay
ExternalUnknown
CompilerGeneratedHelper
RuntimeHelper
```

If callable provenance or a local body is unavailable, the edge is explicitly unresolved or external, includes a reason, and is analyzed conservatively. It is not treated as a pure local helper.

### 4.2 Call-graph analysis

`PredaCallGraphAnalyzer::Analyze()` computes:

- strongly connected components;
- recursive functions;
- topological order of acyclic components;
- reverse callers;
- unresolved outgoing-call flags;
- relay-reachable propagation.

Generated relay lambdas are registered after `DefinePendingRelayLambdas()`, so their generated semantic identity and body participate in the same call graph as ordinary PREDA functions.

## 5. Phase C: conservative effect summaries

`PredaEffectAnalysis::Build()` computes direct and transitive effects for functions and source regions. The owning `EffectSummary` contains:

```text
reads/writes current-scope state
reads/writes global state
writes local state
modifies loop induction variable
may emit relay
may call unknown
may return early / break / continue
may abort or fail
may have external effect
read/written state-variable ID sets
status and reason
```

Exact identifier-use events recover state reads and writes from semantic identifiers and scope flags. This includes relay operands and caret-capture state reads that do not have a standalone expression context.

Synchronous callee effects are propagated to callers with a finite monotone fixed point. Recursive SCCs therefore converge conservatively; unresolved, external, and runtime calls retain unknown/external/failure effects rather than being treated as pure.

Region summaries are emitted for:

- function;
- branch arm;
- loop body;
- synchronous call site;
- relay handler;
- relay region.

Missing or unresolved relay handlers produce an `Unknown` region effect with an explicit external-effect reason.

## 6. Phase D: Relay ICFG

`RelayICFGBuilder::Build()` combines per-function CFGs, the synchronous call graph, and persistent relay-handler edges.

The implemented interprocedural edge kinds are:

```text
SyncCall
SyncReturn
AsyncRelaySpawn
```

Their semantics are:

```text
caller call node -> callee Entry                 (SyncCall)
normal callee Exit -> caller continuation        (SyncReturn)
relay emit node -> relay handler Entry           (AsyncRelaySpawn)
```

`AsyncRelaySpawn` never creates a return edge to the emitting function. Terminal failure sinks also never create `SyncReturn` edges.

Phase D persists:

- per-function dominance, post-dominance, control dependence, and reachability;
- loop nesting;
- synchronous and asynchronous function closures;
- synchronous-call SCCs;
- exact synchronous composition analyses where a unique supported call context exists;
- structural relay-site `CoReachable`, `MutuallyExclusive`, or `Unknown` relations.

Synchronous composition removes the direct call-to-continuation shortcut, inserts the callee CFG, and returns only from the normal callee exit. Cross-function relay relations are produced only when all relevant topology and reachability facts are complete, call contexts are unique, and recursion/unknown external behavior is absent.

The Phase D relation is a structural control-flow result, not a Phase E safety certificate and not a path-feasibility proof. Multiple call contexts, recursion, different roots, unsupported topology, incomplete reachability, or unknown effects produce `Unknown`.

## 7. Changed files

### 7.1 New Phase A-D module

| File | Main responsibility |
|---|---|
| `oxd_preda/transpiler/relay_protocol/cfg/PredaCFG.h` | Owning CFG, call graph, effect, analysis, and ICFG records |
| `PredaCFGBuilder.h/.cpp` | PREDA parse-tree CFG construction and direct effect extraction |
| `PredaCFGAnalysis.h/.cpp` | Dominance, post-dominance, control dependence, reachability, loop nesting, completeness |
| `PredaCallGraphAnalysis.h/.cpp` | SCC, recursion, reverse callers, topology, relay reachability |
| `PredaEffectAnalysis.h/.cpp` | Interprocedural fixed-point and region effect summaries |
| `RelayICFGBuilder.h/.cpp` | Sync/async ICFG edges, closures, composition, structural relay relations |
| `PredaCFGEmitter.h/.cpp` | Additive JSON serialization |

### 7.2 Integration changes

| File | Change |
|---|---|
| `oxd_preda/transpiler/FunctionCallGraph.h/.cpp` | Owning exact call and identifier-use semantic events |
| `oxd_preda/transpiler/ExpressionParser.h/.cpp` | Preserve callable identity and record post-resolution call facts |
| `oxd_preda/transpiler/PredaRealListener.cpp` | Register ordinary/lambda bodies and build owning control-flow input before collector finalization |
| `oxd_preda/transpiler/relay_protocol/RelayProtocolCollector.h/.cpp` | Capture semantic conditions and invoke `BuildControlFlow()` |
| `oxd_preda/transpiler/relay_protocol/RelayProtocolIR.h` | Add owning `controlFlow` field |
| `oxd_preda/transpiler/relay_protocol/RelayManifestEmitter.cpp` | Emit additive `control_flow` object |
| `oxd_preda/transpiler/CMakeLists.txt` | Compile the new module explicitly |
| `oxd_preda/transpiler/test/RelayProtocolIRTests.cpp` | Phase A-D assertions and regressions |

### 7.3 New PREDA fixtures

```text
cfg_phase_ad.prd
cfg_recursive_call.prd
cfg_overloaded_calls.prd
cfg_nested_loop.prd
cfg_abort_helper.prd
cfg_for_update_call.prd
cfg_relay_state_effect.prd
cfg_call_order.prd
cfg_nonterminating_for.prd
cfg_sync_composition.prd
cfg_lambda_sync_effect.prd
cfg_mutual_recursion_effect.prd
cfg_implicit_failure.prd
```

## 8. Manifest shape

The root manifest remains schema v4 in the stock configuration and schema v5 in a module-bound build. This preserves compatibility with the existing strict schema-v5 runtime loader. Phase A-D is an additive compiler-only extension:

```json
{
  "schema_version": 5,
  "relay_sites": [],
  "handlers": [],
  "edges": [],
  "functions": [],
  "refinement": {},
  "control_flow": {
    "extension_schema_version": 1,
    "implemented_phases": ["A", "B", "C", "D"],
    "functions": [
      {
        "function_id": "RelayProtocolTests.Contract::send(address)",
        "status": "Complete",
        "entry_node_id": "...::entry",
        "exit_node_id": "...::exit",
        "nodes": [
          {
            "id": "...::relay::0",
            "kind": "RelayEmit",
            "relay_site_id": "...",
            "supported": true,
            "direct_effect": {
              "may_emit_relay": true,
              "may_abort_or_fail": true
            }
          }
        ],
        "edges": [],
        "regions": [],
        "effect": {}
      }
    ],
    "synchronous_call_graph": {
      "functions": [],
      "edges": [],
      "analysis": {
        "strongly_connected_components": [],
        "recursive_functions": [],
        "topological_components": [],
        "reverse_callers": {},
        "has_unresolved_outgoing": {},
        "relay_reachable": {}
      }
    },
    "region_effects": [],
    "relay_icfg": {
      "interprocedural_edges": [],
      "function_analyses": [],
      "synchronous_composition_analyses": [],
      "relay_site_relations": [],
      "synchronous_reachable_functions": {},
      "async_reachable_handlers": {},
      "synchronous_sccs": []
    }
  }
}
```

The example is abbreviated; actual records contain source locations, predecessor/successor lists, condition Formula IR, state-variable sets, analysis completeness flags, and explicit reasons.

## 9. Soundness policy

The implementation follows these conservative rules:

1. Listener discovery order is never used as a proved execution order.
2. Overloaded callees are identified only by compiler-resolved `FunctionRef` identity.
3. Unresolved/external calls are not assumed pure and receive unknown/external/failure effects.
4. Unsupported expressions or control constructs remain represented as `Opaque`/`Unknown` with source locations and reasons.
5. Exceptional paths terminate at a separate failure sink and cannot return normally.
6. A cyclic CFG has no complete post-dominance result unless termination has been proved.
7. Recursion is represented through SCCs and is never assumed finite.
8. Cross-function composition requires a unique, exact synchronous call context.
9. Structural relay-site relations are not promoted to Phase E certificates.
10. No analysis output changes lowering or runtime behavior.

## 10. Validation results

Validation was run on 2026-07-31 with the repository's GCC 12 conda build environment.

### 10.1 Main module-bound configuration

```text
relay_protocol_ir_tests: 42 / 42 passed
```

This includes the prior persistent IR, summaries, dependency/refinement regression suite plus Phase A-D coverage for:

- ordinary and generated-lambda CFGs;
- exact overloaded synchronous calls;
- direct and mutual recursion;
- nested loops, `break`, `continue`, return, and update-expression calls;
- state effects in relay operands;
- nested-call ordering and sibling-call conservatism;
- runtime/implicit failure paths;
- opaque/unknown calls;
- non-terminating-cycle post-dominance conservatism;
- synchronous composition, multiple call contexts, and cross-function relay mutual exclusion;
- recursive effect fixed-point propagation.

The complete configured CTest suite also passed:

```text
4 / 4 tests passed
  relay_protocol_ir
  relay_plan_tests
  relay_optimization_tests
  pending_txns_batch_tests
```

### 10.2 Stock configuration

An independent build with runtime optimization, runtime trace, and Z3 disabled passed:

```text
relay_protocol_ir: 1 / 1 passed
```

This checks that Phase A-D remains usable without optional runtime/solver features and that the stock schema-v4 behavior is retained.

### 10.3 Generated C++ invariance

The existing golden checks compare exact generated-C++ byte length and FNV-1a hash for nine representative relay fixtures. All golden checks passed in the 42-test run. Therefore the Phase A-D metadata path did not alter generated C++ for those fixtures.

`git diff --check -- oxd_preda/transpiler` also completed without whitespace errors.

## 11. Known conservative limits at this checkpoint

- Disjoint sibling expression calls are `Unknown` because their evaluation order is not proved by the current semantic facts.
- Runtime helpers and external calls retain conservative failure/external effects unless a reviewed semantic summary is added later.
- State passed to a call is conservatively treated as potentially written when the current PREDA semantic API cannot prove the argument non-mutating.
- Cross-function structural relations require a unique synchronous call context; multiple call sites degrade to `Unknown`.
- Recursion and any reachable cycle without a termination proof disable complete post-dominance claims.
- `CoReachable` is structural graph reachability, not a proof that both sites execute on one feasible path.
- No path-sensitive alias proof, effect-conflict proof, vacuity check, or solver-backed parallel certificate exists yet; those belong to Phase E and later phases.

## 12. Deliberate pause before Phase E

Phase A-D now provide the owning control-flow and interprocedural substrate required by Phase E. This checkpoint intentionally stops here for calibration.

No Phase E certificate schema, proof generator, Z3 certificate goal, runtime validator, mutation framework, or evaluation pipeline has been implemented as part of this checkpoint.

