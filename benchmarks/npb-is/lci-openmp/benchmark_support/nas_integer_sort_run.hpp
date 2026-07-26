#pragma once

#include <lci-irregular/irregular_runtime.hpp>
#include "integer_sort/bucket_plan.hpp"
#include "integer_sort/workspace.hpp"
#include "nas/verification.hpp"

#include <memory>

namespace lci_irregular {
class IrregularRuntime;
}

namespace is_lci {

// High-level NAS IS benchmark run. Public callers use these methods as an
// algorithm script; implementation files keep the benchmark and LCI details.
class NasIntegerSortRun {
public:
  explicit NasIntegerSortRun(lci_irregular::IrregularRuntime& runtime);
  ~NasIntegerSortRun();

  NasIntegerSortRun(const NasIntegerSortRun&) = delete;
  NasIntegerSortRun& operator=(const NasIntegerSortRun&) = delete;

  bool initialize();
  int exit_code() const;

  void generate_keys();
  void run_warmup_iteration();

  void start_timed_region();
  void begin_iteration(int iteration);
  void apply_nas_key_changes();
  void organize_keys_into_buckets();
  void compute_global_bucket_sizes();
  void assign_buckets_to_processes();
  void redistribute_keys_with_lci();
  void compute_final_ranking();
  void verify_partial_ranking();
  void finish_iteration();
  void stop_timed_region();

  void verify_full_ranking();
  void print_results();

private:
  void configure_output();
  bool determine_active_rank_count();
  lci_irregular::AmOptions am_exchange_options() const;
  IntegerSortWorkspace& workspace();
  const IntegerSortWorkspace& workspace() const;
  void run_current_iteration_pipeline();

  lci_irregular::IrregularRuntime& runtime_;
  int active_rank_count_ = 0;
  int exit_code_ = 0;
  int current_iteration_ = 0;
  int use_upacket_ = 0;
  int use_loopback_ = 0;
  int passed_verification_ = 0;
  double max_time_ = 0.0;

  FullVerifySnapshot final_snapshot_{};
  BucketPlan current_bucket_plan_{};
  lci_irregular::AmProfile current_redistribution_profile_{};
  std::unique_ptr<IntegerSortWorkspace> workspace_;
};

} // namespace is_lci
