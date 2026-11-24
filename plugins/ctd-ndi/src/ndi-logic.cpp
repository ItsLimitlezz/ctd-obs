/******************************************************************************
    CTD OBS NDI module: mode, allow-list matching, path policy, SHA-256 and
    sender slots and rules (ndi-logic.hpp). No OBS or NDI dependency; unit-tested by
    test/ctd-ndi.

    Copyright (C) 2026 CineTech Digital

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ndi-logic.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ctd_ndi {

/* ---------------------------------------------------------------- mode */

Mode ParseMode(const char *text)
{
	if (!text) {
		return Mode::Off;
	}
	if (strcmp(text, "send") == 0) {
		return Mode::Send;
	}
	return Mode::Off;
}

const char *ModeName(Mode mode)
{
	switch (mode) {
	case Mode::Send:
		return "send";
	case Mode::Off:
		break;
	}
	return "off";
}

Mode EffectiveMode(Mode configured, bool launchDisabled, bool safeMode)
{
	return (launchDisabled || safeMode) ? Mode::Off : configured;
}

/* ---------------------------------------------------------- allow-list */

static bool IsHex64(const std::string &s)
{
	if (s.size() != 64) {
		return false;
	}
	for (char c : s) {
		if (!std::isxdigit(static_cast<unsigned char>(c))) {
			return false;
		}
	}
	return true;
}

static std::string Lower(std::string s)
{
	for (char &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

const AllowEntry *FindAllowed(const std::vector<AllowEntry> &list, Platform platform, const std::string &sha256Hex)
{
	if (!IsHex64(sha256Hex)) {
		return nullptr;
	}
	const std::string want = Lower(sha256Hex);
	for (const AllowEntry &entry : list) {
		if (entry.platform == platform && entry.sha256 && Lower(entry.sha256) == want) {
			return &entry;
		}
	}
	return nullptr;
}

bool VersionStringMatches(const char *ndiVersionText, const char *version)
{
	if (!ndiVersionText || !version || !*version) {
		return false;
	}
	const size_t len = strlen(version);
	for (const char *p = strstr(ndiVersionText, version); p; p = strstr(p + 1, version)) {
		const bool startOk = p == ndiVersionText ||
				     !(std::isdigit(static_cast<unsigned char>(p[-1])) || p[-1] == '.');
		const char after = p[len];
		const bool endOk = !(std::isdigit(static_cast<unsigned char>(after)) ||
				     (after == '.' && std::isdigit(static_cast<unsigned char>(p[len + 1]))));
		if (startOk && endOk) {
			return true;
		}
	}
	return false;
}

int CompareVersions(const std::string &a, const std::string &b)
{
	size_t i = 0, j = 0;
	while (i < a.size() || j < b.size()) {
		unsigned long long x = 0, y = 0;
		while (i < a.size() && a[i] != '.') {
			if (std::isdigit(static_cast<unsigned char>(a[i]))) {
				x = x * 10 + static_cast<unsigned>(a[i] - '0');
			}
			i++;
		}
		while (j < b.size() && b[j] != '.') {
			if (std::isdigit(static_cast<unsigned char>(b[j]))) {
				y = y * 10 + static_cast<unsigned>(b[j] - '0');
			}
			j++;
		}
		if (x != y) {
			return x < y ? -1 : 1;
		}
		if (i < a.size()) {
			i++;
		}
		if (j < b.size()) {
			j++;
		}
	}
	return 0;
}

/* --------------------------------------------------------- path policy */

const char *LibraryFileName(Platform platform)
{
	return platform == Platform::Windows ? "Processing.NDI.Lib.x64.dll" : "libndi.dylib";
}

const char *LicenseFileName(Platform platform)
{
	return platform == Platform::Windows ? "Processing.NDI.Lib.Licenses.txt" : "libndi_licenses.txt";
}

static char Sep(Platform platform)
{
	return platform == Platform::Windows ? '\\' : '/';
}

static std::string Join(const std::string &dir, const std::string &name, Platform platform)
{
	if (dir.empty()) {
		return name;
	}
	const char last = dir.back();
	if (last == '/' || last == '\\') {
		return dir + name;
	}
	return dir + Sep(platform) + name;
}

static std::string Dirname(const std::string &path)
{
	const size_t pos = path.find_last_of("/\\");
	return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

std::vector<std::string> RuntimeLibraryCandidates(Platform platform, const std::string &programFiles,
						  const std::string &ndiRuntimeDirEnv)
{
	std::vector<std::string> out;
	const char *lib = LibraryFileName(platform);
	if (platform == Platform::MacOS) {
		out.push_back(std::string("/usr/local/lib/") + lib);
		if (!ndiRuntimeDirEnv.empty()) {
			out.push_back(Join(ndiRuntimeDirEnv, lib, platform));
		}
	} else if (platform == Platform::Windows) {
		if (!ndiRuntimeDirEnv.empty()) {
			out.push_back(Join(ndiRuntimeDirEnv, lib, platform));
		}
		if (!programFiles.empty()) {
			out.push_back(Join(Join(programFiles, "NDI\\NDI 6 Runtime\\v6", platform), lib, platform));
			out.push_back(Join(Join(programFiles, "NDI\\NDI 6 Runtime", platform), lib, platform));
		}
	}
	return out;
}

std::string LicensePathBeside(const std::string &libraryPath, Platform platform)
{
	return Join(Dirname(libraryPath), LicenseFileName(platform), platform);
}

std::string PrivateLibraryPath(const std::string &runtimeRoot, const std::string &version, Platform platform)
{
	return Join(Join(runtimeRoot, version, platform), LibraryFileName(platform), platform);
}

std::string PrivateLicensePath(const std::string &runtimeRoot, const std::string &version, Platform platform)
{
	return Join(Join(runtimeRoot, version, platform), LicenseFileName(platform), platform);
}

std::string BundledLibraryPath(const std::string &bundleOrModuleDir, Platform platform)
{
	if (platform == Platform::MacOS) {
		return Join(bundleOrModuleDir, "Contents/Frameworks/libndi.dylib", platform);
	}
	return Join(bundleOrModuleDir, LibraryFileName(platform), platform);
}

static bool IsAbsolute(const std::string &path, Platform platform)
{
	if (platform == Platform::Windows) {
		if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' &&
		    (path[2] == '\\' || path[2] == '/')) {
			return true;
		}
		return path.size() >= 2 && (path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/');
	}
	return !path.empty() && path[0] == '/';
}

/* Forward slashes, lowercase on Windows, no empty / "." parts, ".." resolved lexically. Empty if ".." climbs above
 * the root (never allowed). */
static std::string Normalize(const std::string &path, Platform platform)
{
	std::string p = path;
	for (char &c : p) {
		if (c == '\\') {
			c = '/';
		}
	}
	if (platform == Platform::Windows) {
		p = Lower(p);
	}

	std::string prefix;
	size_t start = 0;
	if (platform == Platform::Windows && p.size() >= 2 && p[1] == ':') {
		prefix = p.substr(0, 2);
		start = 2;
	} else if (platform == Platform::Windows && p.rfind("//", 0) == 0) {
		prefix = "/";
		start = 1;
	}

	std::vector<std::string> parts;
	size_t i = start;
	while (i <= p.size()) {
		size_t next = p.find('/', i);
		if (next == std::string::npos) {
			next = p.size();
		}
		std::string part = p.substr(i, next - i);
		if (part.empty() || part == ".") {
			/* skip */
		} else if (part == "..") {
			if (parts.empty()) {
				return std::string();
			}
			parts.pop_back();
		} else {
			parts.push_back(part);
		}
		i = next + 1;
	}

	std::string out = prefix;
	for (const std::string &part : parts) {
		out += '/';
		out += part;
	}
	return out.empty() ? std::string("/") : out;
}

bool PathIsInside(const std::string &path, const std::string &root, Platform platform)
{
	if (!IsAbsolute(path, platform) || !IsAbsolute(root, platform)) {
		return false;
	}
	const std::string p = Normalize(path, platform);
	const std::string r = Normalize(root, platform);
	if (p.empty() || r.empty()) {
		return false;
	}
	if (r == "/") {
		return true;
	}
	return p.size() > r.size() && p.compare(0, r.size(), r) == 0 && p[r.size()] == '/';
}

bool PathIsForbidden(const std::string &path, Platform platform, const std::vector<std::string> &extraForbiddenRoots)
{
	if (!IsAbsolute(path, platform) || Normalize(path, platform).empty()) {
		return true;
	}

	std::vector<std::string> roots;
	if (platform == Platform::MacOS) {
		roots = {"/usr", "/opt", "/Library", "/System", "/bin", "/sbin", "/private/etc"};
	} else if (platform == Platform::Windows) {
		roots = {"C:\\Program Files\\NDI", "C:\\Program Files (x86)\\NDI", "C:\\Windows"};
	}
	for (const std::string &extra : extraForbiddenRoots) {
		if (!extra.empty()) {
			roots.push_back(extra);
		}
	}
	for (const std::string &root : roots) {
		if (IsAbsolute(root, platform) && PathIsInside(path, root, platform)) {
			return true;
		}
	}
	return false;
}

static std::string Basename(const std::string &path)
{
	const size_t pos = path.find_last_of("/\\");
	return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool LoadPathAllowed(const std::string &path, const std::vector<std::string> &allowedRoots, Platform platform,
		     const std::vector<std::string> &extraForbiddenRoots)
{
	if (platform == Platform::Other || !IsAbsolute(path, platform)) {
		return false;
	}
	const std::string name = Basename(path);
	const std::string want = LibraryFileName(platform);
	if (platform == Platform::Windows ? Lower(name) != Lower(want) : name != want) {
		return false;
	}
	if (PathIsForbidden(path, platform, extraForbiddenRoots)) {
		return false;
	}
	for (const std::string &root : allowedRoots) {
		if (!root.empty() && PathIsInside(path, root, platform)) {
			return true;
		}
	}
	return false;
}

std::vector<std::string> LoadCandidates(const LoaderInputs &inputs, const std::vector<AllowEntry> &list)
{
	std::vector<std::string> out;
	if (inputs.platform == Platform::Other) {
		return out;
	}
	if (inputs.bundled) {
		if (!inputs.bundleOrModuleDir.empty()) {
			out.push_back(BundledLibraryPath(inputs.bundleOrModuleDir, inputs.platform));
		}
		return out;
	}
	if (inputs.runtimeRoot.empty()) {
		return out;
	}

	std::vector<std::string> versions;
	for (const std::string &version : inputs.presentVersions) {
		/* only folder names that are an allow-listed version (no "..", no other names) */
		bool listed = false;
		for (const AllowEntry &entry : list) {
			if (entry.platform == inputs.platform && version == entry.version) {
				listed = true;
				break;
			}
		}
		if (listed && std::find(versions.begin(), versions.end(), version) == versions.end()) {
			versions.push_back(version);
		}
	}
	std::sort(versions.begin(), versions.end(),
		  [](const std::string &a, const std::string &b) { return CompareVersions(a, b) > 0; });
	for (const std::string &version : versions) {
		out.push_back(PrivateLibraryPath(inputs.runtimeRoot, version, inputs.platform));
	}
	return out;
}

std::vector<std::string> AllowedLoadRoots(const LoaderInputs &inputs)
{
	if (inputs.bundled) {
		if (inputs.bundleOrModuleDir.empty()) {
			return {};
		}
		if (inputs.platform == Platform::MacOS) {
			return {Join(inputs.bundleOrModuleDir, "Contents/Frameworks", inputs.platform)};
		}
		return {inputs.bundleOrModuleDir};
	}
	if (inputs.runtimeRoot.empty()) {
		return {};
	}
	return {inputs.runtimeRoot};
}

std::vector<std::string> SystemNdiPiecePaths(Platform platform, const std::string &programFiles)
{
	if (platform == Platform::MacOS) {
		return {
			"/Library/CoreMediaIO/Plug-Ins/DAL/NDIVideoOut.plugin",
			"/Library/Audio/Plug-Ins/HAL/NDIAudio.driver",
			"/Library/Audio/Plug-Ins/HAL/NDIAudioOut.driver",
			"/Library/LaunchDaemons/com.newtek.cmio.DPA.NDI.plist",
			"/Library/Application Support/NewTek/NDI/HX_Driver",
		};
	}
	if (platform == Platform::Windows && !programFiles.empty()) {
		return {
			Join(programFiles, "NDI\\NDI 6 Tools", platform),
			Join(programFiles, "NDI\\NDI 5 Tools", platform),
		};
	}
	return {};
}

/* -------------------------------------------------------------- SHA-256 */

static const uint32_t kK[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t Rotr(uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

Sha256::Sha256() : state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}
{
}

void Sha256::Block(const uint8_t *b)
{
	uint32_t w[64];
	for (int i = 0; i < 16; i++) {
		w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 | (uint32_t)b[i * 4 + 2] << 8 |
		       (uint32_t)b[i * 4 + 3];
	}
	for (int i = 16; i < 64; i++) {
		const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = state[0], b2 = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6],
		 h = state[7];
	for (int i = 0; i < 64; i++) {
		const uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
		const uint32_t ch = (e & f) ^ (~e & g);
		const uint32_t t1 = h + S1 + ch + kK[i] + w[i];
		const uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
		const uint32_t maj = (a & b2) ^ (a & c) ^ (b2 & c);
		const uint32_t t2 = S0 + maj;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b2;
		b2 = a;
		a = t1 + t2;
	}
	state[0] += a;
	state[1] += b2;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}

void Sha256::Update(const void *data, size_t size)
{
	if (finished) {
		return;
	}
	const uint8_t *p = static_cast<const uint8_t *>(data);
	totalLen += size;
	while (size > 0) {
		const size_t take = std::min(size, sizeof(buffer) - bufferLen);
		memcpy(buffer + bufferLen, p, take);
		bufferLen += take;
		p += take;
		size -= take;
		if (bufferLen == sizeof(buffer)) {
			Block(buffer);
			bufferLen = 0;
		}
	}
}

std::string Sha256::FinalHex()
{
	if (!finished) {
		const uint64_t bits = totalLen * 8;
		const uint8_t pad = 0x80;
		const uint8_t zero = 0;
		const uint64_t savedLen = totalLen;
		Update(&pad, 1);
		while (bufferLen != 56) {
			Update(&zero, 1);
		}
		uint8_t len[8];
		for (int i = 0; i < 8; i++) {
			len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
		}
		Update(len, 8);
		totalLen = savedLen;
		finished = true;
	}
	static const char *hex = "0123456789abcdef";
	std::string out;
	out.reserve(64);
	for (uint32_t v : state) {
		for (int i = 28; i >= 0; i -= 4) {
			out += hex[(v >> i) & 0xf];
		}
	}
	return out;
}

std::string Sha256Hex(const void *data, size_t size)
{
	Sha256 sha;
	sha.Update(data, size);
	return sha.FinalHex();
}

std::string Sha256File(const std::string &utf8Path)
{
	namespace fs = std::filesystem;
#if defined(__cpp_char8_t)
	const fs::path path(reinterpret_cast<const char8_t *>(utf8Path.c_str()));
#else
	const fs::path path = fs::u8path(utf8Path);
#endif
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return std::string();
	}
	Sha256 sha;
	std::vector<char> chunk(1 << 20);
	while (in) {
		in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
		const std::streamsize got = in.gcount();
		if (got > 0) {
			sha.Update(chunk.data(), static_cast<size_t>(got));
		}
	}
	if (in.bad()) {
		return std::string();
	}
	return sha.FinalHex();
}

/* --------------------------------------------------------- sender slots */

bool SenderSlots::TryAcquire()
{
	int current = inUse.load();
	while (current < max) {
		if (inUse.compare_exchange_weak(current, current + 1)) {
			return true;
		}
	}
	return false;
}

void SenderSlots::Release()
{
	int current = inUse.load();
	while (current > 0) {
		if (inUse.compare_exchange_weak(current, current - 1)) {
			return;
		}
	}
}

/* --------------------------------------------------------- sender rules */

std::string CleanSenderName(const std::string &name)
{
	std::string out;
	for (unsigned char c : name) {
		if (c < 0x20 || c == 0x7f) {
			continue; /* control characters, incl. newlines and tabs */
		}
		out += static_cast<char>(c);
	}
	const size_t first = out.find_first_not_of(' ');
	if (first == std::string::npos) {
		return std::string();
	}
	const size_t last = out.find_last_not_of(' ');
	out = out.substr(first, last - first + 1);
	if (out.size() > kMaxSenderNameBytes) {
		size_t cut = kMaxSenderNameBytes;
		/* back up to the start of a UTF-8 character */
		while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
			cut--;
		}
		out.resize(cut);
		const size_t end = out.find_last_not_of(' ');
		out.resize(end == std::string::npos ? 0 : end + 1);
	}
	return out;
}

SenderState DecideSender(const SenderInputs &in)
{
	if (!in.ndiOn) {
		return SenderState::Off;
	}
	if (in.paused) {
		return SenderState::Paused;
	}
	if (!in.filterEnabled) {
		return SenderState::Disabled;
	}
	if (!in.sourceActive) {
		return SenderState::Inactive;
	}
	if (!in.audioOnly && (in.width == 0 || in.height == 0)) {
		return SenderState::NoSize;
	}
	if (!in.nameUsable) {
		return SenderState::BadName;
	}
	if (in.nameTaken) {
		return SenderState::NameTaken;
	}
	if (!in.slotFree) {
		return SenderState::Limit;
	}
	return SenderState::Run;
}

const char *SenderStateText(SenderState state)
{
	switch (state) {
	case SenderState::Off:
		return "NDI is off";
	case SenderState::Paused:
		return "NDI is paused";
	case SenderState::Disabled:
		return "Stopped (filter disabled)";
	case SenderState::Inactive:
		return "Waiting: the source isn't on air";
	case SenderState::NoSize:
		return "Waiting: the source has no picture yet";
	case SenderState::BadName:
		return "No sender name";
	case SenderState::NameTaken:
		return "Another sender already uses this name";
	case SenderState::Limit:
		return "NDI sender limit reached";
	case SenderState::Run:
		break;
	}
	return "Sending";
}

void SenderCanvasSize(uint32_t width, uint32_t height, uint32_t &outWidth, uint32_t &outHeight)
{
	outWidth = 0;
	outHeight = 0;
	if (width == 0 || height == 0) {
		return;
	}
	constexpr uint32_t kMax = 4096;
	double w = width, h = height;
	if (w > kMax || h > kMax) {
		const double scale = std::min(kMax / w, kMax / h);
		w *= scale;
		h *= scale;
	}
	auto even = [](double v) {
		uint32_t n = static_cast<uint32_t>(v);
		n &= ~1u;
		return n < 2 ? 2u : n;
	};
	outWidth = std::min(even(w), kMax);
	outHeight = std::min(even(h), kMax);
}

} // namespace ctd_ndi
