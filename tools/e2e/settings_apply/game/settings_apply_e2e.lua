-- Review point 6 e2e: the picture is parked on one frame so screenshots
-- compare like for like; SWITCH_BUTTON1 re-sets all six dirk settings to
-- their CURRENT values, the way service.kimmy's readConfig() does at startup.
OVERLAY_UPDATED = 1
local parked = false
function onOverlayUpdate()
  if not parked then parked = true; discSkipToFrame(30); discPause() end
  return OVERLAY_UPDATED
end
function onInputPressed(what)
  if what == 6 then
    dirkSetFullscreen(dirkGetFullscreen())
    dirkSetVolumeVldp(dirkGetVolumeVldp())
    dirkSetVolumeNonVldp(dirkGetVolumeNonVldp())
    dirkSetScanlines(dirkGetScanlines())
    dirkSetBezelIndex(dirkGetBezelIndex())
    dirkSetGenerateLog(dirkGetGenerateLog())
    print("SAME VALUES re-set")
  end
end
