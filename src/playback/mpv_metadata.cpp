// MPV playback metadata/diagnostics layer — extracted implementation unit (D19 god-object split).
//   Static helpers: human-readable end-file reason / mpv error-code strings (Y24.8) and the
//   CLI --vo/--vid/--ao override setters. They remain MPVController static members; only
//   their implementations live here. Declarations stay in mpv_controller.h.
//   Mechanical verbatim move from mpv_controller.cpp (lines 36-109).
#include "panicast/playback/mpv_controller.h"

#include <string>

#include <fmt/format.h>

#include "panicast/core/logger.h"

namespace panicast
{

// Y24.8: human-readable mpv end-file reason. mpv's mpv_end_file_reason enum:
//   0=EOF, 2=stop, 3=quit, 4=error, 5=redirect.
const char *MPVController::end_file_reason_str(int reason) {
    switch (reason) {
    case 0:
        return "track ended (EOF)";
    case 2:
        return "stopped (user/script)";
    case 3:
        return "mpv quitting";
    case 4:
        return "playback error";
    case 5:
        return "redirected";
    default:
        return "unknown reason";
    }
}

// Y24.8: human-readable mpv error code (mpv_error enum).
std::string MPVController::mpv_error_str(int error) {
    switch (error) {
    case 0:
        return "no error";
    case -1:
        return "event queue full";
    case -2:
        return "out of memory";
    case -3:
        return "mpv uninitialized";
    case -4:
        return "invalid parameter";
    case -5:
        return "option not found";
    case -6:
        return "option format error";
    case -7:
        return "option error";
    case -8:
        return "property not found";
    case -9:
        return "property format error";
    case -10:
        return "property unavailable";
    case -11:
        return "property error";
    case -12:
        return "command error";
    case -13:
        return "loading failed (file/stream could not be loaded — check path/URL/network/proxy)";
    case -14:
        return "audio output init failed (AO=null — check [mpv] ao / PulseAudio / WSLg; playback "
               "cannot produce sound)";
    case -15:
        return "video output init failed (VO init — try vo=null or fix GPU/Display)";
    case -16:
        return "nothing to play (empty/no playable tracks)";
    case -17:
        return "unknown format (decoder missing — install ffmpeg)";
    case -18:
        return "unsupported";
    case -19:
        return "not implemented";
    default:
        return fmt::format("error code {}", error);
    }
}

void MPVController::set_cli_overrides(const std::string &vo, const std::string &vid,
                                      const std::string &ao) {
    cli_vo_override_ = vo;
    cli_vid_override_ = vid;
    cli_ao_override_ = ao;
    LOG(fmt::format("[MPV] CLI overrides: vo={}, vid={}, ao={}", vo.empty() ? "(default)" : vo,
                    vid.empty() ? "(default)" : vid, ao.empty() ? "(default)" : ao));
}
} // namespace panicast
