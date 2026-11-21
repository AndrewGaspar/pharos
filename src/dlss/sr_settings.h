#pragma once
#include "dlss/ngx.h"

namespace aco::ngx {
template <class String>
unsigned preset_value(const String& p) {
  if (p.size() == 1 && p[0] >= 'A' && p[0] <= 'Z') return static_cast<unsigned>(p[0] - 'A' + 1);
  return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
}

inline int perf_quality_for(float ratio) {
  if (ratio >= 0.99f) return NVSDK_NGX_PerfQuality_Value_DLAA;
  if (ratio >= 0.66f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
  if (ratio >= 0.57f) return NVSDK_NGX_PerfQuality_Value_Balanced;
  if (ratio >= 0.49f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
  return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

inline void set_sr_preset(Params& p, unsigned preset) {
  for (const char* key :
       {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
    p.set(key, preset);
}
}  // namespace aco::ngx
