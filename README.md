# IntegerSort-LCI
## Installation
```
mkdir build && cd build
cmake ..
make
```

## Algorithm Interface Guide

The LCI implementation is organized so readers can distinguish the algorithm
pipeline, NAS benchmark contract, reusable integer-sort stages, reusable LCI
communication, and the support code needed to run the NPB IS executable:

- `LCI/is.cpp`: public entry point written as an algorithm script. Start here
  to see the sequence: generate keys, organize buckets, compute global bucket
  sizes, assign buckets to ranks, redistribute with LCI, compute ranks, and
  verify.
- `LCI/nas/`: NAS/NPB IS benchmark contract extracted from the MPI baseline.
  It contains benchmark sizes, input generation, verification cases/rules,
  run-size rules, and result-reporting formulas, but no LCI communication code.
- `LCI/integer_sort/`: reusable integer-sort storage and stages: local bucket
  counting, bucket ownership planning, local rank prefix sums, and workspace
  ownership.
- `LCI/runtime/`: LCI runtime ownership, device allocation, and low-level
  collectives used by the benchmark pipeline.
- `LCI/communication/`: reusable LCI active-message redistribution and
  reductions. This is the main communication code to study when adapting the
  approach to other microbenchmarks.
- `LCI/communication/profiling/`: optional profiling for the LCI redistribution
  path.
- `LCI/benchmark_support/`: executable support for this NPB IS run. It wires
  NAS rules, sort stages, timers, verification, result printing, and LCI
  communication together behind the algorithm-step methods used by `is.cpp`.
- `LCI/types.hpp`: shared C++ type aliases used by the algorithm,
  communication, and benchmark code.

Key file map:

```text
LCI/nas/
  problem_config.hpp        NPB IS class sizes and constants
  key_generation.*          NAS randlc/find_my_seed/create_seq behavior
  run_rules.*               NAS process-count, buffer-size, and random-stream rules
  verification_cases.*      NAS test-index/test-rank tables and rank adjustments
  verification_rules.*      NAS partial-verification key capture and rank checks
  reporting_rules.*         NAS verification-count and MOPS formulas
LCI/runtime/
  lci_runtime.*             LCI init/finalize, device pool, and collective wrappers
LCI/integer_sort/
  workspace.*               storage for keys, buckets, histograms, and ranks
  bucket_plan.*             local bucket counting and bucket ownership planning
  ranking.*                 local frequency-histogram prefix sums
LCI/communication/
  lci_redistributor.*       LCI active-message key redistribution
  reductions.*              LCI reduction callbacks used by this executable
  profiling/                optional all-to-all redistribution profiling
LCI/benchmark_support/
  nas_integer_sort_run.*    high-level NAS IS run and per-step algorithm methods
  benchmark_timers.*        NAS-style timer labels and LCI timer summary reduction
  verification.*            LCI full verification and benchmark-side reporting
  results.*                 initial status and final NPB result printing
  run_options.*             LCI-specific environment flags
```

| Paper stage | Code interface | Responsibility |
| --- | --- | --- |
| NAS benchmark contract | `LCI/nas/` | Define NAS class constants, input generation, verification cases/rules, run-size rules, and reporting formulas from the MPI IS baseline. |
| Shared types | `LCI/types.hpp` | Define `KeyValue` for key values, `KeyCount` for local/bucket/frequency counts, and `KeyRank` for cumulative/global key ranks. |
| Executable support | `LCI/benchmark_support/nas_integer_sort_run.hpp` | Expose readable per-step methods used by `LCI/is.cpp` while hiding timers, verification counters, and LCI glue. |
| LCI runtime | `LCI/runtime/lci_runtime.hpp::LciRuntime` | Own LCI initialization, device allocation, barriers, broadcasts, and reductions. |
| Workspace | `LCI/integer_sort/workspace.hpp::IntegerSortWorkspace` | Own the raw key, bucket, histogram, and rank storage used by the pipeline. |
| CountBuckets | `LCI/integer_sort/bucket_plan.hpp::count_local_buckets` | Count local keys into bucket intervals. |
| Global bucket totals | `LCI/benchmark_support/nas_integer_sort_run.hpp::compute_global_bucket_sizes` | Use LCI reduce plus broadcast to compute global bucket sizes. |
| MapBucketsToProcesses | `LCI/integer_sort/bucket_plan.hpp::build_bucket_plan` | Greedily assign bucket intervals to ranks and compute this rank's owned key range. |
| Redistribute keys | `LCI/communication/lci_redistributor.hpp::redistribute_keys` | Stream keys to owner ranks with LCI active messages and update the local frequency histogram. |
| Final ranking | `LCI/integer_sort/ranking.hpp::compute_local_ranks` | Prefix-sum the local frequency histogram into cumulative key ranks. |

To implement a similar integer sort from scratch, keep the same algorithm data
contract after input generation: count local buckets, compute global bucket
totals, map each bucket to an owner rank, redistribute keys into the owner
rank's frequency histogram, and prefix-sum that histogram to produce ranks.
For other LCI-based microbenchmarks, `LCI/communication/` is the most reusable
piece: it shows how this code batches keys, sends active messages, handles
loopback, and progresses receives while worker threads are active. A generic
MPI/LCI backend interface is left for a later refactor.

Development check:

```bash
python3 tests/check_lci_interface_layout.py
```

This lightweight check guards the public `LCI/is.cpp` entry point so it keeps
showing the algorithm pipeline instead of drifting back into low-level runtime
or buffer-management details.
