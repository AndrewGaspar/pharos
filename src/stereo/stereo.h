#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace aco {

// Stereoscopic 3D ([stereo] mode = sbs): both eyes from the same game frame.
//
// The game's frame is submitted once as usual (left eye) while the immediate-context recorder
// (stereo/record.h) keeps every call; at Present the left image is copied out, the frame is
// submitted again (right eye), and the two are composed side by side. See docs/STEREO.md.
void stereo_init(ID3D11Device* dev, ID3D11DeviceContext* imm);
void stereo_on_present(IDXGISwapChain* sc);  // before anything else touches the back buffer
bool stereo_active();                        // side by side is running (Frame Generation stays off)
int stereo_eye();                            // 0 while the game's own submission runs, 1 during the replay

} // namespace aco
