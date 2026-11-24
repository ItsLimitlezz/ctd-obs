/******************************************************************************
    CTD OBS: third-party plugins installed for OBS Studio (docs/ctd/plugins.md).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ctd-plugin-isolation-logic.hpp"

#include <algorithm>
#include <system_error>

namespace fs = std::filesystem;

namespace {

std::string PathToUtf8(const fs::path &path)
{
#if defined(__cpp_char8_t)
	const std::u8string text = path.u8string();
	return std::string(text.begin(), text.end());
#else
	return path.u8string();
#endif
}

std::string Lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(),
		       [](unsigned char c) { return static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c); });
	return text;
}

bool IsDirectory(const fs::path &path)
{
	std::error_code ec;
	return fs::is_directory(path, ec);
}

bool Exists(const fs::path &path)
{
	std::error_code ec;
	/* symlink_status: a dangling link at the destination still counts as taken */
	return fs::exists(fs::symlink_status(path, ec));
}

/* Files that live in OBS Studio's obs-plugins/64bit folder but are not plugins (obs-browser's CEF runtime, graphics
 * runtimes). They have no data/obs-plugins folder, so they are skipped anyway; this keeps the list obvious. */
const char *const kNotPlugins[] = {"chrome_elf", "d3dcompiler_47", "libcef",   "libegl",
				   "libglesv2",  "vk_swiftshader", "vulkan-1", "obs-virtualsource"};

bool IsNotPlugin(const std::string &lowerName)
{
	for (const char *name : kNotPlugins) {
		if (lowerName == name) {
			return true;
		}
	}
	return false;
}

void ScanRoot(const CtdStockPluginRoot &root, const std::function<bool(const std::string &)> &isBundled,
	      std::vector<CtdStockPlugin> &found)
{
	if (!IsDirectory(root.stockDir)) {
		return;
	}

	std::error_code ec;
	fs::directory_iterator it(root.stockDir, fs::directory_options::skip_permission_denied, ec);
	if (ec) {
		return;
	}

	for (const fs::directory_iterator end; it != end; it.increment(ec)) {
		if (ec) {
			break;
		}

		const fs::path entry = it->path();
		const std::string fileName = PathToUtf8(entry.filename());
		if (fileName.empty() || fileName[0] == '.') {
			continue;
		}

		CtdStockPlugin plugin;
		plugin.layout = root.layout;
		plugin.source = entry;

		switch (root.layout) {
		case CtdPluginLayout::Bundle: {
			if (Lower(PathToUtf8(entry.extension())) != ".plugin" || !IsDirectory(entry)) {
				continue;
			}
			plugin.name = PathToUtf8(entry.stem());
			plugin.destination = root.ctdDir / entry.filename();
			break;
		}
		case CtdPluginLayout::Folder: {
			if (!IsDirectory(entry)) {
				continue;
			}
			plugin.name = fileName;
			plugin.destination = root.ctdDir / entry.filename();
			break;
		}
		case CtdPluginLayout::LegacyDll: {
			std::error_code fileEc;
			if (Lower(PathToUtf8(entry.extension())) != ".dll" || !fs::is_regular_file(entry, fileEc)) {
				continue;
			}
			plugin.name = PathToUtf8(entry.stem());
			if (IsNotPlugin(Lower(plugin.name))) {
				continue;
			}
			/* A third-party plugin installed into OBS Studio's own folder has its locale files there too;
			 * DLLs without a data folder are OBS's runtime libraries, not plugins */
			const fs::path data = root.stockDataDir / entry.stem();
			if (root.stockDataDir.empty() || !IsDirectory(data)) {
				continue;
			}
			plugin.dataSource = data;
			plugin.destination = root.ctdDir / entry.stem();
			break;
		}
		}

		if (plugin.name.empty() || (isBundled && isBundled(plugin.name))) {
			continue;
		}

		plugin.alreadyInCtd = Exists(plugin.destination);
		found.push_back(std::move(plugin));
	}
}

bool CopyTree(const fs::path &from, const fs::path &to, std::error_code &ec)
{
	if (IsDirectory(from)) {
		fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
	} else {
		fs::copy_file(from, to, fs::copy_options::none, ec);
	}
	return !ec;
}

void RemoveQuietly(const fs::path &path)
{
	std::error_code ec;
	fs::remove_all(path, ec);
}

} // namespace

std::vector<CtdStockPlugin> CtdFindStockPlugins(const std::vector<CtdStockPluginRoot> &roots,
						const std::function<bool(const std::string &)> &isBundled)
{
	std::vector<CtdStockPlugin> found;
	for (const CtdStockPluginRoot &root : roots) {
		ScanRoot(root, isBundled, found);
	}

	std::stable_sort(found.begin(), found.end(), [](const CtdStockPlugin &a, const CtdStockPlugin &b) {
		const std::string la = Lower(a.name);
		const std::string lb = Lower(b.name);
		if (la != lb) {
			return la < lb;
		}
		return a.source < b.source;
	});
	return found;
}

bool CtdCopyStockPlugin(const CtdStockPlugin &plugin, std::string &error)
{
	error.clear();

	if (plugin.destination.empty() || plugin.source.empty()) {
		error = "nothing to copy";
		return false;
	}
	if (Exists(plugin.destination)) {
		error = "already in CTD OBS's plugin folder";
		return false;
	}
	if (!Exists(plugin.source)) {
		error = "it is no longer in OBS Studio's plugin folder";
		return false;
	}

	std::error_code ec;
	const fs::path parent = plugin.destination.parent_path();
	fs::create_directories(parent, ec);
	if (ec) {
		error = "can't create " + PathToUtf8(parent) + ": " + ec.message();
		return false;
	}

	fs::path staging = plugin.destination;
	staging += ".ctd-copy";
	RemoveQuietly(staging);

	bool copied = false;
	if (plugin.layout == CtdPluginLayout::LegacyDll) {
		const fs::path bin = staging / "bin" / "64bit";
		fs::create_directories(bin, ec);
		if (!ec) {
			copied = CopyTree(plugin.source, bin / plugin.source.filename(), ec);
		}
		if (copied && !plugin.dataSource.empty()) {
			copied = CopyTree(plugin.dataSource, staging / "data", ec);
		}
	} else {
		copied = CopyTree(plugin.source, staging, ec);
	}

	if (!copied) {
		error = "copy failed: " + ec.message();
		RemoveQuietly(staging);
		return false;
	}

	fs::rename(staging, plugin.destination, ec);
	if (ec) {
		error = "can't move the copy into place: " + ec.message();
		RemoveQuietly(staging);
		return false;
	}
	return true;
}
