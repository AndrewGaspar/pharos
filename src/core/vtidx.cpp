// Compiled with CINTERFACE so the headers expose the C vtable structs; offsetof on those gives
// the slot index of each method.
#define CINTERFACE
#define COBJMACROS
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstddef>

namespace aco::vt {

using Device = ID3D11DeviceVtbl;
using Context = ID3D11DeviceContextVtbl;
using Factory = IDXGIFactoryVtbl;
using Factory2 = IDXGIFactory2Vtbl;
using SwapChain = IDXGISwapChainVtbl;
using SwapChain1 = IDXGISwapChain1Vtbl;

#define ACO_VT(iface, method) extern const size_t iface##_##method = offsetof(iface, method) / sizeof(void*);
#include "core/vtidx_list.inc"
#undef ACO_VT

} // namespace aco::vt
