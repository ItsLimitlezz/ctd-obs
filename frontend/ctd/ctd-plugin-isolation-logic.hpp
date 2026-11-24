/******************************************************************************
    CTD OBS: third-party plugins installed for OBS Studio (docs/ctd/plugins.md).

    CTD OBS loads third-party plugins only from its own folder (ctd-obs/plugins),
    never from OBS Studio's (obs-studio/plugins). This finds the plugins that
    are installed for OBS Studio, so CTD OBS can list them once, and copies one
    into CTD OBS's folder when the user asks. OBS Studio's files are only read,
    never moved, changed or deleted.

    No Qt or OBS dependency; test/ctd-plugins unit-tests it.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

enum class CtdPluginLayout {
	/* macOS: <dir>/<name>.plugin (a bundle) */
	Bundle,
	/* <dir>/<name>/ (Windows ProgramData, Linux XDG folders: new and legacy layouts alike) */
	Folder,
	/* Windows, OBS Studio's install folder: <dir>/<name>.dll with its data in <data dir>/<name>; copied into CTD OBS's
	 * folder in the legacy layout <ctd dir>/<name>/bin/64bit/<name>.dll + <ctd dir>/<name>/data */
	LegacyDll,
};

/* One folder OBS Studio loads plugins from, and the CTD OBS folder its plugins are copied to */
struct CtdStockPluginRoot {
	std::filesystem::path stockDir;
	std::filesystem::path ctdDir;
	CtdPluginLayout layout = CtdPluginLayout::Folder;
	/* LegacyDll only: OBS Studio's data/obs-plugins folder */
	std::filesystem::path stockDataDir;
};

struct CtdStockPlugin {
	/* module name, e.g. "distroav" */
	std::string name;
	CtdPluginLayout layout = CtdPluginLayout::Folder;
	/* the bundle, folder or .dll in OBS Studio's folder */
	std::filesystem::path source;
	/* LegacyDll only: its data folder in OBS Studio's install (empty when it has none) */
	std::filesystem::path dataSource;
	/* where "Copy into CTD OBS" puts it */
	std::filesystem::path destination;
	/* something with that name is already in CTD OBS's folder */
	bool alreadyInCtd = false;
};

/* Plugins OBS Studio has in the given folders, sorted by name (then folder). Missing or unreadable folders are skipped.
 * `isBundled` returns true for a module name that CTD OBS ships itself (OBS Studio's own modules in its install folder,
 * which CTD OBS has built in); those are never listed. Hidden entries are skipped. */
std::vector<CtdStockPlugin> CtdFindStockPlugins(const std::vector<CtdStockPluginRoot> &roots,
						const std::function<bool(const std::string &)> &isBundled);

/* Copies the plugin into CTD OBS's folder (never over something already there). It is copied beside its destination
 * first and renamed into place, so a failed copy leaves nothing half-copied. The source is only read. Returns false
 * with a short English reason on failure. */
bool CtdCopyStockPlugin(const CtdStockPlugin &plugin, std::string &error);
