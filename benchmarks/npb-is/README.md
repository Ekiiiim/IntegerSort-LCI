# NPB Integer Sort Benchmark

This directory contains two implementations of the NAS Parallel Benchmarks 3.4.3 Integer Sort benchmark. They are retained as a research comparison and as a case study for `lci-irregular`; they are not part of the installed library.

## Source Ownership

- `mpi-baseline/` contains the benchmark-provided MPI implementation. Its `is.c` source is retained unchanged for comparison.
- `common/` contains NPB timer and result-printing utilities used by both implementations.
- `lci-openmp/` contains this project's multithreaded FA-BSP implementation using LCI and the companion library.
- `lci-openmp/nas/` contains NPB-derived benchmark rules: problem sizes, key generation, timing, verification, and result reporting. Each `.cpp` file begins with a short note describing the mechanism-level adaptations that preserve the benchmark logic.
- `lci-openmp/integer_sort/` contains the local bucket plan, redistribution wrapper, ranking stages, types, and workspace.
- `lci-openmp/benchmark_support/` coordinates one benchmark run and contains benchmark-specific options and reductions.
- `lci-openmp/profiling/` converts generic library profiles into the benchmark's detailed per-worker, min/max, and load-balance output.

The readable algorithm entry point is `lci-openmp/main.cpp`:

```text
generate keys
organize keys into buckets
compute global bucket sizes
assign buckets to ranks
redistribute typed keys with LCI active messages
compute final ranking
run partial and full verification
```

## Configure and Build

Enable all benchmark targets:

```bash
cmake -S . -B build-npb \
  -DLCI_ROOT=/path/to/lci/install \
  -DLCI_IRREGULAR_BUILD_BENCHMARKS=ON \
  -DNPB_IS_BUILD_MPI_BASELINE=ON \
  -DNPB_IS_BUILD_LCI_OPENMP=ON \
  -DNPB_CLASSES=A
cmake --build build-npb -j
```

`NPB_IS_BUILD_MPI_BASELINE` and `NPB_IS_BUILD_LCI_OPENMP` independently select the implementations. The MPI baseline requires MPI C only. The LCI implementation requires OpenMP C++ and links `LCIIrregular::lci_irregular`.

`NPB_CLASSES` is a CMake list containing any of `A;B;C;D;E`. Executables are written to the build tree's `bin/` directory.

## Profiling

Configure with `-DIS_LCI_ENABLE_TIMERS=ON` to enable generic AM profiling for the LCI implementation. The benchmark prints detailed per-worker and min/max rows on rank 0 and one `LB` line on every rank.

Raw per-rank JSONL is written automatically. The default output directory is `profiles`; override it at runtime:

```bash
LCI_IRREGULAR_PROFILE_DIR=/path/to/profiles <lci-launcher> build-npb/bin/is_lci_A
```

The legacy report label `progress_wait` represents time in user-visible generic progress calls and wait iterations. Internal send-retry progress is excluded.
