#pragma once

#include <cstddef>

namespace is_lci {

void sum_op_int(const void* left, const void* right, void* dst, size_t n);
void sum_op_double(const void* left, const void* right, void* dst, size_t n);
void max_op(const void* left, const void* right, void* dst, size_t n);
void min_op(const void* left, const void* right, void* dst, size_t n);

} // namespace is_lci
