#pragma once

#include <windef.h>

namespace aco {

// Paint this game's embedded pair into its existing layered startup window while it loads.
// In automatic mode the worker confirms the splash's own display supports stereo first.
void start_stereo_splash(HINSTANCE module, bool forced, bool swapped);

} // namespace aco
