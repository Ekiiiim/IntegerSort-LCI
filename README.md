# lci-irregular

## Overview

`lci-irregular` is a C++17 companion library for typed, irregular active-message communication over LCI. Applications provide fixed-size records, a destination function, a receive callback, and a completion condition. The library owns message packing, aggregation buffers, loopback delivery, LCI progress, and optional profiling.

This repository also contains the NAS Parallel Benchmarks (NPB) Integer Sort MPI baseline and an LCI+OpenMP implementation as an optional research benchmark. The reusable library has no MPI, OpenMP, NPB, or Integer Sort dependency.

This is an independent companion library, not an official component of LCI. The name is provisional.

## Requirements

- CMake 3.15 or newer
- A C++17 compiler with exceptions enabled
- An installed LCI package that provides the `LCI::lci` CMake target
- MPI C and OpenMP C++ only when their corresponding NPB benchmark implementations are enabled

## Build and Install

The default build compiles only the library:

```bash
cmake -S . -B build -DLCI_ROOT=/path/to/lci/install
cmake --build build -j
cmake --install build --prefix /path/to/lci-irregular/install
```

Examples and benchmarks are disabled by default:

```bash
cmake -S . -B build-all \
  -DLCI_ROOT=/path/to/lci/install \
  -DLCI_IRREGULAR_BUILD_EXAMPLES=ON \
  -DLCI_IRREGULAR_BUILD_BENCHMARKS=ON
cmake --build build-all -j
```

## Use From CMake

```cmake
find_package(LCIIrregular CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE LCIIrregular::lci_irregular)
```

Application code needs one include:

```cpp
#include <lci-irregular/irregular_runtime.hpp>
```

## Typed Active-Message Exchange

Records must be trivially copyable and have alignment no greater than `std::max_align_t`. The receive and completion callbacks must not throw.

```cpp
struct Record {
  std::uint64_t key;
  double value;
};

lci_irregular::IrregularRuntime runtime;
std::atomic<std::size_t> received{0};

auto receive_batch = [&](const Record* records, std::size_t count,
                         int source_rank) noexcept {
  process_records(records, count, source_rank); // Must be thread-safe.
  received.fetch_add(count, std::memory_order_relaxed);
};
auto is_done = [&]() noexcept {
  return received.load(std::memory_order_relaxed) == expected_records;
};

auto exchange = runtime.am_exchange_start<Record>(receive_batch, is_done);
runtime.barrier(); // All participating ranks have registered this exchange.
auto sender = exchange.make_sender(worker_index);
for (const Record& record : local_records) {
  sender.am_send(destination_for(record), record);
}
sender.flush();

while (!exchange.is_done()) {
  exchange.progress(worker_index);
}
exchange.wait();
```

`am_exchange_counted` is the blocking convenience API when completion is an expected receive-record count. `am_exchange_until` accepts a general completion callback. Both return an `AmExchangeProfile` value and include a registration barrier, so every runtime rank must call them in the same order.

## Threading and Progress

Only one `IrregularRuntime` may be live in a process. The application chooses its worker model and passes `device_count`; the library does not create threads or call OpenMP.

Each sender belongs to one worker and is not shared concurrently. Different workers may use different senders for the same exchange. Indexed `progress(worker_index)` maps workers to LCI devices and enables per-worker profiling. Receive callbacks can run concurrently on threads that call progress, so application state updated by callbacks must be thread-safe.

Participating ranks must create exchanges in the same order and complete application phase synchronization before the first send. This keeps the nonblocking start operation free of an implicit global barrier while ensuring that a remote active message cannot arrive before its exchange is registered.

All senders must be flushed and no longer used before `wait()` or `profile()`. An exchange must complete before destruction. `wait()` is the blocking completion operation; `is_done()` and `progress()` support integration with an application's own scheduling loop.

## Profiling

Profiling is disabled by default. It records aggregate and per-worker statistics for nonempty flushes, user-visible progress calls, remote receives, and loopback receives.

```cpp
lci_irregular::IrregularRuntimeOptions options;
options.profiling.enabled = true;
options.profiling.worker_count = application_worker_count;
options.profiling.output_directory = "profiles";
options.profiling.file_prefix = "my-run";

lci_irregular::IrregularRuntime runtime(options);
```

Each operation reports calls, records, typed payload bytes, and elapsed nanoseconds. `exchange.profile()` returns a snapshot after completion. Completed profiles are also queued by the runtime. `runtime.write_profiles()` writes them immediately; otherwise the runtime performs best-effort automatic output during destruction.

Output is one JSONL file per rank, named `<prefix>.rank-<rank>.jsonl`. No file or directory is created when profiling is disabled or no output directory is configured.

## NPB Integer Sort Benchmarks

The optional benchmark compares the NPB 3.4.3 MPI IS implementation with this project's LCI+OpenMP implementation:

```bash
cmake -S . -B build-npb \
  -DLCI_ROOT=/path/to/lci/install \
  -DLCI_IRREGULAR_BUILD_BENCHMARKS=ON \
  -DNPB_IS_BUILD_MPI_BASELINE=ON \
  -DNPB_IS_BUILD_LCI_OPENMP=ON \
  -DNPB_CLASSES=A
cmake --build build-npb -j
```

Enable the LCI benchmark's detailed reporter and automatic raw profiles with `-DIS_LCI_ENABLE_TIMERS=ON`. Set `LCI_IRREGULAR_PROFILE_DIR` to choose the output directory. See `benchmarks/npb-is/README.md` for source ownership and benchmark details.

## Repository Layout

```text
include/lci-irregular/   installed interface and required template details
src/                     compiled runtime, handler dispatch, and profile writer
examples/                optional typed active-message example
benchmarks/npb-is/
  mpi-baseline/          NPB-provided MPI IS implementation
  common/                NPB utility sources used by both implementations
  lci-openmp/            LCI+OpenMP IS implementation and profile reporter
cmake/                   package config and benchmark build helpers
```

Start with `examples/typed_am_exchange.cpp` for the generic API. For the complete Integer Sort algorithm sequence, read `benchmarks/npb-is/lci-openmp/main.cpp`.

## Provenance

The MPI baseline and common utility code originate from NPB 3.4.3. The LCI+OpenMP benchmark preserves the NPB IS algorithm and verification contract while replacing its communication mechanism. Source-level notes in `benchmarks/npb-is/lci-openmp/nas/` identify the mechanism-level adaptations.
