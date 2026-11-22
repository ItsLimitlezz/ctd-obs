/******************************************************************************
    CTD OBS NDI module: NDI source discovery (docs/ctd/ndi.md).

    One background thread runs NDIlib_find_wait_for_sources and keeps the
    list of names, so the NDI Source's properties never wait on the
    network. It starts the first time the list is asked for while NDI runs,
    and stops when NDI goes off or the module unloads.

    Copyright (C) 2026 CineTech Digital

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ndi-module.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace ctd_ndi {

static std::mutex finderMutex; /* guards the thread object */
static std::thread finderThread;
static std::atomic<bool> finderStop{false};
static std::atomic<bool> finderDone{false}; /* the thread ended by itself (NDI went off or paused) */

static std::mutex namesMutex;
static std::vector<std::string> names;

static void FinderLoop()
{
	const NDIlib_v6 *lib = Lib();
	if (!lib) {
		return;
	}
	NDIlib_find_create_t config = {};
	config.show_local_sources = true;
	config.p_groups = nullptr;
	config.p_extra_ips = nullptr;
	NDIlib_find_instance_t finder = lib->find_create_v2(&config);
	if (!finder) {
		CTD_NDI_LOG(LOG_WARNING, "NDI source discovery could not start");
		return;
	}
	CTD_NDI_LOG(LOG_INFO, "NDI source discovery started");

	while (!finderStop.load() && Running()) {
		lib->find_wait_for_sources(finder, 200);
		uint32_t count = 0;
		const NDIlib_source_t *sources = lib->find_get_current_sources(finder, &count);
		std::vector<std::string> found;
		for (uint32_t i = 0; sources && i < count; i++) {
			if (sources[i].p_ndi_name) {
				found.emplace_back(sources[i].p_ndi_name);
			}
		}
		std::sort(found.begin(), found.end());
		std::lock_guard<std::mutex> lock(namesMutex);
		names.swap(found);
	}

	lib->find_destroy(finder);
	CTD_NDI_LOG(LOG_INFO, "NDI source discovery stopped");
}

static void FinderMain()
{
	FinderLoop();
	finderDone = true;
}

void FinderEnsureStarted()
{
	if (!Running()) {
		return;
	}
	std::lock_guard<std::mutex> lock(finderMutex);
	if (finderThread.joinable()) {
		if (!finderDone.load()) {
			return; /* running */
		}
		finderThread.join();
	}
	finderStop = false;
	finderDone = false;
	try {
		finderThread = std::thread(FinderMain);
	} catch (const std::exception &e) {
		CTD_NDI_LOG(LOG_WARNING, "NDI source discovery thread failed: %s", e.what());
	}
}

void FinderStop()
{
	std::lock_guard<std::mutex> lock(finderMutex);
	finderStop = true;
	if (finderThread.joinable()) {
		finderThread.join();
	}
	std::lock_guard<std::mutex> namesLock(namesMutex);
	names.clear();
}

std::vector<std::string> FinderSources()
{
	std::lock_guard<std::mutex> lock(namesMutex);
	return names;
}

} // namespace ctd_ndi
