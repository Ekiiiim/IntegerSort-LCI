# IntegerSort-LCI
## Installation
```
mkdir build && cd build
cmake ..
make
```

## Algorithm Interface Guide

The LCI implementation is organized around the same stages described in the
paper's LCI pseudocode. `LCI/is.cpp` is the benchmark driver: it owns LCI
runtime setup, NAS verification, timers, and output. The reusable algorithm
stages live in focused files under `LCI/`.

| Paper stage | Code interface | Responsibility |
| --- | --- | --- |
| Problem config/types | `LCI/is_config.hpp` | Define NAS class constants, derived problem sizes, and shared `Count`/`Rank` types. |
| GenerateGaussianKeys | `LCI/key_generation.hpp` | Generate the local NPB Gaussian-like key sequence for one rank. |
| CountBuckets | `LCI/bucket_plan.hpp::count_local_buckets` | Count local keys into bucket intervals. |
| Global bucket totals | `LCI/is.cpp` driver | Use LCI reduce plus broadcast to compute global bucket sizes. |
| MapBucketsToProcesses | `LCI/bucket_plan.hpp::build_bucket_plan` | Greedily assign bucket intervals to ranks and compute this rank's owned key range. |
| Redistribute keys | `LCI/lci_redistributor.hpp::redistribute_keys` | Stream keys to owner ranks with LCI active messages and update the local frequency histogram. |
| Final ranking | `LCI/ranking.hpp::compute_local_ranks` | Prefix-sum the local frequency histogram into cumulative key ranks. |

To implement a similar integer sort from scratch, keep the same data contract:
generate local keys, count local buckets, compute global bucket totals, map each
bucket to an owner rank, redistribute keys into the owner rank's frequency
histogram, and prefix-sum that histogram to produce ranks. The current
redistribution interface is intentionally LCI-specific. A generic MPI/LCI
backend interface is left for a later refactor.
