# IntegerSort-LCI
## Installation
```
mkdir build && cd build
cmake ..
make
```

## Algorithm Interface Guide

The LCI implementation is organized so readers can distinguish the executable
benchmark driver, reusable integer-sort stages, reusable LCI communication, and
benchmark-only support code:

- `LCI/is.cpp`: thin executable driver for initialization, warmup, timed
  iterations, final verification, and teardown. Start here to see how the
  pieces are composed.
- `LCI/algorithm/`: integer-sort algorithm stages and the per-iteration
  skeleton.
- `LCI/communication/`: LCI active-message redistribution code. This is the
  main communication pattern to study when adapting the approach to other
  microbenchmarks.
- `LCI/benchmark/nas/`: only code/data split out from the NAS/NPB MPI IS
  implementation.
  - `problem_config.*`: NAS class constants and benchmark sizes.
  - `key_generation.*`: NAS `randlc`, `find_my_seed`, and `create_seq`
    behavior, parameterized for this driver.
  - `verification_cases.*`: NAS partial-verification test tables and
    class-specific rank adjustment rules.
- `LCI/benchmark/driver/`: driver-only benchmark support, including
  environment options and NPB result printing.
- `LCI/benchmark/timing/`: NAS timer wrappers and LCI timer summary printing.
- `LCI/benchmark/verification/`: LCI partial/full verification flow using NAS
  verification cases.
- `LCI/benchmark/profiling/`: optional benchmark/profiling instrumentation
  written for this implementation.
- `LCI/types.hpp`: shared C++ type aliases used by the algorithm,
  communication, and benchmark code.

| Paper stage | Code interface | Responsibility |
| --- | --- | --- |
| NAS problem config | `LCI/benchmark/nas/problem_config.hpp` | Define NAS class constants and derived benchmark sizes from the MPI IS baseline. |
| Shared types | `LCI/types.hpp` | Define `KeyValue` for key values, `KeyCount` for local/bucket/frequency counts, and `KeyRank` for cumulative/global key ranks. |
| Benchmark driver | `LCI/benchmark/driver/` | Keep environment options and result printing separate from reusable implementation code. |
| Benchmark timing | `LCI/benchmark/timing/` | Wrap NAS timers and print LCI timing summaries. |
| Benchmark verification | `LCI/benchmark/verification/` | Verify LCI sort results using NAS verification cases. |
| CountBuckets | `LCI/algorithm/bucket_plan.hpp::count_local_buckets` | Count local keys into bucket intervals. |
| Global bucket totals | `LCI/algorithm/sort_iteration.hpp::run_sort_iteration` | Use LCI reduce plus broadcast to compute global bucket sizes. |
| MapBucketsToProcesses | `LCI/algorithm/bucket_plan.hpp::build_bucket_plan` | Greedily assign bucket intervals to ranks and compute this rank's owned key range. |
| Redistribute keys | `LCI/communication/lci_redistributor.hpp::redistribute_keys` | Stream keys to owner ranks with LCI active messages and update the local frequency histogram. |
| Final ranking | `LCI/algorithm/ranking.hpp::compute_local_ranks` | Prefix-sum the local frequency histogram into cumulative key ranks. |

To implement a similar integer sort from scratch, keep the same algorithm data
contract after input generation: count local buckets, compute global bucket
totals, map each bucket to an owner rank, redistribute keys into the owner
rank's frequency histogram, and prefix-sum that histogram to produce ranks.
For other LCI-based microbenchmarks, `LCI/communication/` is the most reusable
piece: it shows how this code batches keys, sends active messages, handles
loopback, and progresses receives while worker threads are active. A generic
MPI/LCI backend interface is left for a later refactor.
