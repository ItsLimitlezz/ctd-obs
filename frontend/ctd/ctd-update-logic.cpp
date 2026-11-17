/******************************************************************************
    CTD OBS: update check logic (see ctd-update-logic.hpp).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ctd-update-logic.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

static const char kTagPrefix[] = "ctd-v";
static const char kTestSuffix[] = "-test.";

/* A decimal number of 1-9 digits at text[pos], advancing pos */
static bool ReadNumber(const std::string &text, size_t &pos, int &out)
{
	size_t start = pos;
	while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
		pos++;
	}
	const size_t digits = pos - start;
	if (digits == 0 || digits > 9) {
		return false;
	}
	out = std::stoi(text.substr(start, digits));
	return true;
}

std::string CtdVersion::ToString() const
{
	std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
	if (test > 0) {
		s += kTestSuffix + std::to_string(test);
	}
	return s;
}

bool CtdParseVersion(const std::string &text, CtdVersion &out)
{
	size_t pos = 0;
	if (text.compare(0, sizeof(kTagPrefix) - 1, kTagPrefix) == 0) {
		pos = sizeof(kTagPrefix) - 1;
	}

	CtdVersion v;
	if (!ReadNumber(text, pos, v.major) || pos >= text.size() || text[pos++] != '.' ||
	    !ReadNumber(text, pos, v.minor) || pos >= text.size() || text[pos++] != '.' ||
	    !ReadNumber(text, pos, v.patch)) {
		return false;
	}

	if (pos < text.size()) {
		if (text.compare(pos, sizeof(kTestSuffix) - 1, kTestSuffix) != 0) {
			return false;
		}
		pos += sizeof(kTestSuffix) - 1;
		if (!ReadNumber(text, pos, v.test) || v.test == 0 || pos != text.size()) {
			return false;
		}
	}

	out = v;
	return true;
}

int CtdCompareVersions(const CtdVersion &a, const CtdVersion &b)
{
	if (a.major != b.major) {
		return a.major < b.major ? -1 : 1;
	}
	if (a.minor != b.minor) {
		return a.minor < b.minor ? -1 : 1;
	}
	if (a.patch != b.patch) {
		return a.patch < b.patch ? -1 : 1;
	}
	if (a.test == b.test) {
		return 0;
	}
	/* semver: a pre-release sorts before its release */
	if (a.test == 0) {
		return 1;
	}
	if (b.test == 0) {
		return -1;
	}
	return a.test < b.test ? -1 : 1;
}

static bool EndsWith(const std::string &s, const std::string &suffix)
{
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool StartsWith(const std::string &s, const std::string &prefix)
{
	return s.compare(0, prefix.size(), prefix) == 0;
}

const CtdReleaseAsset *CtdRelease::FindAsset(const std::string &assetName) const
{
	for (const CtdReleaseAsset &asset : assets) {
		if (asset.name == assetName) {
			return &asset;
		}
	}
	return nullptr;
}

const CtdReleaseAsset *CtdRelease::PlatformAsset(CtdUpdatePlatform platform) const
{
	for (const CtdReleaseAsset &asset : assets) {
		switch (platform) {
		case CtdUpdatePlatform::Windows:
			if (StartsWith(asset.name, "CTD-OBS-Setup-") && EndsWith(asset.name, "-x64.exe")) {
				return &asset;
			}
			break;
		case CtdUpdatePlatform::MacOS:
			if (StartsWith(asset.name, "ctd-obs-") && EndsWith(asset.name, "-macos-arm64.zip")) {
				return &asset;
			}
			break;
		case CtdUpdatePlatform::Other:
			return nullptr;
		}
	}
	return nullptr;
}

static std::string JsonString(const nlohmann::json &obj, const char *key)
{
	auto it = obj.find(key);
	return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

static bool JsonBool(const nlohmann::json &obj, const char *key)
{
	auto it = obj.find(key);
	return it != obj.end() && it->is_boolean() && it->get<bool>();
}

bool CtdParseReleases(const std::string &json, std::vector<CtdRelease> &out, std::string &error)
{
	out.clear();
	nlohmann::json root = nlohmann::json::parse(json, nullptr, false);
	if (root.is_discarded()) {
		error = "the response is not JSON";
		return false;
	}
	if (!root.is_array()) {
		/* GitHub errors (rate limit, not found) are an object with "message" */
		const std::string message = root.is_object() ? JsonString(root, "message") : std::string();
		error = message.empty() ? "unexpected response" : message;
		return false;
	}

	for (const nlohmann::json &item : root) {
		if (!item.is_object()) {
			continue;
		}
		CtdRelease release;
		release.tag = JsonString(item, "tag_name");
		if (!StartsWith(release.tag, kTagPrefix) || !CtdParseVersion(release.tag, release.version)) {
			continue;
		}
		release.name = JsonString(item, "name");
		release.notes = JsonString(item, "body");
		release.pageUrl = JsonString(item, "html_url");
		release.prerelease = JsonBool(item, "prerelease");
		release.draft = JsonBool(item, "draft");

		auto assets = item.find("assets");
		if (assets != item.end() && assets->is_array()) {
			for (const nlohmann::json &a : *assets) {
				if (!a.is_object()) {
					continue;
				}
				CtdReleaseAsset asset;
				asset.name = JsonString(a, "name");
				asset.url = JsonString(a, "browser_download_url");
				auto size = a.find("size");
				if (size != a.end() && size->is_number_integer()) {
					asset.size = size->get<int64_t>();
				}
				if (!asset.name.empty() && StartsWith(asset.url, "https://")) {
					release.assets.push_back(std::move(asset));
				}
			}
		}
		out.push_back(std::move(release));
	}
	return true;
}

const CtdRelease *CtdPickUpdate(const std::vector<CtdRelease> &releases, const CtdVersion &current,
				CtdUpdateChannel channel, CtdUpdatePlatform platform)
{
	const CtdRelease *best = nullptr;
	for (const CtdRelease &release : releases) {
		if (release.draft) {
			continue;
		}
		if (channel == CtdUpdateChannel::Stable && (release.prerelease || release.version.IsTest())) {
			continue;
		}
		if (!release.PlatformAsset(platform) || !release.ChecksumAsset()) {
			continue;
		}
		if (CtdCompareVersions(release.version, current) <= 0) {
			continue;
		}
		if (!best || CtdCompareVersions(release.version, best->version) > 0) {
			best = &release;
		}
	}
	return best;
}

std::map<std::string, std::string> CtdParseSha256Sums(const std::string &text)
{
	std::map<std::string, std::string> sums;
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) {
			end = text.size();
		}
		std::string line = text.substr(start, end - start);
		start = end + 1;

		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
			line.pop_back();
		}
		if (line.size() < 66) {
			continue;
		}
		std::string hash = line.substr(0, 64);
		if (!std::all_of(hash.begin(), hash.end(), [](char c) { return std::isxdigit((unsigned char)c); })) {
			continue;
		}
		size_t pos = 64;
		if (line[pos] != ' ' && line[pos] != '\t') {
			continue;
		}
		while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
			pos++;
		}
		if (pos < line.size() && line[pos] == '*') {
			pos++;
		}
		std::string name = line.substr(pos);
		if (name.empty()) {
			continue;
		}
		std::transform(hash.begin(), hash.end(), hash.begin(),
			       [](char c) { return (char)std::tolower((unsigned char)c); });
		sums[name] = hash;
	}
	return sums;
}
