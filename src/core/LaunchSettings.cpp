#include "core/LaunchSettings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace dirk {

namespace {

int clampVolume(int v) { return std::max(0, std::min(100, v)); }
int clampSubtitleSize(int v) { return v <= 0 ? 100 : std::max(kSubtitleSizeMin, std::min(kSubtitleSizeMax, v)); }

} // namespace

LaunchSettings loadLaunchSettings(const std::string& path) {
    LaunchSettings s; // defaults on any failure -- see header comment
    std::ifstream in(path);
    if (!in) return s;

    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "fullscreen") {
            s.fullscreen = (val == "1");
        } else if (key == "volume_vldp") {
            s.volumeVldp = clampVolume(std::atoi(val.c_str()));
        } else if (key == "volume_nonvldp") {
            s.volumeNonVldp = clampVolume(std::atoi(val.c_str()));
        } else if (key == "scanlines") {
            s.scanlines = (val == "1");
        } else if (key == "hide_console") {
            s.hideConsole = (val == "1");
        } else if (key == "bezel_index") {
            s.bezelIndex = std::max(0, std::atoi(val.c_str()));
        } else if (key == "bezel_enabled") {
            // Old key (pre-2026-09-23, plain on/off) -- only applied if
            // this factory-default file predates bezel_index and doesn't
            // have it at all; "on" reads back as "bezel #1", matching what
            // on/off actually meant back when there was only ever one.
            if (s.bezelIndex == 0) s.bezelIndex = (val == "1") ? 1 : 0;
        } else if (key == "audio_track") {
            s.audioTrack = std::max(0, std::atoi(val.c_str()));
        } else if (key == "subtitle_size") {
            s.subtitleSize = clampSubtitleSize(std::atoi(val.c_str()));
        } else if (key == "subtitle_index") {
            s.subtitleIndex = std::max(0, std::atoi(val.c_str()));
        } else if (key == "generate_log") {
            s.generateLog = (val == "1");
        } else if (key == "swap_face_buttons") {
            s.swapFaceButtons = (val == "1");
        } else if (key == "error_banner") {
            s.errorBanner = (val != "0"); // anything but an explicit 0 keeps the default (on)
        } else if (key == "decode_threads") {
            if (val == "auto") {
                s.decodeThreads = 0;
            } else {
                const int n = std::atoi(val.c_str());
                s.decodeThreads = n >= 1 ? std::min(n, 8) : 0; // garbage/0/negative -> the default (auto)
            }
        }
    }
    return s;
}

// No longer called from the live main loop (2026-09-22, Cyril) -- a running
// game's F10/F11/F12/menu changes now go straight to that game's own
// game.cfg instead (see main.cpp's persistPerGameDirkSettings()), so this
// factory-default file stops drifting session to session. Kept as the
// write-side counterpart to loadLaunchSettings() for anyone regenerating
// dirk_settings.cfg by hand/tooling rather than editing its "key=value"
// text directly.
void saveLaunchSettings(const std::string& path, const LaunchSettings& settings) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return; // best-effort, see header comment
    out << "fullscreen=" << (settings.fullscreen ? 1 : 0) << "\n";
    out << "volume_vldp=" << clampVolume(settings.volumeVldp) << "\n";
    out << "volume_nonvldp=" << clampVolume(settings.volumeNonVldp) << "\n";
    out << "scanlines=" << (settings.scanlines ? 1 : 0) << "\n";
    out << "hide_console=" << (settings.hideConsole ? 1 : 0) << "\n";
    out << "bezel_index=" << std::max(0, settings.bezelIndex) << "\n";
    out << "subtitle_index=" << std::max(0, settings.subtitleIndex) << "\n";
    out << "subtitle_size=" << clampSubtitleSize(settings.subtitleSize) << "\n";
    out << "audio_track=" << std::max(0, settings.audioTrack) << "\n";
    out << "generate_log=" << (settings.generateLog ? 1 : 0) << "\n";
    out << "error_banner=" << (settings.errorBanner ? 1 : 0) << "\n";
    if (settings.swapFaceButtons) out << "swap_face_buttons=1\n";
    if (settings.decodeThreads == 0) {
        out << "decode_threads=auto\n";
    } else {
        out << "decode_threads=" << std::max(1, settings.decodeThreads) << "\n";
    }
}

void nudgeVolumes(LaunchSettings& settings, int deltaPercent) {
    settings.volumeVldp = clampVolume(settings.volumeVldp + deltaPercent);
    settings.volumeNonVldp = clampVolume(settings.volumeNonVldp + deltaPercent);
}

void setLaunchSettingLine(const std::string& path, const std::string& key, const std::string& value) {
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (in && std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    bool found = false;
    for (auto& l : lines) {
        size_t eq = l.find('=');
        if (eq != std::string::npos && l.substr(0, eq) == key) {
            l = key + "=" + value;
            found = true;
        }
    }
    if (!found) lines.push_back(key + "=" + value);
    std::ofstream out(path, std::ios::trunc);
    if (!out) return; // best-effort, see header comment
    for (const auto& l : lines) out << l << "\n";
}

} // namespace dirk
