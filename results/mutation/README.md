# R-PREDA mutation-study validation run

This directory contains a deterministic validation run with one generated
mutant for each of the 13 required relay mutation operators. The run used seed
`88`, a schema-v5 relay manifest, the Z3-enabled compiler analysis, parallel
certificate comparison, and strict runtime trace validation.

## Published artifacts

- `mutation.json`: complete classifications, per-stage evidence, denominators,
  unsupported checks, coverage gates, and negative-control result.
- `mutation.csv`: one compact row per mutant.
- `baseline/oracle.relay_protocol.json`: the explicit original-program oracle.
- `generation/`: the original and mutated PREDA sources plus generation
  metadata for all mutants.

Large compiler homes, process logs, and raw traces are intentionally not
published. Their paths are represented as `not-retained/...`; rerunning
`MutationRunner.py` recreates them under the chosen output directory.

## Result snapshot

| Measure | Result |
|---|---:|
| Generated / evaluable mutants | 13 / 13 |
| Baseline-oracle protocol differences | 13 / 13 (100%) |
| Certificate regressions | 4 / 13 (30.77%) |
| Runtime strict mismatches | 1 / 13 (7.69%) |
| Z3 safety-goal disprovals | 0 / 13 |
| Detected beyond compiler/static comparison | 4 / 13 (30.77%) |
| Infrastructure-free full runs | 12 / 13 (92.31%) |
| Final `Unsupported` / `Survived` | 0 / 0 |
| Original-control flags | 0 / 1 |

The four certificate regressions are `RelayDuplicate`, `RelayOrderSwap`,
`IntroduceAlias`, and `IntroduceRelayRecursion`. The recursive mutant also
reached its newly inserted relay and terminated with `GasUsedUp`; it is a
`RuntimeStrictMismatch`, not a timeout. Five trace instrumentation errors from
the non-quiescing execution are retained as a later-stage infrastructure
failure, so only 12 of 13 mutants completed every stage without infrastructure
failure.

The baseline fixture executes four source transactions and observes 13 logical
relay emissions. Its coverage gate reaches all five relay-source functions and
all 11 static relay sites. Every mutant has a mutation-specific coverage gate,
including the newly inserted recursion site.

## Reading the rates correctly

The 100% number is a deterministic smoke-test score against an explicit copy of
the original protocol. It is not the independent Z3, certificate, or runtime
detection rate, and it is not a paper-level estimate over real benchmark
contracts. The deeper-stage score is reported separately.

The strict false-positive rate is `null`, because the only negative control has
unsupported Z3/runtime obligations and therefore is not a fully usable control
under the strict denominator. The broader original-control flag rate is `0%`
(`0 / 1`). All mutants likewise contain at least one unsupported solver or
runtime check, so `fully_analyzable_mutants` is `0`; these unsupported checks
are recorded rather than silently counted as passes.

Strict validation against a mutant's own freshly compiled manifest checks
compiler/runtime self-consistency. It does not establish equivalence to the
original source.
