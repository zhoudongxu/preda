# R-PREDA coverage and scalability evaluation

This directory contains the read-only evaluation pipeline used for the
coverage, certificate-quality, runtime-weighted coverage, and scalability
experiments. It does not mutate PREDA contracts and does not change relay
lowering or runtime scheduling.

## Commands

From the repository root, use a trace-enabled Native build for the real
workloads and a separate profiling-only/Z3-enabled build for scalability.
The evaluation runner rejects a scalability driver built with runtime tracing
or runtime optimization enabled, so timing samples cannot silently include
those features.

```bash
python3 oxd_preda/tools/rpreda_evaluation/CoverageScalabilityRunner.py real \
  --config oxd_preda/tools/rpreda_evaluation/benchmarks.json \
  --output-root results \
  --work-root /tmp/rpreda-coverage-real \
  --path-prefix /path/to/gcc-12/bin \
  --library-path /path/to/z3/lib

python3 oxd_preda/tools/rpreda_evaluation/CoverageScalabilityRunner.py scalability \
  --config oxd_preda/tools/rpreda_evaluation/benchmarks.json \
  --analysis-driver bin/bin_release/rpreda_analysis_driver \
  --output-root results \
  --work-root /tmp/rpreda-scalability
```

`real` compiles the configured contracts, regenerates strict Native traces,
and writes `results/coverage` and `results/certificate`. `scalability`
generates deterministic source programs and starts a fresh isolated analysis
driver process for every warm-up and measured sample. Sampling is round-major:
three complete warm-up rounds precede five measured rounds, and every round uses
a recorded deterministic shuffle of all cases. This prevents case order from
being identical to host-time drift.

The explicit compiler `--path-prefix` is important on hosts whose system C++
toolchain cannot compile PREDA's C++17 Native contract support headers. It
must name the same supported compiler toolchain used for the PREDA build.

The checked outputs are:

- `results/coverage/coverage.{json,csv}`;
- `results/certificate/certificate_quality.{json,csv}` and two SVG figures;
- `results/scalability/scalability.{json,csv}` and the scalability SVG.

## Parallel-certificate extension

The `certificate_extension` subdirectory contains a separate experiment for
pair-wise `CoEmissionIndependent`, `MustPrecede`, and `MutuallyExclusive`
certificates, plus finite work/depth bounds.  Per the benchmark integration
decision, its five logical workloads add exported entry points to the four
existing contract sources; Token and AirDrop both use `Token.prd`.  Existing
entry points remain available, while the extension results are never merged
into the original coverage aggregate.

Run the full compile, strict-runtime, certificate, and mutation matrix with a
trace- and Z3-enabled build:

```bash
python3 oxd_preda/tools/rpreda_evaluation/certificate_extension/ParallelCertificateBenchmarkRunner.py \
  --config oxd_preda/tools/rpreda_evaluation/certificate_extension/parallel_certificate_benchmarks.json \
  --repo-root . \
  --library-path /path/to/z3/lib \
  --library-path /path/to/compiler/runtime/lib \
  --path-prefix /path/to/supported/compiler/bin
```

The publication artifacts are written independently to:

- `results/certificate_extension/certificate_extension.json`;
- `results/certificate_extension/certificate_extension.csv`.

Relay sites are resolved from source function, target expression, handler, and
scope metadata in each fresh manifest.  The runner never relies on collector
ordinals, which can drift when the original source files gain new relay sites.

For a fast audit of already-retained raw traces, pass
`--reuse-artifacts <semantic-run-root>`. Such a run is marked `Reused` in the
per-benchmark provenance and should not be presented as newly executed data.

## PREDA-native four-layer ablation

The analysis driver accepts `--analysis-mode site_scan|cfg_icfg|formula_smt|full`.
All four modes use the same profiling-enabled, Z3-enabled binary, with runtime
tracing and runtime optimization disabled. The modes respectively collect relay
sites, add CFG/ICFG/effect/summary analysis, add Formula IR and SMT queries, and
add parallel/resource certificates. They do not select a runtime scheduler.

Run one mode at a time, with a separate output/work directory per mode:

```bash
export LD_LIBRARY_PATH="$(pwd)/bin/bin_release:/path/to/z3/lib:${LD_LIBRARY_PATH:-}"
for mode in site_scan cfg_icfg formula_smt full; do
  python3 oxd_preda/tools/rpreda_evaluation/CoverageScalabilityRunner.py ablation \
    --analysis-mode "$mode" --warmups 3 --repetitions 5 \
    --output-root "results/ablation/$mode" \
    --work-root "results/ablation/$mode/work" || exit 1
done
python3 oxd_preda/tools/rpreda_evaluation/AblationReport.py \
  --run-root results/ablation --output-root results/ablation_report
```

The report checks retained raw processes and phase activation. It keeps the
original summaries and includes output-variable points, without turning a
solver timeout into a successful proof. It checks proof-input stability
separately from solver/certificate output stability. The report's source-depth
field is unavailable for `site_scan`, which builds no synchronous call graph.

Use end-to-end process time or driver pipeline time for a four-mode cost table.
The `analysis_total` timer starts after collection; it is not entered in
`site_scan`. Its zero value therefore does not mean that scanning costs zero.
Report the original stability warnings and mode order. Sequential mode runs
are susceptible to host drift and do not establish a causal speedup.

## Denominators (coverage and scalability)

The separate [`shared_queries`](shared_queries/README.md) study adds a fixed
semantic-query denominator, independently constructed source-level answers,
four mode-local answer adapters, and paired incremental-effectiveness tables.
It complements the cost ablation; solver-goal counts and certificate-object
counts are not used as correctness labels. Run instructions and the precise
query semantics are in that directory. Existing contract slices, authored
parallel extensions, source mutants, and controlled programs are reported
separately. This study does not measure runtime scheduling performance.

- Static real-program totals deduplicate by the manifest-bound `module_id`.
  AirDrop is a `Token.transfer_n` workload, so it remains an RQ3 row but does
  not count as a second static contract.
- Runtime pairs are grouped within one parent microtransaction. The report
  keeps both all occurrence pairs and eligible distinct-site pairs. A zero
  distinct-site denominator is `null`, never 0% or 100%.
- Conservative finite upper bounds count as sound bound coverage; exactness is
  retained separately in certificate records.
- Schema-v5 has no refinement depth formula. The report says so explicitly
  and points to the separate relay-tree depth certificate.
- `formula_ir_constraint_count` includes semantic definitions established by
  construction. It is not mislabeled as solver input. Actual solver-facing
  volume is reported separately as solver goals and assumption references.
- Each timing point reports min, quartiles, median, max, IQR, and a descriptive
  interpolated p95 over five samples. The p95 is not a tail-latency estimate.
- Relay width and depth cannot both be held fixed at constant relay-site count.
  Coupled dimensions are explicit; every width point uses the same 17-function
  floor so function count does not drift within that sweep.
