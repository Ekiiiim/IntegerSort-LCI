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

Records must be trivially copyable, trivially default constructible, trivially move constructible, and have alignment no greater than `std::max_align_t`. The receive and completion callbacks must not throw.

```cpp
struct Record {
  std::uint64_t key;
  double value;
};

lci_irregular::IrregularRuntime runtime;
std::atomic<std::size_t> received{0};

auto am_handler = [&](lci_irregular::RecordBatchView<Record> records,
                      int source_rank) {
  for (std::size_t i = 0; i < records.size(); ++i) {
    process_record(records[i], source_rank); // Must be thread-safe.
  }
  received.fetch_add(records.size(), std::memory_order_relaxed);
};
auto is_done = [&]() {
  return received.load(std::memory_order_relaxed) == expected_records;
};

auto send_phase = [&](auto run_worker) {
  // OpenMP is owned and configured by the application; any thread model can
  // launch these workers as long as the calls execute concurrently.
  #pragma omp parallel
  {
    const std::size_t worker_index = current_worker_index();
    run_worker(worker_index, [&](auto am_send) {
      #pragma omp for nowait
      for (std::size_t i = 0; i < local_records.size(); ++i) {
        const Record& record = local_records[i];
        am_send(destination_for(record), record);
      }
    });
  }
};

const auto profile = runtime.am_exchange_until<Record>(am_handler, is_done, send_phase);
```

`RecordBatchView` reads typed record values directly from the received packet
without materializing a second array. The view is valid only during the receive
callback; copy any records that must be retained after the callback returns.

`am_exchange_start` is the primary asynchronous API. It returns an exchange handle so callers can create worker-local senders, drive progress, wait for completion, and collect profiling data explicitly. This form is intended for dynamic task graphs, work queues, and applications that need custom exchange lifecycles.

`am_exchange_until` is a convenience API for fork-join bulk phases. Its `send_phase` receives `run_worker`; each application worker calls `run_worker(worker_index, produce)`, and `produce(am_send)` submits typed records. The call creates the asynchronous exchange internally, includes registration, sender aggregation, progress, completion, and profiling, and returns an `AmExchangeProfile` after completion. Every runtime rank must call it in the same order.

Every rank must call `run_worker` at least once, including ranks with no outgoing records, so that each rank participates in progress. `send_phase` must wait for and join all `run_worker` calls before returning. `produce` and `am_send` must not escape their `run_worker` invocation. Concurrent calls must use distinct worker indices, and each index must remain stable for the full `run_worker` invocation. Each index maps modulo `device_count`; when profiling is enabled, it must be less than `profiling.worker_count`. A single worker is valid only when it produces all outgoing records for its rank.

All participating workers, including progress-only workers, must launch concurrently because `run_worker` blocks until global completion. The application owns the worker threads and may use OpenMP or any other thread model; `am_handler` and `is_done` can run concurrently with those workers, so all application state they share must be thread-safe. Callable construction must not throw. Errors after registration are fail-fast: the library terminates rather than attempting distributed cancellation.

Use `am_exchange_start` when the application needs direct control over sender creation, progress, completion, and finalization. Use `am_exchange_until` when the exchange is a single fork-join phase and the wrapper's worker contract fits the application.

## Threading and Progress

Only one `IrregularRuntime` may be live in a process. The application chooses its worker model and passes `device_count`; the library does not create threads and has no dependency on OpenMP.

Each sender belongs to one worker and is not shared concurrently. Different workers may use different senders for the same exchange. Indexed `progress(worker_index)` maps workers to LCI devices and enables per-worker profiling. Receive callbacks can run concurrently on threads that call progress, so application state updated by callbacks must be thread-safe.

For the asynchronous API, participating ranks must create exchanges in the same order and complete application phase synchronization before the first send. All senders must be closed or destroyed and all other progress workers must stop operating on the exchange before one thread calls `wait()` or `profile()`. An exchange must complete before destruction.

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
