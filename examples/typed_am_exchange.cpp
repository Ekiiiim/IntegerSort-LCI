#include <lci-irregular/irregular_runtime.hpp>

#include <atomic>
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
  std::atomic<size_t> received_count{0};
  auto receive_batch = [&](lci_irregular::RecordBatchView<Record> records, int source_rank) noexcept {
    for (size_t i = 0; i < records.size(); ++i) {
      Record record = records[i];
      std::printf("rank %d received key=%llu value=%.1f from rank %d\n", runtime.rank(),
                  static_cast<unsigned long long>(record.key), record.value, source_rank);
    }
    received_count.fetch_add(records.size(), std::memory_order_relaxed);
  };
  auto is_done = [&]() noexcept { return received_count.load(std::memory_order_relaxed) == 1; };

  lci_irregular::AmExchangeOptions exchange_options;
  exchange_options.profile_name = "typed-am-exchange";
  auto exchange = runtime.am_exchange_start<Record>(receive_batch, is_done, exchange_options);
  runtime.barrier();
  auto sender = exchange.make_sender(0);
  const int destination = (runtime.rank() + 1) % runtime.rank_count();
  sender.am_send(destination, Record{static_cast<uint64_t>(runtime.rank()), runtime.rank() + 0.5});
  sender.close();
  exchange.wait();

  const auto profile = exchange.profile();
  if (profile.enabled) {
    std::printf("rank %d aggregate flush calls: %llu\n", runtime.rank(),
                static_cast<unsigned long long>(profile.aggregate.flush.calls));
  }
}
