# lci-irregular

`lci-irregular` is a C++17 companion library for typed irregular active-message communication over LCI. It is intended for applications that need many fine-grained, destination-dependent sends, such as integer sorting and other irregular microbenchmarks.

The library provides one reusable interface over LCI active messages:

- users define a fixed-size trivially copyable data type
- users register a receive handler for that type
- users call `post_am()` to send typed values to destination ranks
- the library owns packing, aggregation buffers, loopback delivery, progress integration, and optional profiling
- applications still define their own worker model and completion condition

This repository also contains an optional NAS Parallel Benchmarks Integer Sort comparison: the NPB MPI baseline and this project's LCI+OpenMP implementation. The installed library itself has no MPI, OpenMP, NPB, or Integer Sort dependency.

This is an independent companion library, not an official component of LCI. The name is provisional.

## Requirements

- CMake 3.15 or newer
- A C++17 compiler with exceptions enabled
- An installed LCI package that provides the `LCI::lci` CMake target
- MPI C only when building the NPB MPI baseline
- OpenMP C++ only when building the LCI+OpenMP Integer Sort benchmark

## Build

The default build compiles only the reusable library:

```bash
cmake -S . -B build -DLCI_ROOT=/path/to/lci/install
cmake --build build -j
```

Install it with:

```bash
cmake --install build --prefix /path/to/lci-irregular/install
```

Examples and benchmarks are opt-in:

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

Application code includes one public header:

```cpp
#include <lci-irregular/irregular_runtime.hpp>
```

## Core Interface

The common use pattern is:

1. Construct `Runtime`.
2. Register one typed active-message handler with `set_am_handler<T>()`.
3. Obtain one `WorkerHandler` for each logical worker.
4. Send typed values with `worker.post_am(destination_rank, value)`.
5. Flush each worker's remaining aggregation buffers.
6. Progress until the application-specific receive condition is satisfied.
7. Clear the active-message handler.

```cpp
struct KeyValue {
  std::uint64_t key;
  std::uint64_t value;
};

lci_irregular::Runtime runtime;

auto handler = [&](const KeyValue& value) { process(value); };

lci_irregular::AmOptions am_options;
am_options.profile_name = "redistribute-keys";
runtime.set_am_handler<KeyValue>(handler, am_options);

// The application owns threading. OpenMP is only one possible worker model.
#pragma omp parallel
{
  auto worker =
      runtime.get_worker_handler(current_worker_index());

  #pragma omp for nowait
  for (std::size_t i = 0; i < local_keys.size(); ++i) {
    const KeyValue value = local_keys[i];
    worker.post_am(destination_for(value), value);
  }

  worker.flush();

  while (runtime.recv_count() < expected_receive_count) {
    worker.progress();
  }
}

const lci_irregular::AmProfile profile = runtime.am_profile();
runtime.clear_am_handler();
```

`set_am_handler<T>()` collectively registers the receive logic for one active typed AM phase. It includes the synchronization needed before any rank sends data. Calling it while another phase is active is an error.

`get_worker_handler(worker_index)` returns a lightweight, copyable, non-owning proxy for one stable logical worker. Concurrent workers must use distinct dense indices beginning at zero. A worker handler can be reused across AM phases but must not outlive its runtime.

`worker.post_am(destination_rank, value)` appends one typed value to a runtime-owned aggregation buffer for that worker and destination.

`worker.flush()` posts that worker's nonempty aggregation buffers. It does not wait for network completion.

`recv_count()` returns how many typed values have been processed by the registered handler. Applications use this to implement their own completion condition while calling `worker.progress()`.

`clear_am_handler()` locally ends the current phase. Call it only after every worker has flushed and the application-specific receive-completion condition has been met. It reports unflushed records as an error.

## AM Handlers

The runtime invokes the handler once for each received value:

```cpp
auto handler = [&](const MyType& value) { process(value); };
```

Handlers may accept only the value, or additionally accept the source rank as a second `int` argument:

```cpp
auto handler = [&](const MyType& value, int source_rank) {
  process(value, source_rank);
};
```

Use the single-argument form when receive processing does not depend on the sender. The underlying payload is byte storage owned by LCI, so the runtime loads each value with `memcpy` before invoking the handler instead of exposing a `T*`. This avoids requiring the LCI packet address to satisfy `T` pointer alignment.

When a handler accepts `const T&`, the reference is valid only for that invocation and must not be retained.

Typed AM values must be:

- trivially copyable
- trivially default constructible

The receive handler must not throw. Different workers may invoke it concurrently while progressing their devices, so the handler and any state it accesses must support concurrent invocation.

## Runtime and Workers

Only one `Runtime` may be live in a process. The runtime initializes and finalizes LCI, allocates LCI devices, registers the AM callback, and owns the current AM state. Construction and destruction are collective lifecycle operations: participating ranks must enter them in the same order.

The library does not create threads. The application chooses a worker model and obtains one `WorkerHandler` for each stable worker index. Each handler provides:

- `post_am(destination_rank, value)`
- `flush()`
- `progress()`

Each worker index selects a per-worker set of aggregation buffers and maps to an LCI device modulo `device_count`.

Receive handlers may run on threads that call `WorkerHandler::progress()`, so application data touched by handlers must be thread-safe. `set_am_handler()` and `clear_am_handler()` must not run concurrently with worker operations.

## Profiling

Profiling is disabled by default.

```cpp
lci_irregular::RuntimeOptions options;
options.profiling.enabled = true;
options.profiling.worker_count = application_worker_count;
options.profiling.output_directory = "profiles";
options.profiling.file_prefix = "my-run";

lci_irregular::Runtime runtime(options);
```

Profiling records aggregate and per-worker statistics for:

- nonempty flushes
- user-visible progress calls
- remote receives
- loopback receives

Each operation records calls, typed values, payload bytes, and elapsed nanoseconds.

Per-worker receive profiling uses a lightweight thread-local scope. When a worker calls `progress()`, `post_am()`, or `flush()`, the library temporarily records that worker index for the current thread. If an AM receive is handled inside that scope, the receive is attributed to that worker.

`runtime.am_profile()` returns a snapshot for the active AM state. `clear_am_handler()` queues the completed profile. `runtime.write_profiles()` writes queued profiles immediately; otherwise the runtime performs best-effort automatic output during destruction.

Output is one JSONL file per rank:

```text
<file_prefix>.rank-<rank>.jsonl
```

No file is written when profiling is disabled or no output directory is configured.

## Repository Layout

```text
include/lci-irregular/
  irregular_runtime.hpp   public runtime API and required template definitions
  am_profile.hpp          public profiling option/result types
  detail/                 internal packing, send-buffer, AM-state, and profiling helpers

src/
  irregular_runtime.cpp   LCI lifecycle, runtime methods, incoming AM callback
  profile_writer.cpp      JSONL profile output

examples/
  typed_am_exchange.cpp   small generic typed-AM example

benchmarks/npb-is/
  mpi-baseline/           NPB-provided MPI Integer Sort implementation
  common/                 NPB utility sources shared by benchmark targets
  lci-openmp/             LCI+OpenMP Integer Sort implementation using lci-irregular

cmake/                    package config and benchmark build helpers
```

Start with `examples/typed_am_exchange.cpp` for the generic API. For the complete Integer Sort algorithm sequence, read `benchmarks/npb-is/lci-openmp/main.cpp` and `benchmarks/npb-is/lci-openmp/integer_sort/`.

## Integer Sort Benchmarks

The optional benchmark compares the NPB 3.4.3 MPI Integer Sort implementation with the LCI+OpenMP implementation:

```bash
cmake -S . -B build-npb \
  -DLCI_ROOT=/path/to/lci/install \
  -DLCI_IRREGULAR_BUILD_BENCHMARKS=ON \
  -DNPB_IS_BUILD_MPI_BASELINE=ON \
  -DNPB_IS_BUILD_LCI_OPENMP=ON \
  -DNPB_CLASSES=A
cmake --build build-npb -j
```

Enable the LCI benchmark's detailed timer/profile reporter with:

```bash
-DIS_LCI_ENABLE_TIMERS=ON
```

Set `LCI_IRREGULAR_PROFILE_DIR` to choose the raw profile output directory. See `benchmarks/npb-is/README.md` for benchmark source ownership and build details.

## Source Notes

The MPI baseline and common utility code originate from NPB 3.4.3. The LCI+OpenMP benchmark preserves the NPB Integer Sort algorithm and verification contract while replacing the communication mechanism with `lci-irregular`. Source-level notes in `benchmarks/npb-is/lci-openmp/nas/` identify mechanism-level adaptations to NAS-derived logic.
