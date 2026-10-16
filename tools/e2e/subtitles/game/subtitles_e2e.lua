-- Subtitles e2e (2026-09-27): each press of Left Ctrl (INPUT_ACTION_1,
-- switch 6) moves to the next scenario; run.sh takes a screenshot of each.
-- Keyboard rather than a virtual joystick: SDL drops some virtual button
-- changes made while the picture is paused (seen while writing this test).
OVERLAY_UPDATED = 1
SCANCODE = { LCTRL = { name = "LCTRL", value = 224 } } -- what KimmyScript's framework.kimmy defines, for controls.cfg
local phase = 0
local started = false
function onOverlayUpdate()
  if not started then
    started = true
    local names = {}
    for i = 1, dirkGetSubtitleCount() do names[#names + 1] = dirkGetSubtitleName(i) end
    print("SUBS count=" .. dirkGetSubtitleCount() .. " names=" .. table.concat(names, ",") ..
          " index=" .. dirkGetSubtitleIndex() .. " name99=[" .. dirkGetSubtitleName(99) .. "]")
    discSkipToFrame(15); discPause() -- 0.5 s: before the first cue
  end
  return OVERLAY_UPDATED
end
local steps = {
  function() discSkipToFrame(45) end,   -- 1.5 s: cue 1, one line
  function() discSkipToFrame(105) end,  -- 3.5 s: gap between cues
  function() discSkipToFrame(150) end,  -- 5.0 s: cue 2, two lines, accents
  function() discSkipToFrame(240) end,  -- 8.0 s: cue 3, far too wide
  function() dirkSetSubtitleIndex(0); discSkipToFrame(45) end,   -- turned off
  function() dirkSetSubtitleIndex(2); discSkipToFrame(150) end,  -- FR track, one line
  function() dirkSetSubtitleIndex(7); discSkipToFrame(45) end,   -- out of range -> clamped to 2 (FR), jump back
  function() dirkSetSubtitleIndex(1); dirkSetSubtitleSize(50); discSkipToFrame(45) end,  -- cue 1 at half size
  function() dirkSetSubtitleSize(300); discSkipToFrame(45) end,  -- too big -> clamped to 150
}
function onInputPressed(what)
  if what ~= 6 then return end
  phase = phase + 1
  if steps[phase] then steps[phase](); discPause() end
  print("PHASE " .. phase .. " frame=" .. discGetFrame() .. " index=" .. dirkGetSubtitleIndex() .. " size=" .. dirkGetSubtitleSize())
end
function onInputReleased(what) end
