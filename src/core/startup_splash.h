#pragma once

#include <windef.h>

namespace aco {

// Paint the stereo pair into Origins' own startup window while the game loads.
void configure_startup_splash(HINSTANCE module);

} // namespace aco
