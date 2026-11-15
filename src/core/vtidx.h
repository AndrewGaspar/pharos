#pragma once
#include <cstddef>

// Vtable slot indices, derived from the C (CINTERFACE) declarations of the interfaces in
// vtidx.cpp so they can't drift from the headers.
namespace aco::vt {

#define ACO_VT(iface, method) extern const size_t iface##_##method;
#include "core/vtidx_list.inc"
#undef ACO_VT

} // namespace aco::vt
