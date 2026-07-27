#include <lci-irregular/irregular_runtime.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

struct Record {
  uint64_t key;
  double value;
};

int main() {
  lci_irregular::IrregularRuntimeOptions runtime_options;
  const char* profile_directory = std::getenv("LCI_IRREGULAR_PROFILE_DIR");
  if (profile_directory != nullptr && *profile_directory != '\0') {
    runtime_options.profiling.enabled = true;
    runtime_options.profiling.worker_count = 1;
    runtime_options.profiling.output_directory = profile_directory;
    runtime_options.profiling.file_prefix = "typed-am-example";
  }

  lci_irregular::IrregularRuntime runtime(runtime_options);
  auto am_handler = [&](lci_irregular::AmRecords<Record> records, int source_rank) {
    for (size_t i = 0; i < records.size(); ++i) {
      Record record = records[i];
      std::printf("rank %d received key=%llu value=%.1f from rank %d\n", runtime.rank(),
                  static_cast<unsigned long long>(record.key), record.value, source_rank);
    }
  };

  lci_irregular::AmOptions am_options;
  am_options.profile_name = "typed-am-exchange";
  const int destination = (runtime.rank() + 1) % runtime.rank_count();

  runtime.set_am_handler<Record>(am_handler, am_options);
  runtime.post_am(0, destination, Record{static_cast<uint64_t>(runtime.rank()), runtime.rank() + 0.5});
  runtime.flush_remaining_buffers(0);
  while (runtime.received_record_count() < 1) {
    runtime.progress(0);
  }
  runtime.quiet(0);

  const auto profile = runtime.am_profile();
  runtime.clear_am_handler();
  if (profile.enabled) {
    std::printf("rank %d aggregate flush calls: %llu\n", runtime.rank(),
                static_cast<unsigned long long>(profile.aggregate.flush.calls));
  }
}
