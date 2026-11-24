// `keepterm --probe-renderer <kind> [--allow-software-adapter] [--offscreen] [--show-ms N]`
//. --show-ms shows the probe pattern in a visible window for N ms (manual
// checks of the present path on a test machine; never used by auto-detection).
//
// The probe child: creates the backend, attaches it to a hidden window of the platform layer
// (the present path must work, not only the device; skipped with --offscreen or without a
// display), renders the probe pattern offscreen, compares it with the software backend and
// prints one JSON object (a results[] entry of render-probe.json) on stdout.
// Exit code: 0 = ok, 1 = failed, 2 = unavailable. Writes no state.
//
// `--probe-renderer auto` (diagnostics) runs the auto-detection instead: the probe cache, child
// probes on a miss, and prints the resulting backend order as JSON.
#pragma once

namespace kt::render {

// argv as given to main; looks for --probe-renderer and the flags above.
int run_probe_command(int argc, char** argv);

}  // namespace kt::render
