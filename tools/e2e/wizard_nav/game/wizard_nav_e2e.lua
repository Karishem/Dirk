-- Joystick wizard without a keyboard (2026-10-03): action 2 stands for the
-- "Controller Setup" item of the Dirk Options menu and opens the wizard
-- through dirkOpenJoystickWizard(); run.sh then drives it with the
-- gamepad only, through the game's controls.cfg.
OVERLAY_UPDATED = 1
GAMEPAD_0 = { AXIS_LEFT_Y_U = { name = "AXIS_LEFT_Y_U", value = 504 },
              AXIS_LEFT_Y_D = { name = "AXIS_LEFT_Y_D", value = 505 },
              BUTTON_A      = { name = "BUTTON_A",      value = 518 },
              BUTTON_B      = { name = "BUTTON_B",      value = 519 },
              BUTTON_START  = { name = "BUTTON_START",  value = 524 } }
local tick = 0
function onOverlayUpdate()
  tick = tick + 1
  if tick % 30 == 0 then print("FRAME " .. discGetFrame()) end
  return OVERLAY_UPDATED
end
function onInputPressed(what)
  print("GAME PRESSED " .. what)
  if what == 7 then print("OPEN REQUEST"); dirkOpenJoystickWizard() end
end
function onInputReleased(what) end
