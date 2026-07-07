#include "communication/reductions.hpp"

#include "types.hpp"

namespace is_lci {

void sum_op_int(const void* left, const void* right, void* dst, size_t n) {
  const KeyCount* left_ = static_cast<const KeyCount*>(left);
  const KeyCount* right_ = static_cast<const KeyCount*>(right);
  KeyCount* dst_ = static_cast<KeyCount*>(dst);
  for (size_t i = 0; i < n; ++i) {
    dst_[i] = left_[i] + right_[i];
  }
}

void sum_op_double(const void* left, const void* right, void* dst, size_t n) {
  const double* left_ = static_cast<const double*>(left);
  const double* right_ = static_cast<const double*>(right);
  double* dst_ = static_cast<double*>(dst);
  for (size_t i = 0; i < n; ++i) {
    dst_[i] = left_[i] + right_[i];
  }
}

void max_op(const void* left, const void* right, void* dst, size_t n) {
  const double* left_ = static_cast<const double*>(left);
  const double* right_ = static_cast<const double*>(right);
  double* dst_ = static_cast<double*>(dst);
  for (size_t i = 0; i < n; ++i) {
    dst_[i] = (left_[i] > right_[i]) ? left_[i] : right_[i];
  }
}

void min_op(const void* left, const void* right, void* dst, size_t n) {
  const double* left_ = static_cast<const double*>(left);
  const double* right_ = static_cast<const double*>(right);
  double* dst_ = static_cast<double*>(dst);
  for (size_t i = 0; i < n; ++i) {
    dst_[i] = (left_[i] < right_[i]) ? left_[i] : right_[i];
  }
}

} // namespace is_lci
