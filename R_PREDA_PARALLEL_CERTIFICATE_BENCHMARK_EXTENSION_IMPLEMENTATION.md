# R-PREDA Parallel Certificate Benchmark Extension Implementation

## 1. Outcome

The parallel-certificate extension study is implemented and validated on five workload slices:

- `TokenParallel`
- `BallotParallel`
- `MillionPixelParallel`
- `KittyParallel`
- `AirDropParallel`

The final isolated run reports:

| Metric | Result |
|---|---:|
| Configured workloads | 5 |
| Completed workloads | 5/5 |
| Certificate records | 14 |
| Validated certificate records | 14/14 |
| Directed negative mutations | 10 |
| Detected directed mutations | 10/10 |
| Baseline strict-runtime workloads | 5/5 passed |
| Directed-mutant strict-runtime runs | 10/10 passed |
| Manifest schema | 5 |
| Artifact bindings | 5/5 complete |

Certificate coverage added by this study is:

| Certificate type | Records |
|---|---:|
| `CoEmissionIndependent` | 6 |
| `MustPrecede` | 2 |
| `MutualExclusive` | 1 |
| `WorkBound` | 4 |
| `DepthBound` | 1 |

The publishable evidence is written separately from the original coverage study:

- `results/certificate_extension/certificate_extension.json`
- `results/certificate_extension/certificate_extension.csv`

Both products are generated through a path-redaction gate and contain no absolute user-home paths.

## 2. Implementation decision and scope boundary

The benchmark-design document proposed putting every extension in a separate `.prd` file. During implementation, the user explicitly chose to retain the extensions directly in the existing real workload sources. That later instruction overrides the design document's separate-file default.

The resulting source layout is therefore:

| Logical workload | Physical source | Extension entry point |
|---|---|---|
| TokenParallel | `oxd_preda/simulator/contracts/Token.prd` | `transfer_parallel(address,address,bigint)` |
| BallotParallel | `oxd_preda/simulator/contracts/Ballot.prd` | `vote_parallel(bool,address,address,address,address,uint64)` |
| MillionPixelParallel | `oxd_preda/simulator/contracts/MillionPixel.prd` | `occupy_parallel(uint16,uint16,uint32,address)` |
| KittyParallel | `oxd_preda/simulator/contracts/Kitty.prd` | `breed_parallel(address,address,address,bigint,bool)` |
| AirDropParallel | `oxd_preda/simulator/contracts/Token.prd` | `transfer_n_parallel(address,bigint,address,bigint,address,bigint,address)` |

Token and AirDrop intentionally share one contract module but are selected by different source-function signatures. The study treats them as separate logical workload records.

The existing exported entry-point bodies remain present. However, adding fields and functions changes the compiled contract artifact, state schema, relay ordinals, and some generated relay-lambda opcodes. This is not a byte-for-byte preservation of the original contract modules. Compatibility changes and coverage slicing are described below.

## 3. Validation method

The extension runner performs the following steps independently for every workload:

1. Compile the current PREDA source in a fresh simulator repository.
2. Require a schema-v5 manifest with a complete artifact binding.
3. Resolve relay sites by semantic attributes:
   - source function signature;
   - target expression text;
   - target function, when specified;
   - relay kind;
   - target scope;
   - occurrence number.
4. Reject configuration that embeds a hard-coded `relay_site_N` selector.
5. Validate the expected pair or bound certificate and its evidence ownership.
6. Run the workload with strict runtime tracing and require complete function/site coverage.
7. Generate the configured directed source mutation.
8. Compile the mutant, compare its manifest with the baseline manifest, and require the expected certificate regression.
9. Run the mutant under strict runtime tracing and require complete mutated-site coverage.
10. Emit the isolated JSON and CSV products.

The proof interpretation is deliberately conservative:

- independence uses exact path conditions, target-relation constraints, handler-effect checks, and Z3 target non-aliasing;
- mutual exclusion uses exact acyclic CFG path formulas and a Z3 unsatisfiability proof for joint emission;
- precedence comes from strict dominance in an exact loop-free ICFG;
- finite bounds remain valid when the exact count is path-dependent, provided an owning finite upper-bound expression exists;
- mutation detection compares the baseline and mutant certificates instead of treating source-text change alone as evidence.

## 4. Workload implementations and real relay graphs

The site identifiers below are the identifiers bound in the final validation run. The runner itself locates sites semantically and does not depend on these ordinals.

### 4.1 TokenParallel

Business motivation: a token transfer commonly updates the receiver and also records an audit, accounting, or compliance event.

The extension uses the existing address-scoped balance and adds audit-count, audit-volume, deposit, and audit handlers. For a successful non-negative transfer, the source scope debits the local balance and emits two relays to distinct address targets.

```text
Token.transfer_parallel @ source address
  guard: amount >= 0 && balance >= amount && to != audit
  local: balance -= amount
  |
  +-- relay_site_2 @to
  |     -> parallel_deposit(amount)
  |
  `-- relay_site_3 @audit
        -> parallel_record_audit(amount, 1)
```

Established properties:

- `CoEmissionIndependent(relay_site_2, relay_site_3)` because the targets are distinct under the joint path and both handlers have complete partition-local effects.
- `MustPrecedeAB(relay_site_2, relay_site_3)` because the receiver relay-emission node strictly dominates the audit relay-emission node in the exact loop-free ICFG.

Important boundary: this `MustPrecede` certificate is relay-to-relay. It proves receiver relay emission before audit relay emission. It does **not** directly certify the local `balance -= amount` statement against the audit relay, and it does not assert that the receiver handler finishes before the audit handler executes.

### 4.2 BallotParallel

Business motivation: DAO-style voting may update per-voter reputation, aggregate statistics, and branch-specific YES/NO counters.

```text
Ballot.vote_parallel @ source address
  |
  +-- if voter != statistics
  |     +-- relay_site_0 @voter
  |     |     -> parallel_update_reputation(amount)
  |     `-- relay_site_1 @statistics
  |           -> parallel_update_statistics(amount)
  |
  +-- if choice
  |     `-- relay_site_2 @yesStatistics
  |           -> parallel_update_yes(amount)
  |
  `-- if !choice
        `-- relay_site_3 @noStatistics
              -> parallel_update_no(amount)
```

Established properties:

- `CoEmissionIndependent(relay_site_0, relay_site_1)` under `voter != statistics`.
- `MutuallyExclusive(relay_site_2, relay_site_3)` because `choice && !choice` is unsatisfiable.

The two branch relays use separate `if` statements intentionally. This preserves explicit positive and negative path predicates and lets `GuardNegate` turn one branch into a co-emittable path for the negative study.

This entry point is a focused parallel update slice. It does not replace the original proposal-finalization workflow.

### 4.3 MillionPixelParallel

Business motivation: a pixel-ownership application can update a pixel partition and a separate statistics partition for the same operation.

```text
MillionPixel.occupy_parallel @ source address
  index = uint32(x) * 65536 + uint32(y)
  guard: ownerStatisticsKey != index
  |
  +-- relay_site_1 @ownerStatisticsKey : uint32 scope
  |     -> update_owner_statistics()
  |
  `-- relay_site_2 @index : uint32 scope
        -> occupy_pixel_parallel(sender)
```

Established property:

- `CoEmissionIndependent(relay_site_1, relay_site_2)` because both targets have the same fixed-width `uint32` scope sort and the guard proves them unequal on every joint-emission path.

The arithmetic pixel key remains structurally derived from `uint32(x) * 65536u32 + uint32(y)` through local-definition expansion.

Semantic boundary: `ownerStatisticsKey` is an application-provided `uint32` statistics partition, not an address-scoped owner identity. The pixel handler writes the extension field `parallelPixelOwner`; it does not implement the complete original `Land.occupied/Land.owner` transaction. The `sender` argument is also caller-provided. Consequently, this workload is a real-contract-derived parallel certificate slice, not a claim of full semantic equivalence to the original `occupy` operation.

### 4.4 KittyParallel

Business motivation: an NFT breeding action naturally exposes sibling updates for an offspring and two parents, together with a small finite work/depth bound.

```text
KittyBreeding.breed_parallel @ source address
  guard:
    offspring != parent1 &&
    offspring != parent2 &&
    parent1 != parent2
  |
  +-- relay_site_1 @offspring
  |     -> parallel_create_offspring(genes, gender)
  +-- relay_site_2 @parent1
  |     -> parallel_update_parent()
  `-- relay_site_3 @parent2
        -> parallel_update_parent()
```

Established properties:

- three pairwise `CoEmissionIndependent` certificates;
- direct logical work upper bound `3`;
- transitive logical work upper bound `3`;
- physical route work upper bound `3`;
- relay-tree depth upper bound `1`.

The raw bound status is `Conservative`, because a failed guard can bypass all relays. The upper bounds are nevertheless finite and owning, so the extension result records them as proved upper-bound properties rather than exact unconditional counts.

Semantic boundary: the extension handlers update counters and the last supplied gene value. They do not create a complete Kitty object in `allKitties/myKitties`, perform the original gene calculation, or reproduce the original nested breeding/registration chain. This is a bounded sibling-update model derived from the Kitty domain, not full equivalence to the original breeding business logic.

### 4.5 AirDropParallel

Business motivation: an airdrop distributes value to recipients and then records campaign or accounting summary data.

```text
Token.transfer_n_parallel @ source address
  total = amount0 + amount1 + amount2
  guard: every amount >= 0 && total <= balance
  local: balance -= total
  |
  +-- relay_site_4 @recipient0 -> parallel_deposit(amount0)
  +-- relay_site_5 @recipient1 -> parallel_deposit(amount1)
  +-- relay_site_6 @recipient2 -> parallel_deposit(amount2)
  `-- relay_site_7 @summary    -> parallel_record_audit(total, 3)
```

Established properties:

- `MustPrecedeAB(relay_site_6, relay_site_7)` for the last distribution relay and the summary relay;
- direct logical work upper bound `4`.

The workload is deliberately a fixed-cardinality three-recipient specialization. It avoids claiming occurrence-indexed precedence through the original dynamic array loop, which the current certificate analysis does not model. The `summary` target is an address parameter and is not statically constrained to equal the transaction sender or current source-scope key. Therefore the result demonstrates distribution-to-summary ordering and finite work, but not authenticated sender accounting.

## 5. Certificate results

All 14 records passed evidence validation. `Effective status` is the status used by the isolated study; `Raw status` is the underlying manifest property status.

| Workload | Record | Property | Site(s) or bound | Result | Raw status | Effective status |
|---|---|---|---|---|---|---|
| TokenParallel | `token.receiver_audit.independence` | CoEmissionIndependent | site 2, site 3 | targets proved distinct | Proved | Proved |
| TokenParallel | `token.receiver_audit.precedence` | MustPrecede | site 2 -> site 3 | `MustPrecedeAB` | Proved | Proved |
| BallotParallel | `ballot.voter_statistics.independence` | CoEmissionIndependent | site 0, site 1 | targets proved distinct | Proved | Proved |
| BallotParallel | `ballot.yes_no.mutual_exclusion` | MutualExclusive | site 2, site 3 | joint path UNSAT | Proved | Proved |
| MillionPixelParallel | `million_pixel.pixel_owner_stats.independence` | CoEmissionIndependent | site 1, site 2 | targets proved distinct | Proved | Proved |
| KittyParallel | `kitty.offspring_parent1.independence` | CoEmissionIndependent | site 1, site 2 | targets proved distinct | Proved | Proved |
| KittyParallel | `kitty.offspring_parent2.independence` | CoEmissionIndependent | site 1, site 3 | targets proved distinct | Proved | Proved |
| KittyParallel | `kitty.parent1_parent2.independence` | CoEmissionIndependent | site 2, site 3 | targets proved distinct | Proved | Proved |
| KittyParallel | `kitty.direct_work.bound` | WorkBound | direct logical work | upper bound 3 | Conservative | Proved |
| KittyParallel | `kitty.transitive_work.bound` | WorkBound | transitive logical work | upper bound 3 | Conservative | Proved |
| KittyParallel | `kitty.physical_work.bound` | WorkBound | physical route work | upper bound 3 | Conservative | Proved |
| KittyParallel | `kitty.depth.bound` | DepthBound | relay-tree depth | upper bound 1 | Conservative | Proved |
| AirDropParallel | `airdrop.distribution_summary.precedence` | MustPrecede | site 6 -> site 7 | `MustPrecedeAB` | Proved | Proved |
| AirDropParallel | `airdrop.direct_work.bound` | WorkBound | direct logical work | upper bound 4 | Conservative | Proved |

The complete CFG fact IDs, target-relation constraint IDs, solver-result IDs, source locations, and artifact digests are retained in `certificate_extension.json`. The CSV provides the compact paper-analysis projection.

## 6. Directed mutation study: 10/10 detected

Ten certificate-owned negative mutations were selected. Every mutation was generated, compiled, exercised with complete strict-runtime site coverage, and detected through the expected certificate regression.

| # | Workload | Protected property | Mutation | Mutated site(s) | Required regression | Result |
|---:|---|---|---|---|---|---|
| 1 | TokenParallel | receiver/audit independence | IntroduceAlias | site 2, site 3 | independence lost | Detected |
| 2 | TokenParallel | receiver-before-audit | RelayOrderSwap | site 2, site 3 | precedence reversed | Detected |
| 3 | BallotParallel | voter/statistics independence | IntroduceAlias | site 0, site 1 | independence lost | Detected |
| 4 | BallotParallel | YES/NO exclusion | GuardNegate | site 2 guard | mutual exclusion lost | Detected |
| 5 | MillionPixelParallel | statistics/pixel independence | IntroduceAlias | site 1, site 2 | independence lost | Detected |
| 6 | KittyParallel | offspring/parent1 independence | IntroduceAlias | site 1, site 2 | independence lost | Detected |
| 7 | KittyParallel | offspring/parent2 independence | IntroduceAlias | site 1, site 3 | independence lost | Detected |
| 8 | KittyParallel | parent1/parent2 independence | IntroduceAlias | site 2, site 3 | independence lost | Detected |
| 9 | KittyParallel | work upper bound 3 | RelayDuplicate | site 1 | work bound increases to 4 | Detected |
| 10 | AirDropParallel | recipient2-before-summary | RelayOrderSwap | site 6, site 7 | precedence reversed | Detected |

Four additional bound records are positive-only evidence rather than separate negative mutations:

- Kitty transitive work;
- Kitty physical route work;
- Kitty depth;
- AirDrop direct work.

The Kitty `RelayDuplicate` mutation raises direct, transitive, and physical work from `3` to `4`. It does not increase sibling relay-tree depth, so the depth record is intentionally not reported as a mutation kill. AirDrop `RelayOrderSwap` preserves cardinality, so its work-bound record is likewise positive-only.

Strict runtime passing for a mutant does not contradict mutation detection. The runtime validator checks each mutant execution against that mutant's own freshly compiled manifest. The negative oracle separately detects that the mutant lost or reversed a baseline certificate.

## 7. Strict runtime validation

### 7.1 Baseline extension workloads

| Workload | Compile | Strict runtime | Required sites covered | Passed checks | Unsupported checks skipped | Infrastructure failures |
|---|---|---|---|---:|---:|---:|
| TokenParallel | Compiled | Passed | yes, sites 2-3 | 35 | 7 | 0 |
| BallotParallel | Compiled | Passed | yes, sites 0-3 | 97 | 20 | 0 |
| MillionPixelParallel | Compiled | Passed | yes, sites 1-2 | 35 | 7 | 0 |
| KittyParallel | Compiled | Passed | yes, sites 1-3 | 52 | 10 | 0 |
| AirDropParallel | Compiled | Passed | yes, sites 4-7 | 65 | 13 | 0 |

An `unsupported check skipped` entry is an explicit validator classification for a property that cannot be checked at runtime; it is not a failed check. All required function IDs and relay sites were observed, and every workload had an empty missing-site list.

### 7.2 Directed mutants

| Workload | Directed mutants | Compiled | Strict runtime passed | Complete mutated-site coverage | Expected regression matched |
|---|---:|---:|---:|---:|---:|
| TokenParallel | 2 | 2/2 | 2/2 | 2/2 | 2/2 |
| BallotParallel | 2 | 2/2 | 2/2 | 2/2 | 2/2 |
| MillionPixelParallel | 1 | 1/1 | 1/1 | 1/1 | 1/1 |
| KittyParallel | 4 | 4/4 | 4/4 | 4/4 | 4/4 |
| AirDropParallel | 1 | 1/1 | 1/1 | 1/1 | 1/1 |
| **Total** | **10** | **10/10** | **10/10** | **10/10** | **10/10** |

## 8. Independent coverage handling and compatibility

### 8.1 Extension study isolation

The extension configuration contains:

```json
{
  "study": "parallel_certificate_extension",
  "aggregate_with_original_coverage": false,
  "output_subdirectory": "certificate_extension",
  "site_resolution": "semantic_locator_only"
}
```

Consequently:

- extension certificates are not added to the original coverage denominator;
- extension JSON/CSV are emitted only below `results/certificate_extension/`;
- site resolution remains stable across listener-ordinal shifts;
- TokenParallel and AirDropParallel remain separate logical rows despite sharing one compiled Token module.

### 8.2 Original coverage slices

Because the direct-source implementation adds functions to the original modules, the original evaluation configuration now applies explicit `static_function_filters`:

| Original workload | Static roots | Automatically included closure |
|---|---|---|
| Token | `transfer(address,bigint)` | original transfer relay handler |
| Ballot | `init(array string)`, `vote(uint32,uint32)`, `finalize()` | `is_voting`, shard-gather helpers, original relay lambdas |
| MillionPixel | `occupy(uint16,uint16)` | original occupy relay handler |
| Kitty | `mint(bigint,bool,address)`, `breed(uint32,uint32,bool)`, `registerNewBorns()` | `create`, `sqrt`, and original relay lambdas |
| AirDrop | `transfer_n(array chsimu.Token.payment)` | original transfer-n relay handler |

The analyzer expands each selected root through resolved synchronous calls and asynchronous relay-handler edges. Validation against the current manifests confirmed that none of the five slices contains `transfer_parallel`, `transfer_n_parallel`, `vote_parallel`, `occupy_parallel`, or `breed_parallel`.

### 8.3 Relay-ID and lambda compatibility

Direct-source additions shift discovery-order relay IDs and exported relay-lambda slots in Ballot and Kitty. Compatibility configuration updates are:

- Ballot original sites: `relay_site_4` through `relay_site_7`;
- Ballot original emitting handlers: `__relaylambda_9_finalize()` and `__relaylambda_10_finalize()`;
- Kitty original sites: `relay_site_0` and `relay_site_4` through `relay_site_8`;
- Kitty original emitting handlers: lambda slots `7`, `8`, and `9`.

Token's original sites remain `relay_site_0` and `relay_site_1`; MillionPixel's original site remains `relay_site_0`. The AirDrop runtime-fault selector remains `Token.transfer_n` at `relay_site_1`.

The semantic mutation configuration's portable signatures and Ballot `BroadcastToSingle` filter are updated consistently.

### 8.4 State fixture compatibility

Adding address-scoped fields changes whole-state JSON parsing for Token and Ballot. Existing Token/AirDrop fixtures now initialize:

```json
{"balance":"...", "auditTransfers":0, "auditVolume":"0"}
```

Existing Ballot fixtures now initialize the original `weight` and `voted_case` fields together with the four extension counters. These changes keep the old simulator entry points runnable under the expanded state schema.

### 8.5 Fresh original-workload regression

After applying the compatibility selectors and state fixtures, the existing
`CoverageScalabilityRunner.py real` pipeline was rerun from scratch. Token,
Ballot, MillionPixel, Kitty, and AirDrop all compiled and passed strict runtime
validation; the coverage report contained no diagnostics. The selected static
function closures contained only the original workload roots and their
reachable helpers/relay handlers, with no `*_parallel` entry point present.
FCA also compiled successfully and remained runtime-disabled as configured.

## 9. Reproduction

Run all commands from the repository root. The Native engine, `chsimu`, mutation engine, and optional Z3 backend must already be built. If shared libraries are installed in an activated environment, include its library directory in `LD_LIBRARY_PATH` before running the integration study.

### 9.1 Build the existing targets

```bash
cmake --build build-gcc12 --target preda_engine chsimu rpreda_mutation -j2
```

If the local build directory has another name, replace `build-gcc12` without changing the benchmark command.

### 9.2 Validate the benchmark configuration

```bash
python3 oxd_preda/tools/rpreda_evaluation/certificate_extension/ParallelCertificateBenchmarkRunner.py \
  --repo-root . \
  --config oxd_preda/tools/rpreda_evaluation/certificate_extension/parallel_certificate_benchmarks.json \
  --validate-config-only
```

Expected output includes `Configuration valid: 5 workloads`.

### 9.3 Run focused unit tests

```bash
python3 oxd_preda/tools/rpreda_evaluation/certificate_extension/test_parallel_certificate_benchmarks.py
```

The validated implementation runs 10 focused tests.

### 9.4 Run compile, certificate, mutation, and strict-runtime validation

```bash
python3 oxd_preda/tools/rpreda_evaluation/certificate_extension/ParallelCertificateBenchmarkRunner.py \
  --repo-root . \
  --config oxd_preda/tools/rpreda_evaluation/certificate_extension/parallel_certificate_benchmarks.json \
  --output results/certificate_extension \
  --engine bin/bin_release/rpreda_mutation \
  --chsimu bin/bin_release/chsimu \
  --seed 88 \
  --timeout 60
```

The command returns zero only if every workload, expected certificate, required directed mutation, and strict runtime coverage check passes.

### 9.5 Validate the products

```bash
python3 -m json.tool \
  results/certificate_extension/certificate_extension.json >/dev/null

test -s results/certificate_extension/certificate_extension.csv
```

The expected top-level JSON summary is:

```json
{
  "status": "Passed",
  "configured_workloads": 5,
  "completed_workloads": 5,
  "certificate_records": 14,
  "passed_certificate_records": 14,
  "failures": []
}
```

## 10. Changed files

### 10.1 Direct-source workload extensions

- `oxd_preda/simulator/contracts/Token.prd`
  - TokenParallel handlers and `transfer_parallel`;
  - fixed-three AirDropParallel `transfer_n_parallel`;
  - address-scoped audit state.
- `oxd_preda/simulator/contracts/Ballot.prd`
  - reputation/statistics state and handlers;
  - `vote_parallel`.
- `oxd_preda/simulator/contracts/MillionPixel.prd`
  - keyed statistics and parallel pixel state;
  - `occupy_parallel`.
- `oxd_preda/simulator/contracts/Kitty.prd`
  - bounded sibling-update state and handlers;
  - `breed_parallel`.

### 10.2 Extension benchmark harness

- `oxd_preda/tools/rpreda_evaluation/certificate_extension/ParallelCertificateBenchmarkRunner.py`
- `oxd_preda/tools/rpreda_evaluation/certificate_extension/parallel_certificate_benchmarks.json`
- `oxd_preda/tools/rpreda_evaluation/certificate_extension/test_parallel_certificate_benchmarks.py`
- `oxd_preda/tools/rpreda_evaluation/certificate_extension/fixtures/TokenParallel.prdts.in`
- `oxd_preda/tools/rpreda_evaluation/certificate_extension/fixtures/BallotParallel.prdts.in`
- `oxd_preda/tools/rpreda_evaluation/certificate_extension/fixtures/MillionPixelParallel.prdts.in`
- `oxd_preda/tools/rpreda_evaluation/certificate_extension/fixtures/KittyParallel.prdts.in`
- `oxd_preda/tools/rpreda_evaluation/certificate_extension/fixtures/AirDropParallel.prdts.in`

### 10.3 Original-study compatibility

- `oxd_preda/tools/rpreda_evaluation/benchmarks.json`
  - original-workload static function slices;
  - shifted Ballot and Kitty runtime IDs.
- `oxd_preda/tools/rpreda_mutation/semantic_benchmarks/benchmarks.json`
  - shifted Ballot/Kitty IDs and portable signatures;
  - shifted Ballot broadcast mutation filter.
- `oxd_preda/simulator/contracts/Token.prdts`
- `oxd_preda/simulator/contracts/AirDrop.prdts`
- `oxd_preda/simulator/contracts/Ballot.prdts`
- `oxd_preda/simulator/relay_optimization/benchmarks/fixtures/TokenDeterministic.prdts`
- `oxd_preda/simulator/relay_optimization/benchmarks/fixtures/TokenPerformance.prdts`
- `oxd_preda/simulator/relay_optimization/benchmarks/fixtures/AirDropDeterministic.prdts`
- `oxd_preda/simulator/relay_optimization/benchmarks/fixtures/AirDropPerformance.prdts`
- `oxd_preda/simulator/relay_optimization/benchmarks/fixtures/BallotDeterministic.prdts`
- `oxd_preda/simulator/relay_optimization/benchmarks/fixtures/BallotPerformance.prdts`
- `oxd_preda/tools/rpreda_mutation/semantic_benchmarks/fixtures/Token.prdts.in`
- `oxd_preda/tools/rpreda_mutation/semantic_benchmarks/fixtures/AirDrop.prdts.in`
- `oxd_preda/tools/rpreda_mutation/semantic_benchmarks/fixtures/Ballot.prdts.in`

### 10.4 Generated evidence and report

- `results/certificate_extension/certificate_extension.json`
- `results/certificate_extension/certificate_extension.csv`
- `R_PREDA_PARALLEL_CERTIFICATE_BENCHMARK_EXTENSION_IMPLEMENTATION.md`

## 11. Limitations

1. **Direct-source rather than separate-source extensions.** This is an explicit user-approved deviation from the design document. Original and extension entry points coexist in the same four physical contract sources.

2. **Original module identity changes.** Existing function bodies remain available, but additional state and exported functions change contract hashes, state schemas, exported opcodes, relay ordinals, and generated handler names.

3. **Token precedence is relay-to-relay.** The certificate proves receiver relay emission before audit relay emission. It is not a certificate over the preceding local balance assignment and does not prove handler completion order.

4. **AirDrop is fixed-three.** The current occurrence-insensitive loop analysis cannot prove the requested dynamic `recipient[i]` precedence. The benchmark uses three explicit recipient relays and one summary relay, yielding a finite upper bound of four.

5. **AirDrop summary is not sender-authenticated.** `summary` is a caller-provided address and is not constrained to the sender/current scope key.

6. **MillionPixel is a certificate slice, not full occupation semantics.** The statistics target is a `uint32` application key, not an owner address scope; the extension pixel handler does not reproduce the complete original `Land` update; and `sender` is supplied as an argument.

7. **Kitty is a bounded sibling model, not full breeding semantics.** The extension records counters and genes but does not reproduce object creation, gene mixing, nested owner relays, or newborn registration.

8. **Ballot is a focused counter extension.** It demonstrates reputation/statistics independence and YES/NO exclusion without replacing the original proposal aggregation and finalization workflow.

9. **Finite bounds are upper bounds.** Kitty and AirDrop guard failures can emit zero relays. Their raw certificate status is therefore `Conservative`, even though the finite upper bounds are sound and reported as proved bound properties.

10. **Study-level isolation is logical.** `aggregate_with_original_coverage=false` and explicit function slices keep extension rows out of the original coverage aggregate, but both studies still compile the same direct-source modules.

11. **Semantic mutation baseline health remains module-wide.** The existing semantic mutation runner's baseline Z3 health inspection reads the whole manifest. JSON filters restrict mutation selection and runtime coverage, but they cannot fully isolate module-wide solver diagnostics without a runner change.

12. **Site ordinals are not stable API.** The evidence records the ordinals from one bound artifact. Reproduction and configuration rely on semantic locators, source hashes, manifest hashes, and function signatures rather than assuming those numbers remain unchanged.

13. **The validated run was produced from a tracked-dirty implementation worktree.** The evidence contains source SHA-256 and manifest digests for binding. Committing the implementation changes repository metadata but should not change the semantic results if the source bytes remain identical.

## 12. Conclusion

The extension closes the pair-pattern gap in the existing real-workload suite without changing PREDA runtime routing, queues, scheduling, or engine semantics. It supplies proved examples of independence, mutual exclusion, relay-emission precedence, finite work, and finite depth; every certificate has owning evidence; all five workloads pass strict runtime validation; and all ten directed negative mutations produce the expected certificate regression.

The result should be presented as a protocol-certificate benchmark extension over real contract sources. MillionPixelParallel and KittyParallel are intentionally bounded, business-derived certificate slices and must not be described as complete reimplementations of their original business operations.
