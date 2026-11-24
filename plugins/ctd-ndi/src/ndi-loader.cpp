/******************************************************************************
    CTD OBS NDI module: setting up and loading the NDI library
    (docs/ctd/ndi.md, "Where the library comes from").

      Set up   the official NDI 6 Runtime's library is hashed where it is
               installed (never loaded there); if its SHA-256 is on the
               allow-list it is copied with NDI's licence into
               <module config>/runtime/<version>/
      Load     only that copy (or, CTD_NDI_BUNDLED, the app bundle's),
               hashed again first, by full path, after LoadPathAllowed

    Nothing here searches a path, reads PATH or loads from a system folder.

    Copyright (C) 2026 CineTech Digital

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ndi-module.hpp"

#include <util/bmem.h>
#include <util/platform.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace ctd_ndi {

static std::mutex loadMutex;
static LoadState loadState;
/* set once, never cleared: the library stays mapped until CTD OBS quits */
static std::atomic<const NDIlib_v6 *> loadedLib{nullptr};

const NDIlib_v6 *Lib()
{
	return loadedLib.load(std::memory_order_acquire);
}

static fs::path U8Path(const std::string &utf8)
{
#if defined(__cpp_char8_t)
	return fs::path(reinterpret_cast<const char8_t *>(utf8.c_str()));
#else
	return fs::u8path(utf8);
#endif
}

static std::string PathU8(const fs::path &path)
{
#if defined(__cpp_char8_t)
	const std::u8string s = path.u8string();
	return std::string(s.begin(), s.end());
#else
	return path.u8string();
#endif
}

static std::string Env(const char *name)
{
#ifdef _WIN32
	wchar_t *wname = nullptr;
	os_utf8_to_wcs_ptr(name, 0, &wname);
	std::string out;
	if (wname) {
		const wchar_t *value = _wgetenv(wname);
		if (value && *value) {
			char *utf8 = nullptr;
			os_wcs_to_utf8_ptr(value, 0, &utf8);
			if (utf8) {
				out = utf8;
				bfree(utf8);
			}
		}
		bfree(wname);
	}
	return out;
#else
	const char *value = getenv(name);
	return value ? value : "";
#endif
}

/* <module config>/runtime: ~/Library/Application Support/ctd-obs/plugin_config/ctd-ndi/runtime, or
 * %APPDATA%\ctd-obs\plugin_config\ctd-ndi\runtime */
[[maybe_unused]] static std::string RuntimeRoot()
{
	char *path = obs_module_config_path("runtime");
	std::string out = path ? path : "";
	bfree(path);
#ifdef _WIN32
	for (char &c : out) {
		if (c == '/') {
			c = '\\';
		}
	}
#endif
	return out;
}

static std::string ProgramFiles()
{
	return Env("ProgramFiles");
}

/* Folders a library is never loaded from, beyond the fixed ones in PathIsForbidden */
static std::vector<std::string> ExtraForbiddenRoots()
{
	std::vector<std::string> roots;
	const std::string ndiDir = Env(NDILIB_REDIST_FOLDER);
	if (!ndiDir.empty()) {
		roots.push_back(ndiDir);
	}
#ifdef _WIN32
	for (const char *var : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"}) {
		const std::string dir = Env(var);
		if (!dir.empty()) {
			roots.push_back(dir + "\\NDI");
		}
	}
	const std::string windows = Env("SystemRoot");
	if (!windows.empty()) {
		roots.push_back(windows);
	}
#endif
	return roots;
}

[[maybe_unused]] static std::vector<std::string> PresentVersions(const std::string &root)
{
	std::vector<std::string> out;
	std::error_code ec;
	const fs::path dir = U8Path(root);
	if (!fs::is_directory(dir, ec)) {
		return out;
	}
	for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
		if (it->is_directory(ec)) {
			out.push_back(PathU8(it->path().filename()));
		}
	}
	return out;
}

static LoaderInputs Inputs()
{
	LoaderInputs inputs;
	inputs.platform = CurrentPlatform();
#ifdef CTD_NDI_BUNDLED
	inputs.bundled = true;
#ifdef __APPLE__
	/* the module's binary is CTD OBS.app/Contents/PlugIns/ctd-ndi.plugin/Contents/MacOS/ctd-ndi */
	const char *binary = obs_get_module_binary_path(obs_current_module());
	fs::path p = U8Path(binary ? binary : "");
	for (int i = 0; i < 6 && !p.empty(); i++) {
		p = p.parent_path();
	}
	inputs.bundleOrModuleDir = PathU8(p);
#else
	const char *binary = obs_get_module_binary_path(obs_current_module());
	inputs.bundleOrModuleDir = PathU8(U8Path(binary ? binary : "").parent_path());
#endif
#else
	inputs.runtimeRoot = RuntimeRoot();
	inputs.presentVersions = PresentVersions(inputs.runtimeRoot);
#endif
	return inputs;
}

std::string InstalledVersion()
{
	const LoaderInputs inputs = Inputs();
	if (inputs.bundled) {
		return "bundled";
	}
	const auto candidates = LoadCandidates(inputs, AllowList());
	if (candidates.empty()) {
		return std::string();
	}
	return PathU8(U8Path(candidates.front()).parent_path().filename());
}

/* ------------------------------------------------------------------------------------------------ the platform loader */

static void *OpenLibrary(const std::string &path, std::string &error)
{
#ifdef _WIN32
	wchar_t *wpath = nullptr;
	os_utf8_to_wcs_ptr(path.c_str(), 0, &wpath);
	if (!wpath) {
		error = "path conversion failed";
		return nullptr;
	}
	/* by full path; its own folder for its dependencies, never the DLL search path's current directory */
	HMODULE module = LoadLibraryExW(wpath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	bfree(wpath);
	if (!module) {
		error = "LoadLibraryExW failed (" + std::to_string(GetLastError()) + ")";
	}
	return reinterpret_cast<void *>(module);
#else
	void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		const char *why = dlerror();
		error = why ? why : "dlopen failed";
	}
	return handle;
#endif
}

static void *FindSymbol(void *module, const char *name)
{
#ifdef _WIN32
	return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(module), name));
#else
	return dlsym(module, name);
#endif
}

LoadState CurrentLoadState()
{
	std::lock_guard<std::mutex> lock(loadMutex);
	return loadState;
}

LoadState LoadNdiLibrary()
{
	std::lock_guard<std::mutex> lock(loadMutex);
	if (loadState.loaded) {
		return loadState;
	}

	const Platform platform = CurrentPlatform();
	const LoaderInputs inputs = Inputs();
	const std::vector<std::string> roots = AllowedLoadRoots(inputs);
	const std::vector<std::string> forbidden = ExtraForbiddenRoots();
	const std::vector<std::string> candidates = LoadCandidates(inputs, AllowList());

	if (candidates.empty()) {
		loadState.message = inputs.bundled ? "The NDI library is missing from this build."
						   : "NDI isn't set up yet: install the NDI 6 Runtime, then Set up.";
		CTD_NDI_LOG(LOG_INFO, "%s", loadState.message.c_str());
		return loadState;
	}

	std::string lastError;
	for (const std::string &path : candidates) {
		if (!LoadPathAllowed(path, roots, platform, forbidden)) {
			/* can't happen with LoadCandidates' paths; the gate is checked anyway */
			CTD_NDI_LOG(LOG_ERROR, "Refused to load '%s': not in CTD OBS's own folder", path.c_str());
			lastError = "refused a path outside CTD OBS's folder";
			continue;
		}

		const AllowEntry *entry = nullptr;
		if (inputs.bundled) {
			/* signed with the app; its version must still be one we know */
			for (const AllowEntry &e : AllowList()) {
				if (e.platform == platform) {
					entry = &e;
					break;
				}
			}
		} else {
			const std::string sha = Sha256File(path);
			entry = FindAllowed(AllowList(), platform, sha);
			const std::string folder = PathU8(U8Path(path).parent_path().filename());
			if (!entry || folder != entry->version) {
				CTD_NDI_LOG(LOG_ERROR,
					    "Refused '%s': its SHA-256 %s is not the approved %s library; deleting it",
					    path.c_str(), sha.empty() ? "(unreadable)" : sha.c_str(), folder.c_str());
				std::error_code ec;
				fs::remove(U8Path(path), ec);
				lastError = "CTD OBS's copy of NDI " + folder + " changed; Set up again";
				continue;
			}
		}

		std::string error;
		void *module = OpenLibrary(path, error);
		if (!module) {
			CTD_NDI_LOG(LOG_ERROR, "Could not load '%s': %s", path.c_str(), error.c_str());
			lastError = "the NDI library could not be loaded (" + error + ")";
			continue;
		}

		using LoadFn = const NDIlib_v6 *(*)(void);
		auto load = reinterpret_cast<LoadFn>(FindSymbol(module, "NDIlib_v6_load"));
		const NDIlib_v6 *lib = load ? load() : nullptr;
		if (!lib || !lib->version || !lib->initialize || !lib->recv_create_v3 || !lib->send_create) {
			/* left mapped: unloading code that may have started threads isn't safe */
			CTD_NDI_LOG(LOG_ERROR, "'%s' has no usable NDIlib_v6_load", path.c_str());
			lastError = "the NDI library has no NDI 6 interface";
			continue;
		}

		const char *versionText = lib->version();
		if (!VersionStringMatches(versionText, entry->version)) {
			CTD_NDI_LOG(LOG_ERROR, "'%s' reports '%s', expected NDI %s; not used", path.c_str(),
				    versionText ? versionText : "(none)", entry->version);
			lastError = "the NDI library reports an unexpected version";
			continue;
		}

		if (!lib->initialize()) {
			CTD_NDI_LOG(LOG_ERROR, "NDIlib_initialize failed (this CPU isn't supported by NDI)");
			lastError = "NDI could not start on this computer (unsupported CPU)";
			continue;
		}

		loadedLib.store(lib, std::memory_order_release);
		loadState.loaded = true;
		loadState.version = entry->version;
		loadState.path = path;
		loadState.message = std::string("NDI ") + entry->version + " loaded";
		CTD_NDI_LOG(LOG_INFO, "Loaded NDI %s from '%s' (%s)", entry->version, path.c_str(),
			    versionText ? versionText : "");
		return loadState;
	}

	loadState.message = "NDI is off: " + lastError + ".";
	return loadState;
}

/* ------------------------------------------------------------------------------------------------ set up */

[[maybe_unused]] static bool CopyVerified(const std::string &from, const std::string &to, const char *wantSha,
					  std::string &error)
{
	std::error_code ec;
	const fs::path dst = U8Path(to);
	const fs::path tmp = U8Path(to + ".partial");
	fs::remove(tmp, ec);
	fs::copy_file(U8Path(from), tmp, fs::copy_options::overwrite_existing, ec);
	if (ec) {
		error = "copy failed: " + ec.message();
		return false;
	}
	if (wantSha && Sha256File(PathU8(tmp)) != wantSha) {
		fs::remove(tmp, ec);
		error = "the copy doesn't match the approved SHA-256";
		return false;
	}
	fs::remove(dst, ec);
	fs::rename(tmp, dst, ec);
	if (ec) {
		fs::remove(tmp, ec);
		error = "could not put the copy in place: " + ec.message();
		return false;
	}
	return true;
}

bool SetupFromRuntime(std::string &message)
{
#ifdef CTD_NDI_BUNDLED
	message = "This build has NDI built in; nothing to set up.";
	return true;
#else
	const Platform platform = CurrentPlatform();
	if (platform == Platform::Other) {
		message = "NDI isn't supported on this platform.";
		return false;
	}

	const std::string root = RuntimeRoot();
	if (root.empty()) {
		message = "CTD OBS's settings folder isn't available.";
		return false;
	}

	std::string notApproved;
	for (const std::string &candidate :
	     RuntimeLibraryCandidates(platform, ProgramFiles(), Env(NDILIB_REDIST_FOLDER))) {
		std::error_code ec;
		if (!fs::is_regular_file(U8Path(candidate), ec)) {
			continue;
		}
		/* hashed, never loaded */
		const std::string sha = Sha256File(candidate);
		const AllowEntry *entry = FindAllowed(AllowList(), platform, sha);
		if (!entry) {
			CTD_NDI_LOG(LOG_WARNING, "Found '%s' (SHA-256 %s): not an NDI version approved for CTD OBS",
				    candidate.c_str(), sha.empty() ? "(unreadable)" : sha.c_str());
			notApproved = candidate;
			continue;
		}

		const std::string lib = PrivateLibraryPath(root, entry->version, platform);
		const std::string license = PrivateLicensePath(root, entry->version, platform);
		fs::create_directories(U8Path(lib).parent_path(), ec);
		if (ec) {
			message = "Could not create CTD OBS's NDI folder: " + ec.message();
			return false;
		}

		std::string error;
		const std::string runtimeLicense = LicensePathBeside(candidate, platform);
		if (fs::is_regular_file(U8Path(runtimeLicense), ec)) {
			if (!CopyVerified(runtimeLicense, license, nullptr, error)) {
				CTD_NDI_LOG(LOG_WARNING, "NDI licence file not copied: %s", error.c_str());
			}
		}
		if (!CopyVerified(candidate, lib, entry->sha256, error)) {
			message = "Set up failed: " + error + ".";
			CTD_NDI_LOG(LOG_ERROR, "Set up from '%s' failed: %s", candidate.c_str(), error.c_str());
			return false;
		}

		message = std::string("NDI ") + entry->version + " set up from the NDI Runtime.";
		CTD_NDI_LOG(LOG_INFO, "Set up NDI %s: '%s' copied to '%s' (SHA-256 verified)", entry->version,
			    candidate.c_str(), lib.c_str());
		return true;
	}

	if (!notApproved.empty()) {
		message = "The installed NDI Runtime isn't a version approved for CTD OBS (" + notApproved +
			  "). NDI stays off.";
	} else {
		message = "No NDI 6 Runtime found. Install the NDI 6 Runtime (not NDI Tools), then Set up.";
	}
	return false;
#endif
}

std::vector<std::string> SystemPiecesFound()
{
	std::vector<std::string> found;
	for (const std::string &path : SystemNdiPiecePaths(CurrentPlatform(), ProgramFiles())) {
		std::error_code ec;
		if (fs::exists(U8Path(path), ec)) {
			found.push_back(path);
		}
	}
	return found;
}

} // namespace ctd_ndi
