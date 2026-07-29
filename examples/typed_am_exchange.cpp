#include <lci-irregular/irregular_runtime.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

struct Record {
  uint64_t key;
  double value;
};

int main() {
  lci_irregular::RuntimeOptions runtime_options;
  const char* profile_directory = std::getenv("LCI_IRREGULAR_PROFILE_DIR");
  if (profile_directory != nullptr && *profile_directory != '\0') {
    runtime_options.profiling.enabled = true;
    runtime_options.profiling.worker_count = 1;
    runtime_options.profiling.output_directory = profile_directory;
    runtime_options.profiling.file_prefix = "typed-am-example";
  }

  lci_irregular::Runtime runtime(runtime_options);
  lci_irregular::WorkerHandler worker = runtime.get_worker_handler(0);
  auto am_handler = [&](const Record& record, int source_rank) {
    std::printf("rank %d received key=%llu value=%.1f from rank %d\n", runtime.rank(),
                static_cast<unsigned long long>(record.key), record.value, source_rank);
  };

  lci_irregular::AmOptions am_options;
  am_options.profile_name = "typed-am-exchange";
  const int destination = (runtime.rank() + 1) % runtime.rank_count();

  runtime.set_am_handler<Record>(am_handler, am_options);
  worker.post_am(destination, Record{static_cast<uint64_t>(runtime.rank()), runtime.rank() + 0.5});
  worker.flush();
  while (runtime.recv_count() < 1) {
    worker.progress();
  }

  const auto profile = runtime.am_profile();
  runtime.clear_am_handler();
  if (profile.enabled) {
    std::printf("rank %d aggregate flush calls: %llu\n", runtime.rank(),
                static_cast<unsigned long long>(profile.aggregate.flush.calls));
  }
}
