#pragma once
// Best-effort, per-machine measurement of the default audio output
// endpoint's own round-trip latency -- see AudioLatency.cpp for the full
// story of why this exists (short version: a single hardcoded compensation
// constant tuned on one developer's PC cannot possibly be correct on every
// player's different sound card/driver, so Dirk asks the OS for the real
// number on each machine instead of guessing).

namespace dirk {

// Returns the default render endpoint's own reported latency, in seconds.
// On Windows this queries WASAPI directly (the one API that actually knows
// this number). On any other platform, or if the query fails for any
// reason on Windows (no default device, a COM failure, ...), returns
// `fallbackSeconds` unchanged -- always safe to call, never throws, never
// blocks longer than a normal COM device-enumeration call.
double queryAudioOutputLatencySeconds(double fallbackSeconds);

} // namespace dirk
