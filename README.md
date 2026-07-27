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

1. Construct `IrregularRuntime`.
2. Register one typed active-message handler with `set_am_handler<T>()`.
3. Send typed values with `post_am(worker_index, destination_rank, value)`.
4. Flush each worker's remaining aggregation buffers.
5. Progress until the application-specific receive condition is satisfied.
6. Call `quiet()` before clearing the handler or destroying the runtime.

```cpp
struct KeyValue {
  std::uint64_t key;
  std::uint64_t value;
};

lci_irregular::IrregularRuntime runtime;

auto handler = [&](lci_irregular::AmRecords<KeyValue> records) {
  for (std::size_t i = 0; i < records.size(); ++i) {
    process(records[i]);
  }
};

lci_irregular::AmOptions am_options;
am_options.profile_name = "redistribute-keys";
runtime.set_am_handler<KeyValue>(handler, am_options);

// The application owns threading. OpenMP is only one possible worker model.
#pragma omp parallel
{
  const std::size_t worker_index = current_worker_index();

  #pragma omp for nowait
  for (std::size_t i = 0; i < local_keys.size(); ++i) {
    const KeyValue value = local_keys[i];
    runtime.post_am(worker_index, destination_for(value), value);
  }

  runtime.flush_remaining_buffers(worker_index);

  while (runtime.received_record_count() < expected_receive_count) {
    runtime.progress(worker_index);
  }

  runtime.quiet(worker_index);
}

const lci_irregular::AmProfile profile = runtime.am_profile();
runtime.clear_am_handler();
```

`set_am_handler<T>()` registers the receive logic for one active typed AM state. A runtime supports one AM state at a time. All participating ranks should register handlers in the same order before any rank sends data for that state.

`post_am(worker_index, destination_rank, value)` appends one typed value to a runtime-owned aggregation buffer for that worker and destination. Concurrent workers must use distinct stable worker indices.

`flush_remaining_buffers(worker_index)` posts that worker's nonempty aggregation buffers. It does not wait for network completion.

`received_record_count()` returns how many typed values have been delivered to the registered handler. Applications use this to implement their own completion condition.

`quiet(worker_index)` progresses until locally posted AM sends for the current AM state have reached LCI network completion. Call it before `clear_am_handler()` or runtime destruction.

## AM Records

Handlers receive `AmRecords<T>`:

```cpp
auto handler = [&](lci_irregular::AmRecords<MyType> records) {
  for (std::size_t i = 0; i < records.size(); ++i) {
    MyType value = records[i];
  }
};
```

`AmRecords<T>` is a callback-scoped typed view over the active-message payload. It does not own the memory. The view is valid only while the handler is running.

Handlers may accept only `AmRecords<T>`, or additionally accept the source rank as a second `int` argument. Use the single-argument form when receive processing does not depend on the sender.

The underlying payload is byte storage owned by LCI, so `operator[]` loads each value with `memcpy` instead of exposing a `T*`. This avoids requiring the LCI packet address to satisfy `T` pointer alignment.

Typed AM values must be:

- trivially copyable
- trivially default constructible
- trivially move constructible
- aligned no more strictly than `std::max_align_t`

The receive handler must not throw.

## Runtime and Workers

Only one `IrregularRuntime` may be live in a process. The runtime initializes and finalizes LCI, allocates LCI devices, registers the AM callback, and owns the current AM state.

The library does not create threads. The application chooses a worker model and passes stable worker indices to:

- `post_am(worker_index, ...)`
- `flush_remaining_buffers(worker_index)`
- `progress(worker_index)`
- `quiet(worker_index)`

Each worker index selects a per-worker set of aggregation buffers and maps to an LCI device modulo `device_count`.

Receive handlers may run on threads that call `progress()`, so application data touched by handlers must be thread-safe.

## Profiling

Profiling is disabled by default.

```cpp
lci_irregular::IrregularRuntimeOptions options;
options.profiling.enabled = true;
options.profiling.worker_count = application_worker_count;
options.profiling.output_directory = "profiles";
options.profiling.file_prefix = "my-run";

lci_irregular::IrregularRuntime runtime(options);
```

Profiling records aggregate and per-worker statistics for:

- nonempty flushes
- user-visible progress calls
- remote receives
- loopback receives

Each operation records calls, typed values, payload bytes, and elapsed nanoseconds.

Per-worker receive profiling uses a lightweight thread-local scope. When `progress(worker_index)`, `post_am(worker_index, ...)`, or a worker flush enters library code, the library temporarily records that worker index for the current thread. If an AM receive is handled inside that scope, the receive is attributed to that worker. Calls made through unindexed `progress()` are recorded in the aggregate profile only.

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
