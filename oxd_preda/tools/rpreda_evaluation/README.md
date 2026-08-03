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

For a fast audit of already-retained raw traces, pass
`--reuse-artifacts <semantic-run-root>`. Such a run is marked `Reused` in the
per-benchmark provenance and should not be presented as newly executed data.

## Denominators

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
