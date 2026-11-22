/* CTD OBS: unit tests for the NDI module's logic (plugins/ctd-ndi/src/ndi-logic.cpp, ndi-allowlist.cpp) and the
 * memory watchdog (frontend/ctd/ctd-memory-watchdog-logic.cpp). docs/ctd/ndi.md */

#include "ndi-logic.hpp"
#include "ctd-memory-watchdog-logic.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace ctd_ndi;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                                \
	do {                                                                       \
		checks++;                                                          \
		if (!(cond)) {                                                     \
			failures++;                                                \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                  \
	} while (0)

/* ------------------------------------------------------------------ mode */

static void TestMode()
{
	CHECK(ParseMode(nullptr) == Mode::Off);
	CHECK(ParseMode("") == Mode::Off);
	CHECK(ParseMode("off") == Mode::Off);
	CHECK(ParseMode("receive") == Mode::Receive);
	CHECK(ParseMode("receive_send") == Mode::ReceiveSend);
	CHECK(ParseMode("Receive") == Mode::Off); /* exact only: anything unknown is Off */
	CHECK(ParseMode("send") == Mode::Off);
	CHECK(ParseMode("receive ") == Mode::Off);
	CHECK(std::strcmp(ModeName(Mode::ReceiveSend), "receive_send") == 0);
	CHECK(ParseMode(ModeName(Mode::Receive)) == Mode::Receive);

	CHECK(EffectiveMode(Mode::ReceiveSend, false, false) == Mode::ReceiveSend);
	CHECK(EffectiveMode(Mode::ReceiveSend, true, false) == Mode::Off); /* --ctd-no-ndi */
	CHECK(EffectiveMode(Mode::Receive, false, true) == Mode::Off);     /* --safe-mode */
	CHECK(EffectiveMode(Mode::Off, false, false) == Mode::Off);

	CHECK(!ModeReceives(Mode::Off) && ModeReceives(Mode::Receive) && ModeReceives(Mode::ReceiveSend));
	CHECK(!ModeSends(Mode::Off) && !ModeSends(Mode::Receive) && ModeSends(Mode::ReceiveSend));
}

/* ------------------------------------------------------------- allow-list */

static void TestAllowList()
{
	const auto &list = AllowList();
	CHECK(!list.empty());
	for (const AllowEntry &entry : list) {
		CHECK(entry.platform != Platform::Other);
		CHECK(entry.sha256 && std::strlen(entry.sha256) == 64);
		for (const char *c = entry.sha256; *c; c++) {
			CHECK((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f')); /* lowercase hex */
		}
		CHECK(entry.version && CompareVersions(entry.version, "6.0") >= 0);
	}

	const std::string mac = "b10315f9435c59a5db76dd44d27ea809432468151aea458359129f279ca6f09c";
	const std::string win = "adca7e9c98ddddb01a1b684f90c97d0bd764990ee2839524fde51ab37f21028a";

	const AllowEntry *e = FindAllowed(list, Platform::MacOS, mac);
	CHECK(e && std::strcmp(e->version, "6.4.0.2") == 0);
	e = FindAllowed(list, Platform::Windows, win);
	CHECK(e && std::strcmp(e->version, "6.4.0.2") == 0);

	std::string upper = mac;
	for (char &c : upper) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	CHECK(FindAllowed(list, Platform::MacOS, upper) != nullptr); /* case-insensitive */
	CHECK(FindAllowed(list, Platform::Windows, mac) == nullptr); /* other platform's hash */
	CHECK(FindAllowed(list, Platform::MacOS, win) == nullptr);
	CHECK(FindAllowed(list, Platform::Other, mac) == nullptr);
	CHECK(FindAllowed(list, Platform::MacOS, "") == nullptr); /* unreadable file */
	CHECK(FindAllowed(list, Platform::MacOS, mac.substr(0, 63)) == nullptr);
	CHECK(FindAllowed(list, Platform::MacOS, mac + "0") == nullptr);
	std::string flipped = mac;
	flipped[10] = flipped[10] == '0' ? '1' : '0';
	CHECK(FindAllowed(list, Platform::MacOS, flipped) == nullptr); /* one digit off: unknown */
	CHECK(FindAllowed(list, Platform::MacOS, std::string(64, 'g')) == nullptr);

	/* NDIlib_version() texts */
	CHECK(VersionStringMatches("NDI SDK APPLE 16:21:34 Nov 17 2026 6.4.0.2", "6.4.0.2"));
	CHECK(VersionStringMatches("NDI SDK WIN64 16:09:44 Nov 17 2026 6.4.0.2", "6.4.0.2"));
	CHECK(VersionStringMatches("6.4.0.2", "6.4.0.2"));
	CHECK(!VersionStringMatches("NDI SDK APPLE 6.4.0.20", "6.4.0.2"));
	CHECK(!VersionStringMatches("NDI SDK APPLE 16.4.0.2", "6.4.0.2"));
	CHECK(!VersionStringMatches("NDI SDK APPLE 6.4.0.2.1", "6.4.0.2"));
	CHECK(!VersionStringMatches("NDI SDK APPLE 6.4.1.0", "6.4.0.2"));
	CHECK(!VersionStringMatches(nullptr, "6.4.0.2"));
	CHECK(!VersionStringMatches("6.4.0.2", ""));

	CHECK(CompareVersions("6.4.0.2", "6.4.0.2") == 0);
	CHECK(CompareVersions("6.10", "6.9") > 0);
	CHECK(CompareVersions("6.4", "6.4.0.1") < 0);
	CHECK(CompareVersions("6.4.0.2", "6.3.0.3") > 0);
}

/* ----------------------------------------------------------- path policy */

static const std::vector<std::string> kMacSystem = {
	"/usr/local/lib/libndi.dylib",
	"/usr/lib/libndi.dylib",
	"/usr/local/Cellar/libndi/6.4/lib/libndi.dylib",
	"/opt/homebrew/lib/libndi.dylib",
	"/opt/homebrew/Cellar/libndi/6.4.0/lib/libndi.dylib",
	"/Library/NDI SDK for Apple/lib/macOS/libndi.dylib",
	"/Library/NDI/libndi.dylib",
	"/Library/CoreMediaIO/Plug-Ins/DAL/NDIVideoOut.plugin/Contents/Frameworks/libndi.dylib",
	"/Library/Audio/Plug-Ins/HAL/NDIAudio.driver/Contents/MacOS/libndi.dylib",
	"/Library/Frameworks/libndi.dylib",
	"/System/Library/Frameworks/libndi.dylib",
};

static const std::vector<std::string> kWinSystem = {
	"C:\\Program Files\\NDI\\NDI 6 Runtime\\v6\\Processing.NDI.Lib.x64.dll",
	"C:\\Program Files\\NDI\\NDI 6 Tools\\Runtime\\Processing.NDI.Lib.x64.dll",
	"c:/program files/ndi/ndi 6 runtime/v6/processing.ndi.lib.x64.dll",
	"C:\\PROGRAM FILES\\NDI\\Processing.NDI.Lib.x64.dll",
	"C:\\Program Files (x86)\\NDI\\Processing.NDI.Lib.x64.dll",
	"C:\\Windows\\System32\\Processing.NDI.Lib.x64.dll",
	"D:\\Apps\\NDI\\NDI 6 Runtime\\v6\\Processing.NDI.Lib.x64.dll", /* given as the real Program Files */
	"E:\\custom\\ndi\\Processing.NDI.Lib.x64.dll",                  /* NDI_RUNTIME_DIR_V6 */
};
static const std::vector<std::string> kWinExtraForbidden = {"D:\\Apps\\NDI", "E:\\custom\\ndi"};

static void TestPathPolicy()
{
	const std::string macRoot = "/Users/juan/Library/Application Support/ctd-obs/plugin_config/ctd-ndi/runtime";
	const std::string winRoot = "C:\\Users\\Juan\\AppData\\Roaming\\ctd-obs\\plugin_config\\ctd-ndi\\runtime";

	/* every system location is forbidden, also through tricks */
	for (const std::string &path : kMacSystem) {
		CHECK(PathIsForbidden(path, Platform::MacOS, {}));
		CHECK(!LoadPathAllowed(path, {macRoot}, Platform::MacOS, {}));
	}
	for (const std::string &path : kWinSystem) {
		CHECK(PathIsForbidden(path, Platform::Windows, kWinExtraForbidden));
		CHECK(!LoadPathAllowed(path, {winRoot}, Platform::Windows, kWinExtraForbidden));
	}
	CHECK(PathIsForbidden(macRoot + "/../../../../../../../../usr/local/lib/libndi.dylib", Platform::MacOS, {}));
	CHECK(!LoadPathAllowed(macRoot + "/6.4.0.2/../../../../../../../../../usr/local/lib/libndi.dylib", {macRoot},
			       Platform::MacOS, {}));
	CHECK(PathIsForbidden("//usr/local/lib/libndi.dylib", Platform::MacOS, {}));
	CHECK(PathIsForbidden("/usr/./local/lib/libndi.dylib", Platform::MacOS, {}));
	CHECK(PathIsForbidden("libndi.dylib", Platform::MacOS, {})); /* relative: a search */
	CHECK(PathIsForbidden("Processing.NDI.Lib.x64.dll", Platform::Windows, {}));
	CHECK(PathIsForbidden("/../usr/lib/libndi.dylib", Platform::MacOS, {})); /* climbs above the root */

	/* CTD OBS's own folders aren't */
	CHECK(!PathIsForbidden(macRoot + "/6.4.0.2/libndi.dylib", Platform::MacOS, {}));
	CHECK(!PathIsForbidden("/Applications/CTD OBS.app/Contents/Frameworks/libndi.dylib", Platform::MacOS, {}));
	CHECK(!PathIsForbidden(winRoot + "\\6.4.0.2\\Processing.NDI.Lib.x64.dll", Platform::Windows,
			       kWinExtraForbidden));
	CHECK(!PathIsForbidden("C:\\Program Files\\CTD OBS\\obs-plugins\\64bit\\Processing.NDI.Lib.x64.dll",
			       Platform::Windows, kWinExtraForbidden));
	/* a user's ~/Library is not /Library */
	CHECK(!PathIsForbidden("/Users/juan/Library/x/libndi.dylib", Platform::MacOS, {}));

	CHECK(PathIsInside("/a/b/c", "/a/b", Platform::MacOS));
	CHECK(!PathIsInside("/a/bc", "/a/b", Platform::MacOS));
	CHECK(!PathIsInside("/a/b", "/a/b", Platform::MacOS));
	CHECK(PathIsInside("C:\\A\\B\\c.dll", "c:/a/b", Platform::Windows));
	CHECK(!PathIsInside("/A/B/c", "/a/b", Platform::MacOS)); /* case-sensitive compare on macOS paths */
}

static void TestLoaderPaths()
{
	const auto &list = AllowList();
	const std::string macRoot = "/Users/juan/Library/Application Support/ctd-obs/plugin_config/ctd-ndi/runtime";
	const std::string winRoot = "C:\\Users\\Juan\\AppData\\Roaming\\ctd-obs\\plugin_config\\ctd-ndi\\runtime";

	/* not bundled: only CTD OBS's own copies of allow-listed versions */
	LoaderInputs mac;
	mac.platform = Platform::MacOS;
	mac.runtimeRoot = macRoot;
	mac.presentVersions = {"6.4.0.2", "..", "9.9.9.9", "6.4.0.2", "tmp", ""};
	auto paths = LoadCandidates(mac, list);
	CHECK(paths.size() == 1);
	CHECK(!paths.empty() && paths[0] == macRoot + "/6.4.0.2/libndi.dylib");
	for (const std::string &path : paths) {
		CHECK(LoadPathAllowed(path, AllowedLoadRoots(mac), Platform::MacOS, {}));
		for (const std::string &system : kMacSystem) {
			CHECK(path != system);
		}
	}
	mac.presentVersions = {};
	CHECK(LoadCandidates(mac, list).empty()); /* not set up: nothing to load, never a fallback */
	mac.runtimeRoot = "";
	mac.presentVersions = {"6.4.0.2"};
	CHECK(LoadCandidates(mac, list).empty());
	CHECK(AllowedLoadRoots(mac).empty());

	LoaderInputs win;
	win.platform = Platform::Windows;
	win.runtimeRoot = winRoot;
	win.presentVersions = {"6.4.0.2"};
	paths = LoadCandidates(win, list);
	CHECK(paths.size() == 1);
	CHECK(!paths.empty() && paths[0] == winRoot + "\\6.4.0.2\\Processing.NDI.Lib.x64.dll");
	for (const std::string &path : paths) {
		CHECK(LoadPathAllowed(path, AllowedLoadRoots(win), Platform::Windows, kWinExtraForbidden));
	}

	/* newest first when several approved versions are present */
	std::vector<AllowEntry> two = {{Platform::MacOS, "6.4.0.2", list[0].sha256},
				       {Platform::MacOS, "6.10.0.1", list[0].sha256}};
	mac.runtimeRoot = macRoot;
	mac.presentVersions = {"6.4.0.2", "6.10.0.1"};
	paths = LoadCandidates(mac, two);
	CHECK(paths.size() == 2 && paths[0] == macRoot + "/6.10.0.1/libndi.dylib");

	/* bundled build: only the bundle */
	LoaderInputs bundled;
	bundled.platform = Platform::MacOS;
	bundled.bundled = true;
	bundled.bundleOrModuleDir = "/Applications/CTD OBS.app";
	bundled.runtimeRoot = macRoot;
	bundled.presentVersions = {"6.4.0.2"};
	paths = LoadCandidates(bundled, list);
	CHECK(paths.size() == 1 && paths[0] == "/Applications/CTD OBS.app/Contents/Frameworks/libndi.dylib");
	CHECK(LoadPathAllowed(paths[0], AllowedLoadRoots(bundled), Platform::MacOS, {}));
	CHECK(!LoadPathAllowed(macRoot + "/6.4.0.2/libndi.dylib", AllowedLoadRoots(bundled), Platform::MacOS, {}));

	/* the Runtime's own files are where Set up reads from, and never a load path */
	for (const std::string &path : RuntimeLibraryCandidates(Platform::MacOS, "", "")) {
		CHECK(!LoadPathAllowed(path, {macRoot}, Platform::MacOS, {}));
		CHECK(PathIsForbidden(path, Platform::MacOS, {}));
	}
	CHECK(RuntimeLibraryCandidates(Platform::MacOS, "", "").at(0) == "/usr/local/lib/libndi.dylib");
	const auto winRuntime = RuntimeLibraryCandidates(Platform::Windows, "C:\\Program Files",
							 "C:\\Program Files\\NDI\\NDI 6 Runtime\\v6\\");
	CHECK(winRuntime.size() == 3);
	CHECK(winRuntime.at(1) == "C:\\Program Files\\NDI\\NDI 6 Runtime\\v6\\Processing.NDI.Lib.x64.dll");
	for (const std::string &path : winRuntime) {
		CHECK(PathIsForbidden(path, Platform::Windows, {}));
		CHECK(!LoadPathAllowed(path, {winRoot}, Platform::Windows, {}));
	}
	CHECK(LicensePathBeside("/usr/local/lib/libndi.dylib", Platform::MacOS) ==
	      "/usr/local/lib/libndi_licenses.txt");

	/* the gate also checks the file name */
	CHECK(!LoadPathAllowed(macRoot + "/6.4.0.2/evil.dylib", {macRoot}, Platform::MacOS, {}));
	CHECK(LoadPathAllowed(winRoot + "\\6.4.0.2\\processing.ndi.lib.x64.dll", {winRoot}, Platform::Windows, {}));
	CHECK(!LoadPathAllowed(macRoot + "/6.4.0.2/libndi.dylib", {}, Platform::MacOS, {}));
	CHECK(!LoadPathAllowed(macRoot + "/6.4.0.2/libndi.dylib", {macRoot}, Platform::Other, {}));

	CHECK(SystemNdiPiecePaths(Platform::MacOS, "").size() >= 3);
}

/* ---------------------------------------------------------------- SHA-256 */

static void TestSha256()
{
	CHECK(Sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	CHECK(Sha256Hex(two, std::strlen(two)) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

	/* a million 'a', fed in odd chunks */
	Sha256 sha;
	std::string chunk(997, 'a');
	size_t left = 1000000;
	while (left) {
		const size_t take = std::min(left, chunk.size());
		sha.Update(chunk.data(), take);
		left -= take;
	}
	CHECK(sha.FinalHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

	/* exactly 55, 56 and 64 bytes: the padding edges */
	CHECK(Sha256Hex(std::string(55, 'a').data(), 55) ==
	      "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
	CHECK(Sha256Hex(std::string(56, 'a').data(), 56) ==
	      "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
	CHECK(Sha256Hex(std::string(64, 'a').data(), 64) ==
	      "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");

	/* a file, and a missing one */
	const auto file = std::filesystem::temp_directory_path() / "ctd-ndi-test-sha.bin";
	{
		std::ofstream out(file, std::ios::binary);
		out << "abc";
	}
	CHECK(Sha256File(file.string()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	std::filesystem::remove(file);
	CHECK(Sha256File(file.string()).empty());
}

/* ---------------------------------------------------------- bounded queue */

static void TestBoundedQueue()
{
	BoundedQueue<int> q(2);
	CHECK(q.Capacity() == 2);
	CHECK(!q.Push(1).has_value());
	CHECK(!q.Push(2).has_value());
	auto evicted = q.Push(3); /* full: drops the oldest */
	CHECK(evicted.has_value() && *evicted == 1);
	CHECK(q.Size() == 2);
	CHECK(q.Dropped() == 1);
	for (int i = 4; i < 1000; i++) {
		q.Push(i);
		CHECK(q.Size() <= 2); /* never accumulates */
	}
	CHECK(q.Dropped() == 997);
	auto a = q.TryPop();
	auto b = q.TryPop();
	CHECK(a && *a == 998 && b && *b == 999); /* the newest survive, in order */
	CHECK(!q.TryPop().has_value());

	BoundedQueue<int> zero(0); /* capacity 0 means 1 */
	CHECK(zero.Capacity() == 1);

	/* buffers come back: a pool of 3 owned buffers, pushed far faster than popped */
	struct Buf {
		std::vector<uint8_t> data;
	};
	BoundedQueue<std::unique_ptr<Buf>> frames(2);
	std::vector<std::unique_ptr<Buf>> pool;
	for (int i = 0; i < 3; i++) {
		pool.push_back(std::make_unique<Buf>());
	}
	for (int i = 0; i < 500; i++) {
		CHECK(!pool.empty());
		auto buf = std::move(pool.back());
		pool.pop_back();
		if (auto old = frames.Push(std::move(buf))) {
			pool.push_back(std::move(*old));
		}
		if (i % 7 == 0) {
			if (auto got = frames.TryPop()) {
				pool.push_back(std::move(*got));
			}
		}
		CHECK(pool.size() + frames.Size() == 3); /* no buffer lost, none made */
	}

	/* threads: a producer much faster than the consumer */
	BoundedQueue<int> mt(4);
	std::atomic<bool> done{false};
	size_t maxSeen = 0;
	std::thread consumer([&]() {
		while (!done.load() || mt.Size() > 0) {
			if (mt.PopWait(std::chrono::milliseconds(5))) {
				std::this_thread::sleep_for(std::chrono::microseconds(200));
			}
		}
	});
	for (int i = 0; i < 20000; i++) {
		mt.Push(i);
		maxSeen = std::max(maxSeen, mt.Size());
	}
	done = true;
	mt.Close();
	consumer.join();
	CHECK(maxSeen <= 4);
	CHECK(mt.Dropped() > 0);

	/* Close wakes a waiting consumer */
	BoundedQueue<int> closing(1);
	std::thread waiter([&]() { CHECK(!closing.PopWait(std::chrono::seconds(10)).has_value()); });
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const auto start = std::chrono::steady_clock::now();
	closing.Close();
	waiter.join();
	CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}

/* --------------------------------------------------------- receiver slots */

static void TestReceiverSlots()
{
	ReceiverSlots slots(kMaxReceivers);
	CHECK(slots.Max() == 8);
	std::vector<SlotGuard> held;
	for (int i = 0; i < 8; i++) {
		held.emplace_back(&slots);
		CHECK(held.back().Held());
	}
	SlotGuard ninth(&slots);
	CHECK(!ninth.Held()); /* the cap */
	CHECK(slots.InUse() == 8);
	held.pop_back(); /* a source destroyed */
	CHECK(slots.InUse() == 7);
	SlotGuard again(&slots);
	CHECK(again.Held());
	SlotGuard moved(std::move(again));
	CHECK(moved.Held() && !again.Held() && slots.InUse() == 8);
	moved.Reset();
	moved.Reset(); /* twice is harmless */
	CHECK(slots.InUse() == 7);
	held.clear();
	CHECK(slots.InUse() == 0);
	slots.Release(); /* never below zero */
	CHECK(slots.InUse() == 0);

	/* many threads competing: never above the cap */
	ReceiverSlots shared(8);
	std::atomic<int> peak{0};
	std::vector<std::thread> threads;
	for (int t = 0; t < 16; t++) {
		threads.emplace_back([&]() {
			for (int i = 0; i < 2000; i++) {
				SlotGuard guard(&shared);
				if (guard.Held()) {
					int now = shared.InUse();
					int prev = peak.load();
					while (now > prev && !peak.compare_exchange_weak(prev, now)) {
					}
				}
			}
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}
	CHECK(peak.load() <= 8);
	CHECK(shared.InUse() == 0);
}

/* -------------------------------------------------------------- watchdog */

static void TestWatchdog()
{
	const uint64_t MB = kCtdMiB;

	/* flat: no warning, ever */
	{
		CtdWatchdogLogic w;
		for (int t = 0; t <= 1800; t += 10) {
			w.AddSample(t, 1500 * MB);
			CHECK(w.Evaluate() == CtdWatchdogWarning::None);
		}
		CHECK(w.GrowthMbPerMin(180) == 0.0);
	}

	/* the incident: ~1 GB/min. Warns once 3 full minutes are there, not before */
	{
		CtdWatchdogLogic w;
		bool warned = false;
		double warnedAt = -1;
		for (int t = 0; t <= 600; t += 10) {
			w.AddSample(t, (1500 + static_cast<uint64_t>(t) * 1000 / 60) * MB);
			if (w.Evaluate() == CtdWatchdogWarning::Growth && !warned) {
				warned = true;
				warnedAt = t;
			}
		}
		CHECK(warned);
		CHECK(warnedAt == 180.0);
		CHECK(w.GrowthMbPerMin(180) > 990 && w.GrowthMbPerMin(180) < 1010);
	}

	/* 250 MB/min steady: warns after 3 min; then not again for 10 min */
	{
		CtdWatchdogLogic w;
		int warnings = 0;
		for (int t = 0; t <= 1200; t += 10) {
			w.AddSample(t, (1000 + static_cast<uint64_t>(t) * 250 / 60) * MB);
			if (w.Evaluate() != CtdWatchdogWarning::None) {
				warnings++;
			}
		}
		/* t=180 and t=780 (600 s later); not at every sample. Footprint passes 6144 MB at ~1235 s, later */
		CHECK(warnings == 2);
	}

	/* 150 MB/min: under the rate, no warning (until the limit) */
	{
		CtdWatchdogLogic w;
		for (int t = 0; t <= 900; t += 10) {
			w.AddSample(t, (1000 + static_cast<uint64_t>(t) * 150 / 60) * MB);
			CHECK(w.Evaluate() == CtdWatchdogWarning::None);
		}
	}

	/* one big spike (a scene load: +900 MB at once), then flat: not sustained */
	{
		CtdWatchdogLogic w;
		for (int t = 0; t <= 900; t += 10) {
			w.AddSample(t, (t < 300 ? 1000 : 1900) * MB);
			CHECK(w.Evaluate() == CtdWatchdogWarning::None);
		}
	}

	/* two fast minutes then one slow: not 3 sustained minutes */
	{
		CtdWatchdogLogic w;
		uint64_t bytes = 1000 * MB;
		bool warned = false;
		for (int t = 0; t <= 360; t += 10) {
			const int minute = t / 60;
			bytes += (minute % 3 == 2 ? 5 : 50) * MB; /* 300/min, 300/min, 30/min, … */
			w.AddSample(t, bytes);
			warned = warned || w.Evaluate() != CtdWatchdogWarning::None;
		}
		CHECK(!warned);
	}

	/* the limit */
	{
		CtdWatchdogConfig config;
		config.limitMb = 4000;
		CtdWatchdogLogic w(config);
		w.AddSample(0, 3999 * MB);
		CHECK(w.Evaluate() == CtdWatchdogWarning::None);
		w.AddSample(10, 4001 * MB);
		CHECK(w.OverLimit());
		CHECK(w.Evaluate() == CtdWatchdogWarning::Limit);
		w.AddSample(20, 4002 * MB);
		CHECK(w.Evaluate() == CtdWatchdogWarning::None); /* rate-limited */
		w.AddSample(620, 4002 * MB);
		CHECK(w.Evaluate() == CtdWatchdogWarning::Limit); /* 10 min later, still over */
	}

	/* samples out of order are ignored; old ones are dropped */
	{
		CtdWatchdogLogic w;
		w.AddSample(100, 10 * MB);
		w.AddSample(90, 999 * MB);
		w.AddSample(100, 999 * MB);
		CHECK(w.LatestBytes() && *w.LatestBytes() == 10 * MB);
		for (int t = 110; t < 5000; t += 10) {
			w.AddSample(t, 10 * MB);
		}
		CHECK(!w.BytesAt(1000).has_value()); /* history beyond the window is gone */
		CHECK(w.BytesAt(4990).has_value());
	}

	/* a log line every 60 s, with samples every 10 s */
	{
		CtdWatchdogLogic w;
		int lines = 0;
		for (int t = 0; t < 600; t += 10) {
			w.AddSample(t, 1 * MB);
			if (w.LogDue()) {
				lines++;
			}
		}
		CHECK(lines == 10);
	}

	/* the line itself (the soak script parses it) */
	CtdWatchdogNdiStatus ndi;
	ndi.mode = "receive_send";
	ndi.receivers = 2;
	ndi.sends = 1;
	CHECK(CtdWatchdogLogLine(1834 * MB, 12.4, 3, ndi) ==
	      "[ctd-watchdog] footprint 1834 MB (+12 MB/min over 3 min), NDI receive_send: 2 receivers, 1 send, 0 dropped");
	CHECK(ndi.Active());
	ndi.paused = true;
	CHECK(!ndi.Active());
	CHECK(CtdWatchdogLogLine(1834 * MB, -3, 3, ndi) ==
	      "[ctd-watchdog] footprint 1834 MB (-3 MB/min over 3 min), NDI receive_send (paused): 2 receivers, 1 send, 0 dropped");
	CtdWatchdogNdiStatus off;
	CHECK(!off.Active());
	CHECK(CtdWatchdogLogLine(700 * MB, 0, 3, off) ==
	      "[ctd-watchdog] footprint 700 MB (+0 MB/min over 3 min), NDI off");
}

int main()
{
	TestMode();
	TestAllowList();
	TestPathPolicy();
	TestLoaderPaths();
	TestSha256();
	TestBoundedQueue();
	TestReceiverSlots();
	TestWatchdog();
	std::printf("%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
