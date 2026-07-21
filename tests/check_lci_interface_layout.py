#!/usr/bin/env python3
"""Check that the LCI benchmark entry point exposes the algorithm pipeline.

This intentionally stays lightweight: the local developer environment may not
have LCI/OpenMP installed, but we can still guard the public-facing source
layout that researchers read first.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
IS_CPP = ROOT / "LCI" / "is.cpp"

PIPELINE_STEPS = [
    "initialize",
    "generate_keys",
    "run_warmup_iteration",
    "start_timed_region",
    "begin_iteration",
    "apply_nas_key_changes",
    "organize_keys_into_buckets",
    "compute_global_bucket_sizes",
    "assign_buckets_to_processes",
    "redistribute_keys_with_lci",
    "compute_final_ranking",
    "verify_partial_ranking",
    "finish_iteration",
    "stop_timed_region",
    "verify_full_ranking",
    "print_results",
]

LOW_LEVEL_PATTERNS = [
    r"\bprocess_bucket_distrib_ptr",
    r"\bbucket_i_to_process_ranks\b",
    r"\bbucket_size(?:_totals)?\b",
    r"\bget_num_threads_per_device\b",
    r"\ballocate_devices\b",
    r"\bfree_devices\b",
    r"\ballocate_work_arrays\b",
    r"\bfree_work_arrays\b",
    r"\blci::(?:broadcast|reduce|barrier)_x\b",
    r"\bg_runtime_(?:init|fina)",
    r"\bsetvbuf\s*\(",
    r"\bmalloc\s*\(",
    r"\bfree\s*\(",
]


def main() -> int:
    source = IS_CPP.read_text()
    failures = []

    position = -1
    for step in PIPELINE_STEPS:
        match = re.search(rf"\.{step}\s*\(", source)
        if not match:
            failures.append(f"missing pipeline call: {step}()")
            continue
        if match.start() < position:
            failures.append(f"pipeline call is out of order: {step}()")
        position = match.start()

    for pattern in LOW_LEVEL_PATTERNS:
        if re.search(pattern, source):
            failures.append(f"is.cpp still exposes low-level detail matching: {pattern}")

    required_files = [
        "LCI/irregular/am_exchange_options.hpp",
        "LCI/irregular/irregular_runtime.hpp",
        "LCI/irregular/irregular_runtime.cpp",
        "LCI/irregular/detail/am_exchange.hpp",
        "LCI/irregular/detail/am_exchange_state.hpp",
        "LCI/irregular/detail/am_exchange_state.cpp",
        "LCI/integer_sort/key_redistribution.hpp",
        "LCI/integer_sort/key_redistribution.cpp",
    ]
    for relpath in required_files:
        if not (ROOT / relpath).exists():
            failures.append(f"missing reusable irregular interface file: {relpath}")

    removed_files = [
        "LCI/runtime/lci_runtime.hpp",
        "LCI/runtime/lci_runtime.cpp",
        "LCI/communication/lci_redistributor.hpp",
        "LCI/communication/lci_redistributor.cpp",
    ]
    for relpath in removed_files:
        if (ROOT / relpath).exists():
            failures.append(f"old low-level communication boundary still exists: {relpath}")

    if (ROOT / "LCI/integer_sort/key_redistribution.cpp").exists():
        redistribution_source = (ROOT / "LCI/integer_sort/key_redistribution.cpp").read_text()
        for required_token in ["bucket_to_rank", "frequency_histogram", "am_exchange_start", "make_sender"]:
            if required_token not in redistribution_source:
                failures.append(f"IS key redistribution wrapper does not contain: {required_token}")
        for forbidden_token in ["post_am", "get_upacket", "alloc_handler"]:
            if forbidden_token in redistribution_source:
                failures.append(f"IS key redistribution wrapper exposes LCI AM detail: {forbidden_token}")

    for forbidden_token in [
        "communication/lci_redistributor",
        "runtime/lci_runtime",
        "RedistributorRuntime",
        "bucket_to_rank",
        "frequency_histogram",
    ]:
        if forbidden_token in source:
            failures.append(f"is.cpp exposes implementation detail: {forbidden_token}")

    irregular_paths = [
        path for path in (ROOT / "LCI" / "irregular").rglob("*") if path.suffix in {".hpp", ".cpp"}
    ]
    irregular_source = "\n".join(path.read_text() for path in irregular_paths)
    for forbidden_pattern in [
        r"\b_OPENMP\b",
        r"\bomp_[A-Za-z0-9_]+",
        r"#pragma\s+omp",
        r"<omp\.h>",
        r"max_threads",
        r"threads_per_device",
    ]:
        if re.search(forbidden_pattern, irregular_source):
            failures.append(f"generic irregular runtime depends on thread-model detail: {forbidden_pattern}")

    for forbidden_token in [
        "KeyValue",
        "KeyCount",
        "bucket",
        "frequency_histogram",
        "problem_config",
        "is_lci",
    ]:
        if forbidden_token in irregular_source:
            failures.append(f"generic irregular runtime exposes IS detail: {forbidden_token}")

    for required_token in [
        "device_count",
        "progress",
        "class AmExchange",
        "am_exchange_start",
        "make_sender",
        "am_send",
        "flush",
        "is_done",
        "wait",
    ]:
        if required_token not in irregular_source:
            failures.append(f"generic irregular runtime is missing public AM API token: {required_token}")

    if failures:
        print("LCI interface layout check failed:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("LCI interface layout check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
