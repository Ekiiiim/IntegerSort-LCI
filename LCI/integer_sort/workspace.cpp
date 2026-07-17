#include "integer_sort/workspace.hpp"

namespace is_lci {

IntegerSortWorkspace::IntegerSortWorkspace(KeyCount local_key_count, KeyCount work_buffer_size,
                                           int bucket_workspace_size)
    : local_key_count_(local_key_count), work_buffer_size_(work_buffer_size),
      keys_(static_cast<size_t>(work_buffer_size)),
      frequency_storage_(new std::atomic<KeyCount>[static_cast<size_t>(work_buffer_size)]),
      cumulative_storage_(static_cast<size_t>(work_buffer_size)),
      local_bucket_counts_(static_cast<size_t>(bucket_workspace_size)),
      global_bucket_counts_(static_cast<size_t>(bucket_workspace_size)),
      first_bucket_by_rank_(static_cast<size_t>(bucket_workspace_size)),
      last_bucket_by_rank_(static_cast<size_t>(bucket_workspace_size)),
      bucket_to_rank_(static_cast<size_t>(bucket_workspace_size)) {}

KeyCount IntegerSortWorkspace::local_key_count() const {
  return local_key_count_;
}

KeyCount IntegerSortWorkspace::work_buffer_size() const {
  return work_buffer_size_;
}

KeyValue* IntegerSortWorkspace::keys() {
  return keys_.data();
}

const KeyValue* IntegerSortWorkspace::keys() const {
  return keys_.data();
}

KeyCount* IntegerSortWorkspace::local_bucket_counts() {
  return local_bucket_counts_.data();
}

KeyCount* IntegerSortWorkspace::global_bucket_counts() {
  return global_bucket_counts_.data();
}

int* IntegerSortWorkspace::first_bucket_by_rank() {
  return first_bucket_by_rank_.data();
}

int* IntegerSortWorkspace::last_bucket_by_rank() {
  return last_bucket_by_rank_.data();
}

int* IntegerSortWorkspace::bucket_to_rank() {
  return bucket_to_rank_.data();
}

std::atomic<KeyCount>* IntegerSortWorkspace::frequency_storage() {
  return frequency_storage_.get();
}

KeyRank* IntegerSortWorkspace::cumulative_storage() {
  return cumulative_storage_.data();
}

std::atomic<KeyCount>* IntegerSortWorkspace::frequency_by_key(KeyValue min_key_value) {
  return frequency_storage_.get() - min_key_value;
}

KeyRank* IntegerSortWorkspace::cumulative_by_key(KeyValue min_key_value) {
  return cumulative_storage_.data() - min_key_value;
}

void IntegerSortWorkspace::clear_bucket_metadata() {
  int metadata_size = static_cast<int>(local_bucket_counts_.size());

#pragma omp parallel for schedule(static)
  for (int i = 0; i < metadata_size; ++i) {
    local_bucket_counts_[i] = 0;
    global_bucket_counts_[i] = 0;
    first_bucket_by_rank_[i] = 0;
    last_bucket_by_rank_[i] = 0;
    bucket_to_rank_[i] = 0;
  }
}

void IntegerSortWorkspace::clear_frequency_range(KeyValue min_key_value, KeyValue max_key_value) {
  if (max_key_value < min_key_value) {
    return;
  }

  KeyCount key_range = static_cast<KeyCount>(max_key_value - min_key_value + 1);

#pragma omp parallel for schedule(static)
  for (KeyCount i = 0; i < key_range; ++i) {
    frequency_storage_[i].store(0, std::memory_order_relaxed);
    cumulative_storage_[static_cast<size_t>(i)] = 0;
  }
}

} // namespace is_lci
