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

    if failures:
        print("LCI interface layout check failed:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("LCI interface layout check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
