#include "fg/fg.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#define VK_NO_PROTOTYPES
#include "fg/dxvk_interop.h"

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_params.h>
#include <nvsdk_ngx_helpers_vk.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "dlss/ngx.h"
#include "dlss/upscaler.h"
#include "fg/vk_device_ext.h"
#include "fg/fg_d3d12.h"
#include "core/iat.h"

namespace aco {

namespace {

int64_t now_us() {
  static const int64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return c.QuadPart / freq * 1000000 + c.QuadPart % freq * 1000000 / freq;
}

template <typename T>
void safe_release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

// ---- Vulkan entry points (winevulkan's vulkan-1.dll; DXVK's handles are winevulkan handles) ----
struct Vk {
  PFN_vkGetInstanceProcAddr gipa = nullptr;
  PFN_vkGetDeviceProcAddr gdpa = nullptr;
  PFN_vkCreateCommandPool CreateCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
  PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
  PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
  PFN_vkQueueSubmit QueueSubmit = nullptr;
  PFN_vkCreateFence CreateFence = nullptr;
  PFN_vkWaitForFences WaitForFences = nullptr;
  PFN_vkResetFences ResetFences = nullptr;
  PFN_vkCreateImageView CreateImageView = nullptr;
  PFN_vkDestroyImageView DestroyImageView = nullptr;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  PFN_vkCreateBuffer CreateBuffer = nullptr;
  PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
  PFN_vkAllocateMemory AllocateMemory = nullptr;
  PFN_vkBindBufferMemory BindBufferMemory = nullptr;
  PFN_vkMapMemory MapMemory = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkCreateQueryPool CreateQueryPool = nullptr;
  PFN_vkCmdResetQueryPool CmdResetQueryPool = nullptr;
  PFN_vkCmdWriteTimestamp CmdWriteTimestamp = nullptr;
  PFN_vkGetQueryPoolResults GetQueryPoolResults = nullptr;
  PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
  PFN_vkCreateImage CreateImage = nullptr;
  PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
  PFN_vkBindImageMemory BindImageMemory = nullptr;
  PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
  PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;
} vk;

// ---- NGX Vulkan API (driver _nvngx.dll) ----
using PFN_VkInit = NVSDK_NGX_Result(__cdecl*)(unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice,
                                              VkDevice, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr,
                                              const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
using PFN_GetParams = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Parameter**);
using PFN_CreateFeature1 = NVSDK_NGX_Result(__cdecl*)(VkDevice, VkCommandBuffer, NVSDK_NGX_Feature,
                                                      NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFN_Evaluate = NVSDK_NGX_Result(__cdecl*)(VkCommandBuffer, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                                                void*);
using PFN_Release = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Handle*);

constexpr unsigned long long kAppId = 0x41434f444c5353ull;
constexpr int kRing = 4;

struct State {
  bool tried = false, ok = false;
  uint64_t frames = 0;
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  IDXGIVkInteropDevice* interop = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice pd = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd[kRing] = {};
  VkFence fence[kRing] = {};
  // D3D11 resources used by each in-flight submission. DXVK only tracks its own GPU work, so we keep
  // them alive until our fence signals.
  std::vector<ID3D11Resource*> held[kRing];
  std::vector<ID3D11Resource*> pending_refs;  // for the submission being recorded
  int ring = 0;
  // Per-slot "disable interpolation" output buffers (DLSS-G writes 1 to byte 0 when it decided not
  // to generate a frame), host-visible so we can read them after the slot's fence.
  VkBuffer disable_buf[kRing] = {};
  volatile uint8_t* disable_map[kRing] = {};
  // GPU time of each submission (timestamps at the start/end of our command buffer).
  VkQueryPool timestamps = VK_NULL_HANDLE;
  bool ts_written[kRing] = {};
  double ts_period_ns = 1.0;
  double perf_gpu_ms = 0;
  uint32_t perf_gpu_n = 0;

  PFN_GetParams alloc_params = nullptr;
  PFN_CreateFeature1 create = nullptr;
  PFN_Evaluate evaluate = nullptr;
  PFN_Release release = nullptr;
  ngx::Params create_params, eval_params;
  NVSDK_NGX_Handle* feature = nullptr;
  uint32_t feat_w = 0, feat_h = 0, feat_rw = 0, feat_rh = 0;
  VkFormat feat_format = VK_FORMAT_UNDEFINED;

  // Display-size D3D11 textures owned by us (DXVK images): the real frame, the generated frame.
  ID3D11Texture2D* real = nullptr;
  ID3D11Texture2D* interp = nullptr;
  ID3D11Texture2D* outreal = nullptr;  // debug: DLSS-G's OutputReal ([fg] show=outreal)
  ID3D11Texture2D* scratch = nullptr;  // output for generated frames we don't present
  ID3D11UnorderedAccessView* interp_uav = nullptr;  // debug: pre-fill with magenta
  uint32_t tex_w = 0, tex_h = 0;
  DXGI_FORMAT tex_format = DXGI_FORMAT_UNKNOWN;

  struct View {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint64_t last_used = 0;
  };
  std::unordered_map<ID3D11Resource*, View> views;  // holds a reference to each key
  bool reset = true;
  uint64_t last_input_frame = 0;
  int64_t last_real_present = 0;
  double frame_interval_us = 0;  // EMA of real frame intervals

  // perf
  int64_t perf_last = 0;
  uint32_t perf_real = 0, perf_generated = 0, perf_fail = 0, perf_disabled = 0, perf_resets = 0;
  double perf_eval_cpu_ms = 0;
} S;

template <typename T>
bool dev_fn(T& out, const char* name) {
  out = reinterpret_cast<T>(vk.gdpa(S.device, name));
  if (!out) ACO_LOG("fg: vkGetDeviceProcAddr(%s) failed", name);
  return out != nullptr;
}

bool load_vulkan() {
  HMODULE m = LoadLibraryW(L"vulkan-1.dll");
  if (!m) return false;
  vk.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(m, "vkGetInstanceProcAddr"));
  if (!vk.gipa) return false;
  vk.gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(vk.gipa(S.instance, "vkGetDeviceProcAddr"));
  return vk.gdpa && dev_fn(vk.CreateCommandPool, "vkCreateCommandPool") &&
         dev_fn(vk.AllocateCommandBuffers, "vkAllocateCommandBuffers") &&
         dev_fn(vk.BeginCommandBuffer, "vkBeginCommandBuffer") && dev_fn(vk.EndCommandBuffer, "vkEndCommandBuffer") &&
         dev_fn(vk.ResetCommandBuffer, "vkResetCommandBuffer") && dev_fn(vk.QueueSubmit, "vkQueueSubmit") &&
         dev_fn(vk.CreateFence, "vkCreateFence") && dev_fn(vk.WaitForFences, "vkWaitForFences") &&
         dev_fn(vk.ResetFences, "vkResetFences") && dev_fn(vk.CreateImageView, "vkCreateImageView") &&
         dev_fn(vk.DestroyImageView, "vkDestroyImageView") &&
         dev_fn(vk.CmdPipelineBarrier, "vkCmdPipelineBarrier") && dev_fn(vk.CreateBuffer, "vkCreateBuffer") &&
         dev_fn(vk.GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements") &&
         dev_fn(vk.AllocateMemory, "vkAllocateMemory") && dev_fn(vk.BindBufferMemory, "vkBindBufferMemory") &&
         dev_fn(vk.MapMemory, "vkMapMemory") && dev_fn(vk.CreateQueryPool, "vkCreateQueryPool") &&
         dev_fn(vk.CmdResetQueryPool, "vkCmdResetQueryPool") && dev_fn(vk.CmdWriteTimestamp, "vkCmdWriteTimestamp") &&
         dev_fn(vk.GetQueryPoolResults, "vkGetQueryPoolResults") && dev_fn(vk.CreateImage, "vkCreateImage") &&
         dev_fn(vk.GetImageMemoryRequirements, "vkGetImageMemoryRequirements") &&
         dev_fn(vk.BindImageMemory, "vkBindImageMemory") && dev_fn(vk.CmdCopyImageToBuffer, "vkCmdCopyImageToBuffer") &&
         dev_fn(vk.CmdClearColorImage, "vkCmdClearColorImage") &&
         (vk.GetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
              vk.gipa(S.instance, "vkGetPhysicalDeviceProperties"))) != nullptr &&
         (vk.GetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
              vk.gipa(S.instance, "vkGetPhysicalDeviceMemoryProperties"))) != nullptr;
}

// Full barrier: makes DXVK's earlier writes visible to NGX and NGX's writes to DXVK's later work
// (pipeline barriers cover earlier/later submissions on the same queue).
void full_barrier(VkCommandBuffer cmd) {
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                        nullptr, 0, nullptr);
}

// Records into the next ring command buffer and submits it on DXVK's queue after everything D3D11
// has recorded so far. wait=true blocks until the GPU finished it.
template <typename F>
bool submit(F&& record, bool wait) {
  int i = S.ring;
  S.ring = (S.ring + 1) % kRing;
  vk.WaitForFences(S.device, 1, &S.fence[i], VK_TRUE, UINT64_MAX);
  vk.ResetFences(S.device, 1, &S.fence[i]);
  for (auto* r : S.held[i]) r->Release();
  S.held[i].swap(S.pending_refs);
  S.pending_refs.clear();
  if (S.timestamps && S.ts_written[i]) {
    uint64_t ts[2] = {};
    if (vk.GetQueryPoolResults(S.device, S.timestamps, i * 2, 2, sizeof(ts), ts, sizeof(uint64_t),
                               VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      S.perf_gpu_ms += double(ts[1] - ts[0]) * S.ts_period_ns / 1e6;
      ++S.perf_gpu_n;
    }
    S.ts_written[i] = false;
  }
  vk.ResetCommandBuffer(S.cmd[i], 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vk.BeginCommandBuffer(S.cmd[i], &bi);
  if (S.timestamps) {
    vk.CmdResetQueryPool(S.cmd[i], S.timestamps, i * 2, 2);
    vk.CmdWriteTimestamp(S.cmd[i], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, S.timestamps, i * 2);
  }
  full_barrier(S.cmd[i]);
  bool ok = record(S.cmd[i]);
  full_barrier(S.cmd[i]);
  if (S.timestamps) {
    vk.CmdWriteTimestamp(S.cmd[i], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, S.timestamps, i * 2 + 1);
    S.ts_written[i] = true;
  }
  vk.EndCommandBuffer(S.cmd[i]);
  S.interop->FlushRenderingCommands();
  S.interop->LockSubmissionQueue();
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &S.cmd[i];
  VkResult r = vk.QueueSubmit(S.queue, 1, &si, S.fence[i]);
  S.interop->ReleaseSubmissionQueue();
  if (r != VK_SUCCESS) {
    ACO_LOG("fg: vkQueueSubmit -> %d", r);
    return false;
  }
  if (wait) vk.WaitForFences(S.device, 1, &S.fence[i], VK_TRUE, UINT64_MAX);
  return ok;
}

// NGX's own log lines (DLSS-G reports why it does or doesn't produce frames here).
std::atomic<int> g_ngx_log_lines{0};
void NVSDK_CONV ngx_log(const char* msg, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature feature) {
  if (g_ngx_log_lines++ > 4000) return;
  std::string s(msg ? msg : "");
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  ACO_LOG("ngx[%d]: %s", static_cast<int>(feature), s.c_str());
}

// ---- Diagnostics: how NGX creates and launches its CUDA kernels through Vulkan ([fg] trace_cu=1) --
PFN_vkCreateCuModuleNVX g_real_create_module = nullptr;
PFN_vkCreateCuFunctionNVX g_real_create_function = nullptr;
PFN_vkCmdCuLaunchKernelNVX g_real_launch = nullptr;
std::atomic<int> g_cu_logged{0};
std::atomic<uint32_t> g_cu_launches{0};

VkResult VKAPI_CALL tr_create_module(VkDevice d, const VkCuModuleCreateInfoNVX* ci, const VkAllocationCallbacks* a,
                                     VkCuModuleNVX* m) {
  VkResult r = g_real_create_module(d, ci, a, m);
  if (config().fg_trace_cu && g_cu_logged++ < 60)
    ACO_LOG("fg: cu: vkCreateCuModuleNVX(size %zu) -> %d", ci ? ci->dataSize : 0, r);
  return r;
}
VkResult VKAPI_CALL tr_create_function(VkDevice d, const VkCuFunctionCreateInfoNVX* ci, const VkAllocationCallbacks* a,
                                       VkCuFunctionNVX* f) {
  VkResult r = g_real_create_function(d, ci, a, f);
  if (config().fg_trace_cu && g_cu_logged++ < 60)
    ACO_LOG("fg: cu: vkCreateCuFunctionNVX(%s) -> %d", ci && ci->pName ? ci->pName : "?", r);
  return r;
}
void VKAPI_CALL tr_launch(VkCommandBuffer cb, const VkCuLaunchInfoNVX* li) {
  ++g_cu_launches;
  if (!config().fg_trace_cu) return g_real_launch(cb, li);
  static std::atomic<int> launch_logged{0};
  if (li && launch_logged++ < 12) {
    ACO_LOG("fg: cu: launch fn=%p grid %ux%ux%u block %ux%ux%u shmem %u params %zu (p=%p) extras %zu (e=%p)",
            static_cast<void*>(li->function), li->gridDimX, li->gridDimY, li->gridDimZ, li->blockDimX, li->blockDimY,
            li->blockDimZ, li->sharedMemBytes, li->paramCount, li->pParams, li->extraCount, li->pExtras);
    for (size_t i = 0; i < li->extraCount && i < 6; ++i)
      ACO_LOG("fg: cu:   extra[%zu] = %p", i, li->pExtras ? li->pExtras[i] : nullptr);
  }
  g_real_launch(cb, li);
}
// [fg] hide_handle64=1: don't let DLSS-G use 64-bit image view handles ("texmode_raw").
bool hidden(const char* name) {
  return name && config().fg_hide_handle64 &&
         (!strcmp(name, "vkGetImageViewHandle64NVX") || !strcmp(name, "vkGetImageViewAddressNVX"));
}

PFN_vkVoidFunction VKAPI_CALL tr_gdpa(VkDevice d, const char* name) {
  if (hidden(name)) {
    ACO_LOG("fg: hiding %s from NGX", name);
    return nullptr;
  }
  if (name && !strcmp(name, "vkCmdCuLaunchKernelNVX")) {
    g_real_launch = reinterpret_cast<PFN_vkCmdCuLaunchKernelNVX>(vk.gdpa(d, name));
    return g_real_launch ? reinterpret_cast<PFN_vkVoidFunction>(tr_launch) : nullptr;
  }
  if (name && !strcmp(name, "vkCreateCuModuleNVX")) {
    g_real_create_module = reinterpret_cast<PFN_vkCreateCuModuleNVX>(vk.gdpa(d, name));
    return g_real_create_module ? reinterpret_cast<PFN_vkVoidFunction>(tr_create_module) : nullptr;
  }
  if (name && !strcmp(name, "vkCreateCuFunctionNVX")) {
    g_real_create_function = reinterpret_cast<PFN_vkCreateCuFunctionNVX>(vk.gdpa(d, name));
    return g_real_create_function ? reinterpret_cast<PFN_vkVoidFunction>(tr_create_function) : nullptr;
  }
  if (name && !strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(tr_gdpa);
  return vk.gdpa(d, name);
}
PFN_vkVoidFunction VKAPI_CALL tr_gipa(VkInstance i, const char* name);
using PFN_GetProcAddressW = FARPROC(WINAPI*)(HMODULE, LPCSTR);
PFN_GetProcAddressW g_dlssg_gpa = nullptr;
// nvngx_dlssg.dll resolves Vulkan itself (GetProcAddress on vulkan-1.dll): route its lookups through
// the tracers above.
FARPROC WINAPI tr_dlssg_gpa(HMODULE m, LPCSTR name) {
  FARPROC f = g_dlssg_gpa(m, name);
  if (!IS_INTRESOURCE(name) && hidden(name)) {
    ACO_LOG("fg: hiding %s from nvngx_dlssg (GetProcAddress)", name);
    return nullptr;
  }
  if (f && !IS_INTRESOURCE(name)) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<FARPROC>(tr_gdpa);
    if (!strcmp(name, "vkGetInstanceProcAddr")) return reinterpret_cast<FARPROC>(tr_gipa);
    if (!strcmp(name, "vkCmdCuLaunchKernelNVX")) {
      g_real_launch = reinterpret_cast<PFN_vkCmdCuLaunchKernelNVX>(f);
      return reinterpret_cast<FARPROC>(tr_launch);
    }
  }
  return f;
}

PFN_vkVoidFunction VKAPI_CALL tr_gipa(VkInstance i, const char* name) {
  if (name && !strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(tr_gdpa);
  if (name && !strcmp(name, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(tr_gipa);
  return vk.gipa(i, name);
}

bool init(IDXGISwapChain* sc) {
  if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&S.dev)))) return false;
  S.dev->GetImmediateContext(&S.imm);
  HRESULT hr = S.dev->QueryInterface(__uuidof(IDXGIVkInteropDevice), reinterpret_cast<void**>(&S.interop));
  if (FAILED(hr) || !S.interop) {
    ACO_LOG("fg: device has no IDXGIVkInteropDevice (not DXVK?) hr=0x%08lx", hr);
    return false;
  }
  S.interop->GetVulkanHandles(&S.instance, &S.pd, &S.device);
  S.interop->GetSubmissionQueue(&S.queue, &S.family);
  if (!load_vulkan()) {
    ACO_LOG("fg: Vulkan loader (vulkan-1.dll) unavailable");
    return false;
  }
  HMODULE ngx = ngx::core_module();
  if (!ngx) return false;
  auto init_fn = reinterpret_cast<PFN_VkInit>(GetProcAddress(ngx, "NVSDK_NGX_VULKAN_Init"));
  auto caps_fn = reinterpret_cast<PFN_GetParams>(GetProcAddress(ngx, "NVSDK_NGX_VULKAN_GetCapabilityParameters"));
  S.alloc_params = reinterpret_cast<PFN_GetParams>(GetProcAddress(ngx, "NVSDK_NGX_VULKAN_AllocateParameters"));
  S.create = reinterpret_cast<PFN_CreateFeature1>(GetProcAddress(ngx, "NVSDK_NGX_VULKAN_CreateFeature1"));
  S.evaluate = reinterpret_cast<PFN_Evaluate>(GetProcAddress(ngx, "NVSDK_NGX_VULKAN_EvaluateFeature"));
  S.release = reinterpret_cast<PFN_Release>(GetProcAddress(ngx, "NVSDK_NGX_VULKAN_ReleaseFeature"));
  if (!init_fn || !caps_fn || !S.alloc_params || !S.create || !S.evaluate || !S.release) {
    ACO_LOG("fg: NGX Vulkan entry points missing");
    return false;
  }
  std::wstring dir = module_dir();
  const wchar_t* paths[] = {dir.c_str()};
  NVSDK_NGX_FeatureCommonInfo common{};
  common.PathListInfo.Path = paths;
  common.PathListInfo.Length = 1;
  if (config().fg_ngx_log) {
    common.LoggingInfo.LoggingCallback = ngx_log;
    common.LoggingInfo.MinimumLoggingLevel = config().fg_ngx_log >= 2 ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE
                                                                       : NVSDK_NGX_LOGGING_LEVEL_ON;
  }
  {
    // Load the DLSS-G snippet ourselves before NGX does and hook its GetProcAddress import (it
    // resolves Vulkan itself), for kernel tracing ([fg] trace_cu) and the hide_handle64 experiment.
    HMODULE dlssg = LoadLibraryW((dir + L"\\nvngx_dlssg.dll").c_str());
    if (dlssg && iat_hook(dlssg, "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(tr_dlssg_gpa),
                          reinterpret_cast<void**>(&g_dlssg_gpa)))
      ACO_LOG("fg: tracing nvngx_dlssg.dll Vulkan lookups");
  }
  int64_t t0 = now_us();
  NVSDK_NGX_Result r =
      init_fn(kAppId, dir.c_str(), S.instance, S.pd, S.device, tr_gipa, tr_gdpa, &common, NVSDK_NGX_Version_API);
  ACO_LOG("fg: NVSDK_NGX_VULKAN_Init -> %s (%.1f s)", ngx::result_str(r), (now_us() - t0) / 1e6);
  if (NVSDK_NGX_FAILED(r)) return false;
  NVSDK_NGX_Parameter* caps_raw = nullptr;
  caps_fn(&caps_raw);
  ngx::Params caps(caps_raw);
  int avail = -1;
  if (caps) caps.get(NVSDK_NGX_Parameter_FrameGeneration_Available, &avail);
  ACO_LOG("fg: FrameGeneration available=%d", avail);
  if (avail != 1) return false;

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = S.family;
  vk.CreateCommandPool(S.device, &pci, nullptr, &S.pool);
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = S.pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = kRing;
  vk.AllocateCommandBuffers(S.device, &ai, S.cmd);
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  for (auto& f : S.fence) vk.CreateFence(S.device, &fci, nullptr, &f);

  VkPhysicalDeviceProperties props{};
  vk.GetPhysicalDeviceProperties(S.pd, &props);
  S.ts_period_ns = props.limits.timestampPeriod;
  VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qci.queryCount = kRing * 2;
  vk.CreateQueryPool(S.device, &qci, nullptr, &S.timestamps);

  VkPhysicalDeviceMemoryProperties mp{};
  vk.GetPhysicalDeviceMemoryProperties(S.pd, &mp);
  for (int i = 0; i < kRing; ++i) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = 256;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vk.CreateBuffer(S.device, &bci, nullptr, &S.disable_buf[i]);
    VkMemoryRequirements req{};
    vk.GetBufferMemoryRequirements(S.device, S.disable_buf[i], &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; ++t)
      if ((req.memoryTypeBits & (1u << t)) &&
          (mp.memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
              (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        type = t;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* ptr = nullptr;
    if (type == UINT32_MAX || vk.AllocateMemory(S.device, &mai, nullptr, &mem) != VK_SUCCESS ||
        vk.BindBufferMemory(S.device, S.disable_buf[i], mem, 0) != VK_SUCCESS ||
        vk.MapMemory(S.device, mem, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS) {
      ACO_LOG("fg: disable-interpolation buffer allocation failed");
      S.disable_buf[i] = VK_NULL_HANDLE;
      continue;
    }
    S.disable_map[i] = static_cast<volatile uint8_t*>(ptr);
    S.disable_map[i][0] = 0xee;
  }

  NVSDK_NGX_Parameter* p = nullptr;
  S.alloc_params(&p);
  S.create_params = ngx::Params(p);
  p = nullptr;
  S.alloc_params(&p);
  S.eval_params = ngx::Params(p);
  return S.create_params && S.eval_params;
}

struct VkTex {
  VkImage image = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
};

bool vk_tex(ID3D11Resource* r, VkTex* t) {
  IDXGIVkInteropSurface* s = nullptr;
  if (FAILED(r->QueryInterface(__uuidof(IDXGIVkInteropSurface), reinterpret_cast<void**>(&s)))) return false;
  s->GetVulkanImageInfo(&t->image, &t->layout, &t->info);
  s->Release();
  return t->image != VK_NULL_HANDLE;
}

// NGX resource descriptor for a D3D11 texture. Views are cached per D3D11 resource (the cache holds
// a reference, so the VkImage can't be destroyed and its handle reused under a stale view), and the
// resource is also held by the submission being recorded.
bool ngx_res(ID3D11Resource* r, bool rw, NVSDK_NGX_Resource_VK* out, VkTex* tex_out = nullptr) {
  VkTex t;
  if (!vk_tex(r, &t)) return false;
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  auto it = S.views.find(r);
  if (it == S.views.end() || it->second.image != t.image) {
    if (it != S.views.end()) {  // shouldn't happen while we hold a reference
      vk.DestroyImageView(S.device, it->second.view, nullptr);
      it->first->Release();
      S.views.erase(it);
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t.info.format;
    vi.subresourceRange = range;
    VkImageView view = VK_NULL_HANDLE;
    if (vk.CreateImageView(S.device, &vi, nullptr, &view) != VK_SUCCESS) {
      ACO_LOG("fg: vkCreateImageView failed (format %d)", t.info.format);
      return false;
    }
    r->AddRef();
    it = S.views.emplace(r, State::View{t.image, view, 0}).first;
    ACO_LOG("fg: view for %p (VkImage %p) %ux%u format=%d layout=%d usage=0x%x flags=0x%x (%zu cached)",
            static_cast<void*>(r), static_cast<void*>(t.image), t.info.extent.width, t.info.extent.height,
            t.info.format, t.layout, t.info.usage, t.info.flags, S.views.size());
    // Diagnostics: the NVX handles NGX's kernels use to reach this view.
    auto get_handle = reinterpret_cast<PFN_vkGetImageViewHandleNVX>(vk.gdpa(S.device, "vkGetImageViewHandleNVX"));
    auto get_handle64 =
        reinterpret_cast<PFN_vkGetImageViewHandle64NVX>(vk.gdpa(S.device, "vkGetImageViewHandle64NVX"));
    auto get_addr = reinterpret_cast<PFN_vkGetImageViewAddressNVX>(vk.gdpa(S.device, "vkGetImageViewAddressNVX"));
    for (VkDescriptorType dt : {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE}) {
      VkImageViewHandleInfoNVX hi{VK_STRUCTURE_TYPE_IMAGE_VIEW_HANDLE_INFO_NVX};
      hi.imageView = view;
      hi.descriptorType = dt;
      uint32_t h32 = get_handle ? get_handle(S.device, &hi) : 0xffffffffu;
      uint64_t h64 = get_handle64 ? get_handle64(S.device, &hi) : ~0ull;
      ACO_LOG("fg:   NVX handle (%s): 32-bit 0x%x, 64-bit 0x%llx (fn %p/%p)",
              dt == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? "storage" : "sampled", h32, (unsigned long long)h64,
              reinterpret_cast<void*>(get_handle), reinterpret_cast<void*>(get_handle64));
    }
    if (get_addr) {
      VkImageViewAddressPropertiesNVX ap{VK_STRUCTURE_TYPE_IMAGE_VIEW_ADDRESS_PROPERTIES_NVX};
      VkResult ar = get_addr(S.device, view, &ap);
      ACO_LOG("fg:   NVX address: %d addr 0x%llx size %llu", ar, (unsigned long long)ap.deviceAddress,
              (unsigned long long)ap.size);
    }
  }
  it->second.last_used = S.frames;
  r->AddRef();
  S.pending_refs.push_back(r);
  *out = NVSDK_NGX_Create_ImageView_Resource_VK(it->second.view, t.image, range, t.info.format, t.info.extent.width,
                                                t.info.extent.height, rw);
  if (tex_out) *tex_out = t;
  return true;
}

// Drops cached views of resources we haven't used for a while (e.g. render targets the engine
// recreated on a level load), after our in-flight work has finished.
void prune_views() {
  if (S.frames % 600 || S.views.size() < 8) return;
  bool waited = false;
  for (auto it = S.views.begin(); it != S.views.end();) {
    if (S.frames - it->second.last_used < 300) {
      ++it;
      continue;
    }
    if (!waited) {
      vk.WaitForFences(S.device, kRing, S.fence, VK_TRUE, UINT64_MAX);
      waited = true;
    }
    vk.DestroyImageView(S.device, it->second.view, nullptr);
    it->first->Release();
    it = S.views.erase(it);
  }
}

bool ensure_textures(const D3D11_TEXTURE2D_DESC& bb) {
  if (S.real && S.tex_w == bb.Width && S.tex_h == bb.Height && S.tex_format == bb.Format) return true;
  safe_release(S.real);
  safe_release(S.interp_uav);
  safe_release(S.interp);
  safe_release(S.outreal);
  safe_release(S.scratch);
  D3D11_TEXTURE2D_DESC d{};
  d.Width = bb.Width;
  d.Height = bb.Height;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = bb.Format;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  // Debug: the generated-frame texture starts out magenta (uploaded at creation, never written by
  // D3D11 again), so a frame DLSS-G never wrote is recognisable.
  std::vector<uint32_t> magenta(size_t(bb.Width) * bb.Height, 0xffff00ffu);
  D3D11_SUBRESOURCE_DATA init{magenta.data(), bb.Width * 4, 0};
  bool fill = config().fg_debug_fill && (bb.Format == DXGI_FORMAT_R8G8B8A8_UNORM || bb.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
  if (FAILED(S.dev->CreateTexture2D(&d, nullptr, &S.real)) ||
      FAILED(S.dev->CreateTexture2D(&d, fill ? &init : nullptr, &S.interp)) ||
      FAILED(S.dev->CreateTexture2D(&d, fill ? &init : nullptr, &S.outreal)) ||
      FAILED(S.dev->CreateTexture2D(&d, nullptr, &S.scratch))) {
    ACO_LOG("fg: failed to create %ux%u format %d textures", d.Width, d.Height, d.Format);
    safe_release(S.real);
    return false;
  }
  S.dev->CreateUnorderedAccessView(S.interp, nullptr, &S.interp_uav);
  S.tex_w = bb.Width;
  S.tex_h = bb.Height;
  S.tex_format = bb.Format;
  return true;
}

bool ensure_feature(uint32_t w, uint32_t h, uint32_t rw, uint32_t rh, VkFormat fmt) {
  if (S.feature && S.feat_w == w && S.feat_h == h && S.feat_rw == rw && S.feat_rh == rh && S.feat_format == fmt)
    return true;
  if (S.feature) {
    submit([](VkCommandBuffer) { return true; }, true);  // idle our work first
    S.release(S.feature);
    S.feature = nullptr;
  }
  ngx::Params& p = S.create_params;
  p.reset();
  p.set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
  p.set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
  p.set(NVSDK_NGX_Parameter_Width, w);
  p.set(NVSDK_NGX_Parameter_Height, h);
  p.set(NVSDK_NGX_DLSSG_Parameter_Width, w);
  p.set(NVSDK_NGX_DLSSG_Parameter_Height, h);
  // MultiFrameCount is read at create time. In 310.9 the LAST index of a sequence is the real-frame
  // call and earlier ones are generated at t = index / (count + 1) (EndpointCoreInputs::
  // ComputeAndValidateTimeFactor, reverse-engineered), so count=1 would never generate.
  p.set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, static_cast<unsigned int>(config().fg_count));
  for (const auto& kv : config().fg_create_extra) p.set(kv.first.c_str(), kv.second);
  // Which numbering NGX expects for the backbuffer format on Vulkan is undocumented; [fg]
  // bb_format_dxgi=1 passes the DXGI value of the backbuffer instead of the VkFormat.
  unsigned int fmt_param = config().fg_bb_format_dxgi ? static_cast<unsigned int>(S.tex_format) : static_cast<unsigned int>(fmt);
  p.set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, fmt_param);
  p.set(NVSDK_NGX_DLSSG_Parameter_InternalWidth, rw);
  p.set(NVSDK_NGX_DLSSG_Parameter_InternalHeight, rh);
  p.set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
  NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
  submit([&](VkCommandBuffer cmd) {
    r = S.create(S.device, cmd, NVSDK_NGX_Feature_FrameGeneration, p.get(), &S.feature);
    return !NVSDK_NGX_FAILED(r);
  }, true);
  ACO_LOG("fg: CreateFeature(FrameGeneration %ux%u, render %ux%u, format param %u) -> %s", w, h, rw, rh, fmt_param,
          ngx::result_str(r));
  if (NVSDK_NGX_FAILED(r)) {
    S.feature = nullptr;
    return false;
  }
  S.feat_w = w;
  S.feat_h = h;
  S.feat_rw = rw;
  S.feat_rh = rh;
  S.feat_format = fmt;
  S.reset = true;
  return true;
}

const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// Generates the frame between the previous real frame and S.real (current) into S.interp.
// Debug ([fg] raw_output=1): an output image created directly with Vulkan (not by DXVK), cleared to a
// marker colour before each evaluation and copied to a host-visible buffer afterwards, to tell
// whether DLSS-G's kernels can write DXVK-created images at all.
struct RawOut {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkBuffer readback = VK_NULL_HANDLE;
  volatile uint32_t* map = nullptr;
  uint32_t w = 0, h = 0;
  int pending_slot = -1;
} g_raw;

uint32_t find_memory(uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp{};
  vk.GetPhysicalDeviceMemoryProperties(S.pd, &mp);
  for (uint32_t t = 0; t < mp.memoryTypeCount; ++t)
    if ((bits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) return t;
  return UINT32_MAX;
}

bool raw_out_init(uint32_t w, uint32_t h, VkFormat fmt) {
  if (g_raw.image && g_raw.w == w && g_raw.h == h) return true;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = fmt;
  ci.extent = {w, h, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vk.CreateImage(S.device, &ci, nullptr, &g_raw.image) != VK_SUCCESS) return false;
  VkMemoryRequirements req{};
  vk.GetImageMemoryRequirements(S.device, g_raw.image, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkDeviceMemory mem = VK_NULL_HANDLE;
  if (vk.AllocateMemory(S.device, &mai, nullptr, &mem) != VK_SUCCESS) return false;
  vk.BindImageMemory(S.device, g_raw.image, mem, 0);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = g_raw.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = fmt;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vk.CreateImageView(S.device, &vi, nullptr, &g_raw.view);
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = VkDeviceSize(w) * h * 4;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  vk.CreateBuffer(S.device, &bci, nullptr, &g_raw.readback);
  vk.GetBufferMemoryRequirements(S.device, g_raw.readback, &req);
  mai.allocationSize = req.size;
  mai.memoryTypeIndex =
      find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory bmem = VK_NULL_HANDLE;
  void* ptr = nullptr;
  if (vk.AllocateMemory(S.device, &mai, nullptr, &bmem) != VK_SUCCESS) return false;
  vk.BindBufferMemory(S.device, g_raw.readback, bmem, 0);
  vk.MapMemory(S.device, bmem, 0, VK_WHOLE_SIZE, 0, &ptr);
  g_raw.map = static_cast<volatile uint32_t*>(ptr);
  g_raw.w = w;
  g_raw.h = h;
  ACO_LOG("fg: raw Vulkan output image %ux%u format %d created", w, h, fmt);
  return true;
}

// Called with the previous evaluation's slot fence signalled: summarise what landed in the image.
void raw_out_report() {
  if (g_raw.pending_slot < 0 || !g_raw.map) return;
  vk.WaitForFences(S.device, 1, &S.fence[g_raw.pending_slot], VK_TRUE, UINT64_MAX);
  uint32_t n = g_raw.w * g_raw.h, marker = 0, zero = 0, other = 0;
  uint64_t sum = 0;
  for (uint32_t i = 0; i < n; i += 97) {
    uint32_t px = g_raw.map[i];
    if (px == 0xffff00ffu) ++marker;
    else if (px == 0) ++zero;
    else ++other, sum += (px & 0xff) + ((px >> 8) & 0xff) + ((px >> 16) & 0xff);
  }
  static int logged = 0;
  if (logged++ < 20 || logged % 120 == 0)
    ACO_LOG("fg: raw output after eval: marker %u, zero %u, other %u (mean rgb %.1f) of %u samples", marker, zero,
            other, other ? double(sum) / other / 3 : 0.0, (n + 96) / 97);
  g_raw.pending_slot = -1;
}

void gen_fail(const char* why) {
  static int64_t last = 0;
  if (now_us() - last > 5000000) {
    last = now_us();
    ACO_LOG("fg: generate: failed at: %s", why);
  }
}

bool generate(const FgInputs& in, ID3D11Texture2D* swap_bb) {
  NVSDK_NGX_Resource_VK bb{}, depth{}, mv{}, out{}, outreal{}, hudless{};
  VkTex bb_t;
  // [fg] input=swapchain passes the game's (rotating) swapchain image itself; =copy our copy of it.
  ID3D11Texture2D* input = config().fg_input == "copy" ? S.real : swap_bb;
  // [fg] swap_outputs=1 (debug): hand S.outreal to NGX as OutputInterpolated and S.interp as
  // OutputReal, to tell "parameter not honoured" from "texture not writable".
  bool swap = config().fg_swap_outputs;
  if (!ngx_res(input, false, &bb, &bb_t) || !ngx_res(in.depth, false, &depth) || !ngx_res(in.mv, false, &mv) ||
      !ngx_res(swap ? S.outreal : S.interp, true, &out))
    { gen_fail("ngx_res inputs"); return false; }
  if (!ensure_feature(S.tex_w, S.tex_h, in.render_w, in.render_h, bb_t.info.format)) { gen_fail("!ensure_feature(S.tex_w, S.tex_h, in.render_w, in.render_h, "); return false; }

  ngx::Params& p = S.eval_params;
  p.reset();
  p.set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, static_cast<void*>(&bb));
  p.set(NVSDK_NGX_DLSSG_Parameter_Depth, static_cast<void*>(&depth));
  p.set(NVSDK_NGX_DLSSG_Parameter_MVecs, static_cast<void*>(&mv));
  p.set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, static_cast<void*>(&out));
  // HUD-less colour: the DLSS output (the scene at display resolution before the UI composite).
  if (config().fg_hudless && in.hudless && ngx_res(in.hudless, false, &hudless)) {
    p.set(NVSDK_NGX_DLSSG_Parameter_HUDLess, static_cast<void*>(&hudless));
    p.set(NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectWidth, S.tex_w);
    p.set(NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectHeight, S.tex_h);
  }
  if ((config().fg_show == "outreal" || config().fg_show == "compare" || swap) && ngx_res(swap ? S.interp : S.outreal, true, &outreal)) {
    p.set(NVSDK_NGX_DLSSG_Parameter_OutputReal, static_cast<void*>(&outreal));
    p.set(NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectWidth, S.tex_w);
    p.set(NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectHeight, S.tex_h);
  }
  const unsigned int count = static_cast<unsigned int>(config().fg_count);
  const unsigned int show_index = static_cast<unsigned int>(config().fg_show_index);
  NVSDK_NGX_Resource_VK scratch{};
  if (!ngx_res(S.scratch, true, &scratch)) { gen_fail("!ngx_res(S.scratch, true, &scratch)"); return false; }
  for (const char* m : {NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, NVSDK_NGX_DLSSG_Parameter_ClipToCameraView,
                        NVSDK_NGX_DLSSG_Parameter_ClipToLensClip, NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip,
                        NVSDK_NGX_DLSSG_Parameter_PrevClipToClip})
    p.set(m, const_cast<void*>(static_cast<const void*>(kIdentity)));
  const Config& cfg = config();
  // Our motion vectors are in UV units (1 = full screen) = DLSS-G's normalized space.
  p.set(NVSDK_NGX_DLSSG_Parameter_MvecScaleX, cfg.fg_mv_scale);
  p.set(NVSDK_NGX_DLSSG_Parameter_MvecScaleY, cfg.fg_mv_scale);
  p.set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, in.jitter_x);
  p.set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetY, in.jitter_y);
  // A valid (if synthetic) camera frame: an all-zero basis is "not a valid frame" for DLSS-G.
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPosX, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPosY, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPosZ, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraUpX, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraUpY, 1.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraUpZ, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraRightX, 1.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraRightY, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraRightZ, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFwdX, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFwdY, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFwdZ, 1.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetX, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetY, 0.0f);
  // The value that marks an invalid motion vector: NOT 0 (that would mark every static pixel
  // invalid); Streamline uses FLT_MIN.
  p.set(NVSDK_NGX_DLSSG_Parameter_MvecInvalidValue, 1.17549435e-38f);
  p.set(NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_Scale, 1.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_NearFarPartition, 600.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_AutomodeOverrideReset, 0u);
  // Increments by one per rendered (real) frame, as the SDK asks for.
  static unsigned long long backbuffer_id = 0;
  p.set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, ++backbuffer_id);
  if (cfg.fg_refresh_hz > 0) p.set("DLSSG.RefreshRate", cfg.fg_refresh_hz);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraNear, 0.1f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFar, 100000.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFOV, 1.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, float(S.tex_w) / float(S.tex_h));
  p.set(NVSDK_NGX_DLSSG_Parameter_ColorBuffersHDR, 0u);
  p.set(NVSDK_NGX_DLSSG_Parameter_DepthInverted, 1u);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, 1u);
  p.set(NVSDK_NGX_DLSSG_Parameter_Reset, S.reset ? 1u : 0u);
  if (S.reset) ++S.perf_resets;
  p.set(NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, 0u);
  p.set(NVSDK_NGX_DLSSG_Parameter_OrthoProjection, 0u);
  p.set(NVSDK_NGX_DLSSG_Parameter_MvecDilated, 0u);
  p.set(NVSDK_NGX_DLSSG_Parameter_MenuDetectionEnabled, 0u);
  p.set(NVSDK_NGX_DLSSG_Parameter_MinRelativeLinearDepthObjectSeparation, 40.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, in.render_w);
  p.set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, in.render_h);
  p.set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectWidth, in.render_w);
  p.set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectHeight, in.render_h);
  p.set(NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectWidth, S.tex_w);
  p.set(NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectHeight, S.tex_h);
  p.set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectWidth, S.tex_w);
  p.set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectHeight, S.tex_h);
  // Experiments with undocumented knobs (strings in nvngx_dlssg.dll), [fg] extra=name:value,...
  for (const auto& kv : cfg.fg_extra) p.set(kv.first.c_str(), kv.second);

  // The slot this evaluation will use: read what DLSS-G reported there last time, then reset it.
  int slot = S.ring;
  NVSDK_NGX_Resource_VK disable{};
  if (S.disable_buf[slot]) {
    vk.WaitForFences(S.device, 1, &S.fence[slot], VK_TRUE, UINT64_MAX);
    uint8_t v = S.disable_map[slot][0];
    if (v == 1) ++S.perf_disabled;
    static int logged_v = 0;
    if (logged_v++ < 8) ACO_LOG("fg: disable-interpolation byte from previous use of slot %d: 0x%02x", slot, v);
    S.disable_map[slot][0] = 0xee;
    disable = NVSDK_NGX_Create_Buffer_Resource_VK(S.disable_buf[slot], 4, true);
    p.set(NVSDK_NGX_DLSSG_Parameter_OutputDisableInterpolation, static_cast<void*>(&disable));
  }
  NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
  int64_t t0 = now_us();
  bool use_raw = config().fg_raw_output && raw_out_init(S.tex_w, S.tex_h, bb_t.info.format);
  NVSDK_NGX_Resource_VK raw{};
  if (use_raw) {
    raw_out_report();
    raw = NVSDK_NGX_Create_ImageView_Resource_VK(g_raw.view, g_raw.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                                                 bb_t.info.format, S.tex_w, S.tex_h, true);
    g_raw.pending_slot = S.ring;
  }
  bool ok = submit([&](VkCommandBuffer cmd) {
    if (use_raw) {
      // To GENERAL, cleared to magenta (RGBA8 0xff00ff), before DLSS-G writes it.
      VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      ib.srcAccessMask = 0;
      ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      ib.image = g_raw.image;
      ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                            nullptr, 1, &ib);
      VkClearColorValue magenta{};
      magenta.float32[0] = 1.0f;
      magenta.float32[2] = 1.0f;
      magenta.float32[3] = 1.0f;
      VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vk.CmdClearColorImage(cmd, g_raw.image, VK_IMAGE_LAYOUT_GENERAL, &magenta, 1, &rr);
      full_barrier(cmd);
    }
    // One evaluation per generated frame.
    for (unsigned int i = 1; i <= count; ++i) {
      p.set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, count);
      p.set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, i);
      p.set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated,
            static_cast<void*>(i == show_index ? (use_raw ? &raw : &out) : &scratch));
      r = S.evaluate(cmd, S.feature, p.get(), nullptr);
      if (NVSDK_NGX_FAILED(r)) { static int n = 0; if (n++ < 5) ACO_LOG("fg: Evaluate(index %u of %u) -> %s (0x%08x)", i, count, ngx::result_str(r), static_cast<unsigned>(r)); return false; }
    }
    if (use_raw) {
      full_barrier(cmd);
      VkBufferImageCopy bic{};
      bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      bic.imageExtent = {g_raw.w, g_raw.h, 1};
      vk.CmdCopyImageToBuffer(cmd, g_raw.image, VK_IMAGE_LAYOUT_GENERAL, g_raw.readback, 1, &bic);
    }
    return true;
  }, false);
  S.perf_eval_cpu_ms += (now_us() - t0) / 1000.0;
  static int logged = 0;
  if (logged < 3) {
    // What NGX leaves in the parameter block after an evaluation.
    for (const char* n : {"DLSSG.MustCallEval", "DLSSG.MultiFrameCount", "DLSSG.MultiFrameCountMax",
                          "DLSSG.DynamicMaxFrameCount", "DLSSG.NvAppOvrAppliedVal.MultiFrameCount",
                          "DLSSG.NvAppOvrAppliedVal.StreamlineMode", "DLSSG.StreamlineMode",
                          "DLSSG.ReflexWarp.Available", "DLSSG.Dynamic.ReflexDriven", "DLSSG.EvalFlags"}) {
      unsigned int v = 0xdeadbeef;
      bool ok = p.get(n, &v);
      ACO_LOG("fg:   after eval %s = %s%u", n, ok ? "" : "(unset) ", v);
    }
  }
  if (logged < 3 || (NVSDK_NGX_FAILED(r) && logged < 20)) {
    ++logged;
    ACO_LOG("fg: Evaluate -> %s (reset=%d, render %ux%u, jitter %.3f,%.3f)", ngx::result_str(r), S.reset,
            in.render_w, in.render_h, in.jitter_x, in.jitter_y);
  }
  S.reset = false;
  return ok;
}

void perf_report() {
  int64_t now = now_us();
  if (!S.perf_last) S.perf_last = now;
  double secs = (now - S.perf_last) / 1e6;
  if (secs < 2.0) return;
  ACO_LOG("perf: fg real %.1f fps + generated %.1f fps = %.1f presents/s (fail %u, dlssg-disabled %u, resets %u, cu launches %u), eval CPU %.2f ms GPU %.3f ms/frame",
          S.perf_real / secs, S.perf_generated / secs, (S.perf_real + S.perf_generated) / secs, S.perf_fail, S.perf_disabled, S.perf_resets, g_cu_launches.exchange(0),
          S.perf_real ? S.perf_eval_cpu_ms / S.perf_real : 0.0, S.perf_gpu_n ? S.perf_gpu_ms / S.perf_gpu_n : 0.0);
  S.perf_gpu_ms = 0;
  S.perf_gpu_n = 0;
  S.perf_last = now;
  S.perf_real = S.perf_generated = S.perf_fail = S.perf_disabled = S.perf_resets = 0;
  S.perf_eval_cpu_ms = 0;
}

void wait_until(int64_t t_us) {
  for (;;) {
    int64_t left = t_us - now_us();
    if (left <= 0) return;
    if (left > 2000) Sleep(static_cast<DWORD>((left - 1500) / 1000));
    else YieldProcessor();
  }
}

} // namespace

void fg_preinit() {
  // The driver's Wine NGX core logs only when asked through its environment.
  if (int lvl = config().fg_ngx_log) {
    std::wstring path = out_dir();
    SetEnvironmentVariableW(L"__NGX_LOG_LEVEL", lvl >= 2 ? L"2" : L"1");
    SetEnvironmentVariableW(L"__NGX_ENABLE_OVERRIDE_LOG_PATH", L"1");
    SetEnvironmentVariableW(L"__NGX_LOG_PATH_OVERRIDE", path.c_str());
    ACO_LOG("fg: NGX logging level %d to %ls", lvl, path.c_str());
  }
  // DXVK's device must be created with VK_KHR_push_descriptor (see vk_device_ext.cpp). dxgi.dll is
  // a static import of the game, so it is already mapped; DXVK's d3d11.dll is hooked when we load it.
  if (config().fg_mode == "on") fg_hook_dxvk_device_creation(GetModuleHandleW(L"dxgi.dll"), nullptr);
}

void fg_on_present(IDXGISwapChain*) { ++S.frames; }

bool fg_present(IDXGISwapChain* sc, UINT sync, UINT flags, PFN_RealPresent real, HRESULT* hr) {
  const Config& cfg = config();
  if (cfg.fg_mode == "d3d12" || fg12_proxy_active()) return fg12_present(sc, sync, flags, real, hr, S.frames);
  if (S.frames < 300) return false;
  if (cfg.fg_mode != "on") return false;
  if (S.ok) prune_views();
  if (!S.tried) {
    S.tried = true;
    S.ok = init(sc);
    ACO_LOG("fg: %s", S.ok ? "ready" : "unavailable, presenting normally");
  }
  if (!S.ok) return false;

  FgInputs in;
  if (!upscaler_fg_inputs(&in)) {  // no DLSS this frame (menus, loading): present normally
    S.reset = true;
    static int64_t last = 0;
    if (now_us() - last > 5000000) {
      last = now_us();
      ACO_LOG("fg: no DLSS inputs this frame (menus/loading?) - presenting normally");
    }
    return false;
  }
  if (in.frame != S.last_input_frame + 1) S.reset = true;
  S.last_input_frame = in.frame;

  ID3D11Texture2D* bb = nullptr;
  bool done = false;
  if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) {
    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    if (ensure_textures(bd)) {
      S.imm->CopyResource(S.real, bb);
      if (generate(in, bb)) {
        // Generated frame first (it lies between the previous real frame and this one) ...
        if (cfg.fg_show == "compare" || cfg.fg_show == "compare_gen") {
          // Debug: left half the input frame, right half OutputReal, both from this evaluation.
          D3D11_BOX left{0, 0, 0, S.tex_w / 2, S.tex_h, 1};
          S.imm->CopySubresourceRegion(bb, 0, 0, 0, 0, S.real, 0, &left);
          S.imm->CopySubresourceRegion(bb, 0, S.tex_w / 2, 0, 0, cfg.fg_show == "compare_gen" ? S.interp : S.outreal, 0, &left);
        } else {
          S.imm->CopyResource(bb, cfg.fg_show == "real" ? S.real : cfg.fg_show == "outreal" ? S.outreal : S.interp);
        }
        *hr = real(sc, sync, flags);
        int64_t t_gen = now_us();
        ++S.perf_generated;
        // ... then the real frame, half a frame interval later.
        if (cfg.fg_pacing && S.frame_interval_us > 0)
          wait_until(t_gen + static_cast<int64_t>(S.frame_interval_us / 2));
        ID3D11Texture2D* bb2 = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb2)))) {
          if (cfg.fg_show == "compare" || cfg.fg_show == "compare_gen") {
            D3D11_BOX left{0, 0, 0, S.tex_w / 2, S.tex_h, 1};
            S.imm->CopySubresourceRegion(bb2, 0, 0, 0, 0, S.real, 0, &left);
            S.imm->CopySubresourceRegion(bb2, 0, S.tex_w / 2, 0, 0, cfg.fg_show == "compare_gen" ? S.interp : S.outreal, 0, &left);
          } else {
            S.imm->CopyResource(bb2, cfg.fg_show == "generated" ? S.interp : cfg.fg_show == "outreal" ? S.outreal : S.real);
          }
          bb2->Release();
        }
        *hr = real(sc, sync, flags);
        done = true;
      } else {
        ++S.perf_fail;
        static int64_t last_fail = 0;
        if (now_us() - last_fail > 5000000) {
          last_fail = now_us();
          ACO_LOG("fg: generate() failed (%u failures so far this window)", S.perf_fail);
        }
      }
    }
    bb->Release();
  }
  safe_release(in.mv);
  safe_release(in.depth);
  safe_release(in.hudless);
  if (!done) return false;

  int64_t now = now_us();
  if (S.last_real_present) {
    double dt = double(now - S.last_real_present);
    S.frame_interval_us = S.frame_interval_us > 0 ? S.frame_interval_us * 0.9 + dt * 0.1 : dt;
  }
  S.last_real_present = now;
  ++S.perf_real;
  perf_report();
  return true;
}

} // namespace aco
