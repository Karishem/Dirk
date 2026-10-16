-- Forced controls (2026-10-08): the script names a file of KimmyScript/Cfg/
-- in forcedControls (FC env), and its lines win over the game's controls.cfg
-- and over a joystick profile. FC unset: the game's bindings only.
OVERLAY_UPDATED = 1
SCANCODE  = { SPACE    = { name = "SPACE",    value = 44 } }
GAMEPAD_0 = { BUTTON_A = { name = "BUTTON_A", value = 518 },
              BUTTON_X = { name = "BUTTON_X", value = 520 } }
forcedControls = os.getenv("FC")
function onOverlayUpdate() return OVERLAY_UPDATED end
function onInputPressed(what) print("GAME PRESSED " .. what) end
function onInputReleased(what) end
