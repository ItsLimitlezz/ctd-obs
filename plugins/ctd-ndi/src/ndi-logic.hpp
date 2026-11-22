/******************************************************************************
    CTD OBS NDI module: the parts with no OBS and no NDI dependency, so
    test/ctd-ndi can unit-test them (docs/ctd/ndi.md).

      mode           Off / Receive / ReceiveSend, from user.ini and the launch
      allow-list     {platform, version, SHA-256} of the NDI libraries CTD
                     OBS accepts (ndi-allowlist.cpp)
      path policy    where the loader may load from (CTD OBS's own folder or
                     the app bundle) and where it never may (system paths)
      SHA-256        to check a library before it is ever loaded
      BoundedQueue   never holds more than its capacity; drops the oldest
      ReceiverSlots  the cap on NDI receivers

    Copyright (C) 2026 CineTech Digital

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ctd_ndi {

/* -------------------------------------------------------------------------
 * Mode
 * ---------------------------------------------------------------------- */

enum class Mode { Off, Receive, ReceiveSend };

/* "off", "receive", "receive_send"; anything else (missing, misspelt) is Off */
Mode ParseMode(const char *text);
const char *ModeName(Mode mode);

/* The mode this launch runs with: --ctd-no-ndi and OBS's safe mode turn NDI off whatever the setting */
Mode EffectiveMode(Mode configured, bool launchDisabled, bool safeMode);

inline bool ModeReceives(Mode mode)
{
	return mode != Mode::Off;
}
inline bool ModeSends(Mode mode)
{
	return mode == Mode::ReceiveSend;
}

/* -------------------------------------------------------------------------
 * Platforms and the allow-list
 * ---------------------------------------------------------------------- */

enum class Platform { MacOS, Windows, Other };

constexpr Platform CurrentPlatform()
{
#if defined(__APPLE__)
	return Platform::MacOS;
#elif defined(_WIN32)
	return Platform::Windows;
#else
	return Platform::Other;
#endif
}

struct AllowEntry {
	Platform platform;
	const char *version; /* "6.4.0.2"; NDIlib_version() must contain it */
	const char *sha256;  /* lowercase hex of the library file */
};

/* ndi-allowlist.cpp */
const std::vector<AllowEntry> &AllowList();

/* The entry whose platform and SHA-256 match, or nullptr. Case-insensitive hex; anything not 64 hex digits never
 * matches. */
const AllowEntry *FindAllowed(const std::vector<AllowEntry> &list, Platform platform, const std::string &sha256Hex);

/* True if NDIlib_version()'s text names the entry's version as a whole token ("… 6.4.0.2", not "6.4.0.20") */
bool VersionStringMatches(const char *ndiVersionText, const char *version);

/* <0, 0, >0 like strcmp, numerically per dotted part ("6.10" > "6.9") */
int CompareVersions(const std::string &a, const std::string &b);

/* -------------------------------------------------------------------------
 * Path policy
 * ---------------------------------------------------------------------- */

/* The library's file name: libndi.dylib / Processing.NDI.Lib.x64.dll */
const char *LibraryFileName(Platform platform);
/* NDI's licence file that must travel with the library */
const char *LicenseFileName(Platform platform);

/* Where the official NDI 6 Runtime puts its library, in order. Only ever hashed and copied from, never loaded:
 *   macOS    /usr/local/lib/libndi.dylib, then <NDI_RUNTIME_DIR_V6>/libndi.dylib if that is set
 *   Windows  <NDI_RUNTIME_DIR_V6>\…dll if set, <programFiles>\NDI\NDI 6 Runtime\v6\…dll, <programFiles>\NDI\NDI 6 Runtime\…dll
 * The SHA-256 allow-list decides whether a file found there is accepted. */
std::vector<std::string> RuntimeLibraryCandidates(Platform platform, const std::string &programFiles,
						  const std::string &ndiRuntimeDirEnv);
/* NDI's licence file in the same folder as a library */
std::string LicensePathBeside(const std::string &libraryPath, Platform platform);

/* CTD OBS's own copy: <runtimeRoot>/<version>/<library> (runtimeRoot is the module's config folder + "/runtime") */
std::string PrivateLibraryPath(const std::string &runtimeRoot, const std::string &version, Platform platform);
std::string PrivateLicensePath(const std::string &runtimeRoot, const std::string &version, Platform platform);

/* CTD_NDI_BUNDLED builds: <app>/Contents/Frameworks/libndi.dylib, or the DLL next to ctd-ndi.dll */
std::string BundledLibraryPath(const std::string &bundleOrModuleDir, Platform platform);

/* System locations nothing may ever be loaded from:
 *   macOS    /usr (/usr/lib, /usr/local/lib, Homebrew on Intel), /opt (Homebrew, MacPorts), /Library (NDI SDK, DAL, HAL,
 *            Frameworks), /System
 *   Windows  C:\Program Files\NDI, C:\Program Files (x86)\NDI, C:\Windows
 * plus the given extra roots (the real Program Files\NDI and Windows folders, NDI_RUNTIME_DIR_V6). Case-insensitive on
 * Windows, either slash. The app bundle (/Applications/…) and the user's own folders are not in it. */
bool PathIsForbidden(const std::string &path, Platform platform, const std::vector<std::string> &extraForbiddenRoots);

/* True if path is inside root (after normalising slashes, "." and ".."), not just a prefix match ("/a/bc" is not in
 * "/a/b") */
bool PathIsInside(const std::string &path, const std::string &root, Platform platform);

/* The loader's single gate: absolute, inside one of the allowed roots, not forbidden, and named like the library */
bool LoadPathAllowed(const std::string &path, const std::vector<std::string> &allowedRoots, Platform platform,
		     const std::vector<std::string> &extraForbiddenRoots);

/* What the loader may load, in the order it tries. The loader (ndi-loader.cpp) loads nothing else, and still passes
 * each path through LoadPathAllowed with AllowedLoadRoots.
 *   bundled build   only BundledLibraryPath(bundleOrModuleDir)
 *   otherwise       CTD OBS's own copies, <runtimeRoot>/<version>/<library>, for the versions present that are on the
 *                   allow-list for this platform, newest first */
struct LoaderInputs {
	Platform platform = CurrentPlatform();
	bool bundled = false;
	std::string bundleOrModuleDir;
	std::string runtimeRoot;
	std::vector<std::string> presentVersions; /* folder names found under runtimeRoot */
};
std::vector<std::string> LoadCandidates(const LoaderInputs &inputs, const std::vector<AllowEntry> &list);
std::vector<std::string> AllowedLoadRoots(const LoaderInputs &inputs);

/* System-wide NDI pieces of the 2026-11-22 incident (NDI Tools), to warn about. Paths only; the module checks them. */
std::vector<std::string> SystemNdiPiecePaths(Platform platform, const std::string &programFiles);

/* -------------------------------------------------------------------------
 * SHA-256
 * ---------------------------------------------------------------------- */

class Sha256 {
public:
	Sha256();
	void Update(const void *data, size_t size);
	/* lowercase hex; the object can't be updated afterwards */
	std::string FinalHex();

private:
	void Block(const uint8_t *block);
	uint32_t state[8];
	uint8_t buffer[64];
	size_t bufferLen = 0;
	uint64_t totalLen = 0;
	bool finished = false;
};

std::string Sha256Hex(const void *data, size_t size);
/* UTF-8 path; empty string if the file can't be read */
std::string Sha256File(const std::string &utf8Path);

/* -------------------------------------------------------------------------
 * BoundedQueue: a thread-safe FIFO that never holds more than `capacity` items. A push into a full queue drops the
 * oldest item and hands it back (so its buffer can be reused); nothing ever accumulates.
 * ---------------------------------------------------------------------- */

template<typename T> class BoundedQueue {
public:
	explicit BoundedQueue(size_t capacity_) : capacity(capacity_ ? capacity_ : 1) {}

	/* Returns the evicted oldest item when the queue was full */
	std::optional<T> Push(T item)
	{
		std::optional<T> evicted;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (items.size() >= capacity) {
				evicted.emplace(std::move(items.front()));
				items.pop_front();
				dropped++;
			}
			items.push_back(std::move(item));
		}
		cv.notify_one();
		return evicted;
	}

	/* Waits up to `timeout` for an item; nullopt on timeout or after Close() */
	std::optional<T> PopWait(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait_for(lock, timeout, [this]() { return closed || !items.empty(); });
		if (items.empty()) {
			return std::nullopt;
		}
		std::optional<T> item(std::move(items.front()));
		items.pop_front();
		return item;
	}

	std::optional<T> TryPop() { return PopWait(std::chrono::milliseconds(0)); }

	/* Wakes PopWait; items stay until Drain */
	void Close()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			closed = true;
		}
		cv.notify_all();
	}

	std::vector<T> Drain()
	{
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<T> out;
		for (auto &item : items) {
			out.push_back(std::move(item));
		}
		items.clear();
		return out;
	}

	size_t Size() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return items.size();
	}
	size_t Capacity() const { return capacity; }
	uint64_t Dropped() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return dropped;
	}

private:
	const size_t capacity;
	mutable std::mutex mutex;
	std::condition_variable cv;
	std::deque<T> items;
	uint64_t dropped = 0;
	bool closed = false;
};

/* -------------------------------------------------------------------------
 * ReceiverSlots: the cap on concurrently connected NDI receivers
 * ---------------------------------------------------------------------- */

constexpr int kMaxReceivers = 8;

class ReceiverSlots {
public:
	explicit ReceiverSlots(int max_) : max(max_) {}
	bool TryAcquire();
	void Release();
	int InUse() const { return inUse.load(); }
	int Max() const { return max; }

private:
	const int max;
	std::atomic<int> inUse{0};
};

/* Holds one slot while it lives (moved, never copied) */
class SlotGuard {
public:
	SlotGuard() = default;
	explicit SlotGuard(ReceiverSlots *slots_) : slots(slots_ && slots_->TryAcquire() ? slots_ : nullptr) {}
	SlotGuard(SlotGuard &&other) noexcept : slots(other.slots) { other.slots = nullptr; }
	SlotGuard &operator=(SlotGuard &&other) noexcept
	{
		if (this != &other) {
			Reset();
			slots = other.slots;
			other.slots = nullptr;
		}
		return *this;
	}
	SlotGuard(const SlotGuard &) = delete;
	SlotGuard &operator=(const SlotGuard &) = delete;
	~SlotGuard() { Reset(); }

	bool Held() const { return slots != nullptr; }
	void Reset()
	{
		if (slots) {
			slots->Release();
			slots = nullptr;
		}
	}

private:
	ReceiverSlots *slots = nullptr;
};

} // namespace ctd_ndi
