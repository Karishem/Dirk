-- Review point 4 e2e: frames printed every overlay tick; SWITCH_BUTTON1
-- simulates a game parking on a menu (seek + pause) while the native
-- Pause freeze is active.
OVERLAY_UPDATED = 1
GAMEPAD_0 = { AXIS_LEFT_Y_D = { name = "AXIS_LEFT_Y_D", value = 505 },
              BUTTON_START  = { name = "BUTTON_START",  value = 524 } }
local tick = 0
function onOverlayUpdate()
  tick = tick + 1
  local f = discGetFrame()
  print(string.format("F %d %d", tick, f))
  -- Scenario E: once armed, a picture frozen for 30 ticks (the F9 wizard)
  -- makes the script park on a menu by itself, like a game's attract/
  -- timeout logic would while the wizard is open.
  if armed then
    if f == lastF then still = still + 1 else still = 0 end
    lastF = f
    if still == 30 then armed = false; discSkipToFrame(20); discPause(); print("WIZ parked at 20") end
  end
  return OVERLAY_UPDATED
end
function onInputPressed(what)
  print("PRESSED " .. what)
  if what == 7 then discPlay(); armed = true; still = 0; print("RESUMED by script, armed") end
  if what == 6 then discSkipToFrame(10); discPause(); print("MENU parked at 10") end
end
function onInputReleased(what) print("RELEASED " .. what) end
