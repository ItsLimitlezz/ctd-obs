/******************************************************************************
    CTD OBS: the app's side of NDI (docs/ctd/ndi.md). The NDI module
    (plugins/ctd-ndi) does the NDI work; the app talks to it through its
    procs and owns the settings, the menu, the launch flag and the memory
    watchdog.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <string>

/* user.ini keys, shared with plugins/ctd-ndi/src/ndi-plugin.cpp */
inline constexpr const char *kCtdNdiSection = "CTD";
inline constexpr const char *kCtdNdiKeyMode = "NDIMode";
inline constexpr const char *kCtdNdiKeySendProgram = "NDISendProgram";
inline constexpr const char *kCtdNdiKeySendVertical = "NDISendVertical";
inline constexpr const char *kCtdNdiKeyWatchdogLimit = "WatchdogLimitMB";

/* What proc ctd_ndi_status reports; available is false when the NDI module isn't loaded */
struct CtdNdiStatus {
	bool available = false;
	std::string mode = "off";
	std::string configured = "off";
	bool paused = false;
	bool launchDisabled = false;
	bool loaded = false;
	std::string version;
	std::string installed;
	std::string message;
	int senders = 0; /* running: program, vertical and per source */
	int maxSenders = 0;
	long long dropped = 0;
	bool program = false;     /* "CTD OBS Program" is sending */
	bool vertical = false;    /* "CTD OBS Vertical" is sending */
	std::string systemPieces; /* newline-separated */
};

CtdNdiStatus CtdNdiQueryStatus();
/* ctd_ndi_apply: the module re-reads user.ini and applies it now */
void CtdNdiApply();
/* ctd_ndi_setup; false with a message when nothing approved was found */
bool CtdNdiSetup(std::string &message);
/* ctd_ndi_pause */
void CtdNdiSetPaused(bool paused);

/* Writes the user.ini defaults (NDI off, program send on, vertical off, watchdog limit) */
void CtdNdiSetConfigDefaults();
