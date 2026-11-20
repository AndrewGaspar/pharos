#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aco {

// Quiver lab channel (design/LAB.md in the Quiver repo, "The lab channel"). Inert outside the lab
// (no QUIVER_LAB in the environment).
//  - lab_state("in-world", {"k=v"}): %QUIVER_LAB%\state, replaced atomically; `quiver lab wait
//    <i> in-world` waits on it.
//  - lab_verb("where", fn): answers `quiver lab call <i> where ...`. fn returns a JSON value for
//    "result" (an empty string means null); set ok = false to answer an error (the returned
//    string is then the error's JSON string). Each request is served on its own thread, so a long
//    one (teleport) doesn't hold up the others.
void lab_init();
bool lab_active();
void lab_state(const std::string& state, const std::vector<std::string>& kv = {});

using LabVerb = std::function<std::string(const std::vector<std::string>& args, bool& ok)>;
void lab_verb(const std::string& verb, LabVerb fn);

// Runs fn on the render thread at the next Present (lab_on_present) and waits for it; false if no
// Present came within timeout_ms (game not rendering, or held by `quiver lab clock pause`).
bool on_present(const std::function<void()>& fn, unsigned timeout_ms = 10000);
// Presents seen so far, and a wait for n more (false on timeout).
uint64_t present_count();
bool wait_presents(uint64_t n, unsigned timeout_ms);
// From the DXGI Present hook.
void lab_on_present();

// JSON string literal for a value (quotes and escapes).
std::string json_str(const std::string& s);
std::string lab_error(bool& ok, const std::string& msg);

} // namespace aco
