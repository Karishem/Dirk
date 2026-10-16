-- Script error with a file left open, every frame (2026-10-03, R36S):
-- KimmyScript's readSave() does h = io.input(s1.cfg); s = io.read();
-- string.find(s, ...) -- an empty s1.cfg makes s nil, the error skips
-- io.close(h). run.sh counts Dirk's open files while this repeats.
OVERLAY_UPDATED = 1
SCANCODE = { LCTRL = { name = "LCTRL", value = 224 } }
local frames = 0
function onOverlayUpdate()
  frames = frames + 1
  if frames % 60 == 0 then print("FRAMES " .. frames) end
  local h = io.input(DIR_FILES .. "/Cfg/s1.cfg")
  local s = io.read("*line")
  local cut = string.find(s, ",")   -- s is nil: error, h stays open
  io.close(h)
  return OVERLAY_UPDATED
end
function onInputPressed(what) end
function onInputReleased(what) end
