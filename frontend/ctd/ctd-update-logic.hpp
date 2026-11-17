/******************************************************************************
    CTD OBS: update check logic (no Qt, no OBS), unit-tested by
    test/ctd-update (docs/ctd/updates.md).

    Releases are tagged ctd-v<X.Y.Z> (stable) or ctd-v<X.Y.Z>-test.<N>
    (test builds from ctd/main). A stable X.Y.Z is newer than any of its
    own test builds; test builds of the same X.Y.Z compare by N.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct CtdVersion {
	int major = 0;
	int minor = 0;
	int patch = 0;
	int test = 0; /* 0 = stable release, N = test build N */

	bool IsTest() const { return test > 0; }
	/* "0.10.0" or "0.10.0-test.42" */
	std::string ToString() const;
};

/* "0.10.0", "0.10.0-test.42", optionally with the "ctd-v" tag prefix. Anything else (OBS's own tags such as
 * "33.0.1", other suffixes) is rejected. */
bool CtdParseVersion(const std::string &text, CtdVersion &out);

/* <0, 0, >0 like strcmp */
int CtdCompareVersions(const CtdVersion &a, const CtdVersion &b);

/* The update settings in user.ini (frontend/ctd/ctd-update.cpp, Settings ▸ CTD ▸ Updates) */
inline constexpr char kCtdUpdateSection[] = "CTDUpdates";
inline constexpr char kCtdUpdateKeyAuto[] = "Auto";       /* bool, default true */
inline constexpr char kCtdUpdateKeyChannel[] = "Channel"; /* "stable" or "test" */

enum class CtdUpdateChannel { Stable, Test };
enum class CtdUpdatePlatform { Windows, MacOS, Other };

/* The platform this build installs updates for: CTD OBS ships Windows x64 and Apple Silicon macOS builds */
inline CtdUpdatePlatform CtdCurrentPlatform()
{
#if defined(_WIN32)
	return CtdUpdatePlatform::Windows;
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
	return CtdUpdatePlatform::MacOS;
#else
	return CtdUpdatePlatform::Other;
#endif
}

struct CtdReleaseAsset {
	std::string name;
	std::string url; /* browser_download_url */
	int64_t size = 0;
};

struct CtdRelease {
	std::string tag;
	CtdVersion version;
	std::string name;
	std::string notes; /* body, Markdown */
	std::string pageUrl;
	bool prerelease = false;
	bool draft = false;
	std::vector<CtdReleaseAsset> assets;

	const CtdReleaseAsset *FindAsset(const std::string &name) const;
	/* The installer (Windows: CTD-OBS-Setup-*-x64.exe) or app zip (macOS: *-macos-arm64.zip) */
	const CtdReleaseAsset *PlatformAsset(CtdUpdatePlatform platform) const;
	const CtdReleaseAsset *ChecksumAsset() const { return FindAsset("SHA256SUMS.txt"); }
};

/* GitHub's GET /repos/{owner}/{repo}/releases response. Releases whose tag isn't a CTD OBS version are left out. */
bool CtdParseReleases(const std::string &json, std::vector<CtdRelease> &out, std::string &error);

/* The newest release newer than current that this channel offers and that has this platform's asset and
 * SHA256SUMS.txt, or nullptr. Drafts are never offered; Stable ignores prereleases and test builds. */
const CtdRelease *CtdPickUpdate(const std::vector<CtdRelease> &releases, const CtdVersion &current,
				CtdUpdateChannel channel, CtdUpdatePlatform platform);

/* sha256sum output: "<64 hex>  <name>" (or "<hex> *<name>") per line -> name: lowercase hex */
std::map<std::string, std::string> CtdParseSha256Sums(const std::string &text);
