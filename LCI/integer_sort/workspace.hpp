#pragma once

#include "types.hpp"

#include <atomic>
#include <memory>
#include <vector>

namespace is_lci {

// Owns the reusable storage used by the integer-sort pipeline. The benchmark
// driver names algorithm steps; this class hides the raw arrays those steps use.
class IntegerSortWorkspace {
public:
  IntegerSortWorkspace(KeyCount local_key_count, KeyCount work_buffer_size, int bucket_workspace_size);

  KeyCount local_key_count() const;
  KeyCount work_buffer_size() const;

  KeyValue* keys();
  const KeyValue* keys() const;

  KeyCount* local_bucket_counts();
  KeyCount* global_bucket_counts();
  int* first_bucket_by_rank();
  int* last_bucket_by_rank();
  int* bucket_to_rank();

  std::atomic<KeyCount>* frequency_storage();
  KeyRank* cumulative_storage();

  std::atomic<KeyCount>* frequency_by_key(KeyValue min_key_value);
  KeyRank* cumulative_by_key(KeyValue min_key_value);

  void clear_bucket_metadata();
  void clear_frequency_range(KeyValue min_key_value, KeyValue max_key_value);

private:
  KeyCount local_key_count_;
  KeyCount work_buffer_size_;

  std::vector<KeyValue> keys_;
  std::unique_ptr<std::atomic<KeyCount>[]> frequency_storage_;
  std::vector<KeyRank> cumulative_storage_;

  std::vector<KeyCount> local_bucket_counts_;
  std::vector<KeyCount> global_bucket_counts_;
  std::vector<int> first_bucket_by_rank_;
  std::vector<int> last_bucket_by_rank_;
  std::vector<int> bucket_to_rank_;
};

} // namespace is_lci
