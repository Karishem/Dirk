#pragma once
// Registers Dirk's native Lua API: the 38 functions KimmyScript actually
// calls (feasibility study section 2), reimplemented from observed call
// sites and the official manual -- never from Singe's source, which we
// don't have and aren't seeking out.
//
// Naming: functions are registered under their native dirkXxx names
// (dirkQuit, dirkGetScriptPath, dirkGetDataPath) for the 3 that historically
// carried "Singe" in their name. Immediately after registration, this also
// injects singeQuit = dirkQuit (and the other two) as plain Lua global
// aliases -- exactly the mechanism framework.singe already uses for the
// Daphne -> Singe transition (daphneGetHeight = singeGetHeight), just
// applied by the engine at load time instead of hand-edited into
// framework.singe. Every other native function keeps its existing Singe
// name unchanged, since KimmyScript never asked to rename those.

struct lua_State;

namespace dirk {

struct EngineContext;

void registerSingeApi(lua_State* L, EngineContext* ctx);

} // namespace dirk
