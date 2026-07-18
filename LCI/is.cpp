/*************************************************************************
 *                                                                       *
 *        N  A  S     P A R A L L E L     B E N C H M A R K S  3.4       *
 *                                                                       *
 *                                  I S                                  *
 *                                                                       *
 *************************************************************************
 *                                                                       *
 *   This benchmark is part of the NAS Parallel Benchmark 3.4 suite.     *
 *   It is described in NAS Technical Report 95-020.                     *
 *                                                                       *
 *   Permission to use, copy, distribute and modify this software        *
 *   for any purpose with or without fee is hereby granted.  We          *
 *   request, however, that all derived work reference the NAS           *
 *   Parallel Benchmarks 3.4. This software is provided "as is"          *
 *   without express or implied warranty.                                *
 *                                                                       *
 *   Information on NPB 3.4, including the technical report, the         *
 *   original specifications, source code, results and information       *
 *   on how to submit new results, is available at:                      *
 *                                                                       *
 *          http://www.nas.nasa.gov/Software/NPB                         *
 *                                                                       *
 *   Send comments or suggestions to  npb@nas.nasa.gov                   *
 *                                                                       *
 *         NAS Parallel Benchmarks Group                                 *
 *         NASA Ames Research Center                                     *
 *         Moffett Field, CA   94035-1000                                *
 *                                                                       *
 *************************************************************************
 *                                                                       *
 *   Author: M. Yarrow                                                   *
 *           H. Jin                                                      *
 *                                                                       *
 *************************************************************************/

/*************************************************************************
 *  LCI + OpenMP variant of NPB IS -- Multithreaded Fine-grained
 *  Asynchronous BSP (FA-BSP).
 *
 *  This executable is intentionally written as an algorithm script. The
 *  implementation details for each step live under irregular/,
 *  benchmark_support/, integer_sort/, communication/, and nas/.
 *************************************************************************/

#include "benchmark_support/nas_integer_sort_run.hpp"
#include "benchmark_support/run_options.hpp"
#include "irregular/irregular_runtime.hpp"
#include "nas/problem_config.hpp"

int main() {
  lci_irregular::IrregularRuntimeOptions runtime_options;
  runtime_options.threads_per_device = is_lci::read_threads_per_device_option();
  lci_irregular::IrregularRuntime runtime(runtime_options);
  is_lci::NasIntegerSortRun run(runtime);
  if (!run.initialize()) {
    return run.exit_code();
  }

  run.generate_keys();
  run.run_warmup_iteration();

  run.start_timed_region();
  for (int iteration = 1; iteration <= MAX_ITERATIONS; ++iteration) {
    run.begin_iteration(iteration);

    run.apply_nas_key_changes();
    run.organize_keys_into_buckets();
    run.compute_global_bucket_sizes();
    run.assign_buckets_to_processes();
    run.redistribute_keys_with_lci();
    run.compute_final_ranking();
    run.verify_partial_ranking();

    run.finish_iteration();
  }
  run.stop_timed_region();

  run.verify_full_ranking();
  run.print_results();

  return run.exit_code();
}
