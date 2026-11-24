/******************************************************************************
    CTD OBS NDI module: state shared by the module's parts (docs/ctd/ndi.md).

    Copyright (C) 2026 CineTech Digital

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "ndi-logic.hpp"

#include <obs-module.h>

#include <Processing.NDI.Lib.h>

#include <cstdint>
#include <string>
#include <vector>

#define CTD_NDI_LOG(level, format, ...) blog(level, "[ctd-ndi] " format, ##__VA_ARGS__)

namespace ctd_ndi {

/* ---------------------------------------------------------------- state (ndi-plugin.cpp) */

/* The NDI library's function table once loaded (never unloaded until CTD OBS quits), else nullptr */
const NDIlib_v6 *Lib();

/* The mode this launch runs with right now (Off while launch-disabled) */
Mode CurrentMode();
bool Paused();
/* NDI may send: the library is loaded, the mode sends and NDI isn't paused */
bool Running();
/* Changes whenever Running() may have changed; senders re-check when it does */
uint64_t RunGeneration();

/* The cap on running senders: program, vertical and per-source together */
SenderSlots &Slots();

/* ---------------------------------------------------------------- loader (ndi-loader.cpp) */

struct LoadState {
	bool loaded = false;
	std::string version; /* allow-listed version that is loaded */
	std::string path;    /* the file loaded */
	std::string message; /* for Settings and the log */
};

/* Loads CTD OBS's own verified copy (or, CTD_NDI_BUNDLED, the bundle's). Idempotent; never loads anything else. */
LoadState LoadNdiLibrary();
LoadState CurrentLoadState();

/* Hashes the official NDI Runtime's library where it installs it (never loads it) and, if it's on the allow-list,
 * copies it with NDI's licence into CTD OBS's own folder. Returns a message for Settings. */
bool SetupFromRuntime(std::string &message);

/* What's set up in CTD OBS's own folder ("6.4.0.2", or empty) */
std::string InstalledVersion();

/* NDI Tools pieces installed system-wide (the 2026-11-22 incident), for a warning in Settings */
std::vector<std::string> SystemPiecesFound();

/* ---------------------------------------------------------------- registration */

void RegisterOutputs(); /* ndi-output.cpp */
void RegisterFilter();  /* ndi-filter.cpp */

/* ---------------------------------------------------------------- per-source senders (ndi-filter.cpp) */

/* Every NDI Output filter re-checks whether it should run (mode, pause, settings changes). UI thread. */
void FiltersReconcileAll();
/* Stops every per-source sender now (exit, profile / collection switch). UI thread. */
void FiltersStopAll();
/* The state line of one NDI Output filter ("Sending", "Waiting: …"), or empty if it isn't one */
std::string FilterState(obs_source_t *filter);

/* Prevents a profile / collection switch from racing a sender start (ndi-plugin.cpp) */
bool Switching();

} // namespace ctd_ndi
