-- Bezel e2e (2026-10-02, see run.sh): asks for bezel #1 like a saved
-- game.cfg would, then reports what the script sees before and after F11.
OVERLAY_UPDATED = 1
SCANCODE = { LCTRL = { name = "LCTRL", value = 224 } }
local started = false
function onOverlayUpdate()
  if not started then
    started = true
    dirkSetBezelIndex(1)
    print("BEZEL start count=" .. dirkGetBezelCount() .. " index=" .. dirkGetBezelIndex())
    discPause()
  end
  return OVERLAY_UPDATED
end
function onInputPressed(what)
  if what == 6 then
    print("BEZEL after count=" .. dirkGetBezelCount() .. " index=" .. dirkGetBezelIndex())
  end
end
function onInputReleased(what) end
