#pragma once
// Ported from torchbearer (src/core/hyprland.h, same author).
#include <windef.h>

#include <string>

namespace aco {

// Hyprland / HypXRland IPC from inside Wine: Wine's winsock bridges AF_UNIX sockets, so the
// mod talks to the compositor's request socket ($XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/
// .socket.sock, reached as Z:\...) like hyprctl does. Inert when not running under Hyprland.

// True when the environment names a Hyprland instance.
bool hypr_available();
// One request ("j/clients", "dispatch tagwindow +x address:0x…"); the reply, or "" on failure.
std::string hypr_request(const std::string& req);

// Keep the Hyprland window tag `tag` on this process's top-level game window (empty = none),
// removing the previously applied one. Applied from a background thread once the window is mapped
// (the compositor learns it a moment after CreateWindow); safe to call at any time.
void hypr_set_window_tag(const std::string& tag);

// Start the background poll (window tag, display state) if running under Hyprland. Idempotent.
void hypr_start();
// Stereo on the mapped compositor client verified for this process. A missing client
// is mono (0); unavailable IPC is unknown (-1). The startup probe uses the swapchain
// HWND when supplied so auto can choose stereo before FG allocates a back buffer.
int hypr_display_stereo();
int hypr_display_stereo_now(HWND hwnd = nullptr);

// An environment variable from Windows or, for variables Wine omits, its Unix environment.
std::string process_env(const char* name);

}  // namespace aco
