/* CTD OBS: unit tests for frontend/ctd/ctd-plugin-isolation-logic.cpp (finding OBS Studio's third-party plugins and
 * copying one into CTD OBS's folder without touching OBS Studio's). */

#include "ctd-plugin-isolation-logic.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

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

static void WriteFile(const fs::path &path, const std::string &text)
{
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary);
	out << text;
}

static std::string ReadFile(const fs::path &path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static fs::path MakeTemp(const char *name)
{
	fs::path dir = fs::temp_directory_path() / (std::string("ctd-plugins-test-") + name);
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir);
	return dir;
}

static bool NoBundled(const std::string &)
{
	return false;
}

static void TestBundles()
{
	const fs::path tmp = MakeTemp("bundles");
	const fs::path stock = tmp / "obs-studio" / "plugins";
	const fs::path ctd = tmp / "ctd-obs" / "plugins";

	WriteFile(stock / "distroav.plugin" / "Contents" / "MacOS" / "distroav", "binary");
	WriteFile(stock / "distroav.plugin" / "Contents" / "Resources" / "locale" / "en-US.ini", "Name=NDI");
	WriteFile(stock / "Aitum-Stream-Suite.plugin" / "Contents" / "MacOS" / "x", "y");
	WriteFile(stock / "notes.txt", "not a plugin");
	fs::create_directories(stock / ".hidden.plugin");
	fs::create_directories(stock / "folder-not-bundle");

	std::vector<CtdStockPlugin> found = CtdFindStockPlugins({{stock, ctd, CtdPluginLayout::Bundle, {}}}, NoBundled);
	CHECK(found.size() == 2);
	if (found.size() == 2) {
		CHECK(found[0].name == "Aitum-Stream-Suite"); /* sorted case-insensitively */
		CHECK(found[1].name == "distroav");
		CHECK(found[1].destination == ctd / "distroav.plugin");
		CHECK(!found[1].alreadyInCtd);

		std::string error;
		CHECK(CtdCopyStockPlugin(found[1], error));
		CHECK(error.empty());
		CHECK(ReadFile(ctd / "distroav.plugin" / "Contents" / "MacOS" / "distroav") == "binary");
		CHECK(ReadFile(ctd / "distroav.plugin" / "Contents" / "Resources" / "locale" / "en-US.ini") ==
		      "Name=NDI");
		CHECK(!fs::exists(ctd / "distroav.plugin.ctd-copy"));

		/* OBS Studio's copy is untouched */
		CHECK(ReadFile(stock / "distroav.plugin" / "Contents" / "MacOS" / "distroav") == "binary");

		/* never over something already there */
		CHECK(!CtdCopyStockPlugin(found[1], error));
		CHECK(!error.empty());

		found = CtdFindStockPlugins({{stock, ctd, CtdPluginLayout::Bundle, {}}}, NoBundled);
		CHECK(found.size() == 2 && found[1].alreadyInCtd && !found[0].alreadyInCtd);
	}

	/* the bundled-module filter applies */
	found = CtdFindStockPlugins({{stock, ctd, CtdPluginLayout::Bundle, {}}},
				    [](const std::string &name) { return name == "distroav"; });
	CHECK(found.size() == 1 && found[0].name == "Aitum-Stream-Suite");

	fs::remove_all(tmp);
}

static void TestFolders()
{
	const fs::path tmp = MakeTemp("folders");
	const fs::path stock = tmp / "ProgramData" / "obs-studio" / "plugins";
	const fs::path ctd = tmp / "ProgramData" / "ctd-obs" / "plugins";

	WriteFile(stock / "distroav" / "bin" / "64bit" / "distroav.dll", "dll");
	WriteFile(stock / "distroav" / "data" / "locale" / "en-US.ini", "ini");
	WriteFile(stock / "loose.dll", "loose");

	std::vector<CtdStockPlugin> found = CtdFindStockPlugins({{stock, ctd, CtdPluginLayout::Folder, {}}}, NoBundled);
	CHECK(found.size() == 1);
	if (found.size() == 1) {
		CHECK(found[0].name == "distroav");
		std::string error;
		CHECK(CtdCopyStockPlugin(found[0], error));
		CHECK(ReadFile(ctd / "distroav" / "bin" / "64bit" / "distroav.dll") == "dll");
		CHECK(ReadFile(ctd / "distroav" / "data" / "locale" / "en-US.ini") == "ini");
		CHECK(fs::exists(stock / "distroav" / "bin" / "64bit" / "distroav.dll"));
	}

	/* missing folders are fine */
	found = CtdFindStockPlugins({{tmp / "nope", ctd, CtdPluginLayout::Folder, {}}}, NoBundled);
	CHECK(found.empty());

	/* a source that vanished is reported, nothing is created */
	CtdStockPlugin gone;
	gone.name = "gone";
	gone.layout = CtdPluginLayout::Folder;
	gone.source = stock / "gone";
	gone.destination = ctd / "gone";
	std::string error;
	CHECK(!CtdCopyStockPlugin(gone, error));
	CHECK(!error.empty());
	CHECK(!fs::exists(ctd / "gone"));

	fs::remove_all(tmp);
}

static void TestLegacyDlls()
{
	const fs::path tmp = MakeTemp("legacy");
	const fs::path bin = tmp / "obs-studio" / "obs-plugins" / "64bit";
	const fs::path data = tmp / "obs-studio" / "data" / "obs-plugins";
	const fs::path ctd = tmp / "ctd-obs" / "plugins";

	WriteFile(bin / "distroav.dll", "ndi");
	WriteFile(data / "distroav" / "locale" / "en-US.ini", "ini");
	WriteFile(bin / "win-capture.dll", "obs");
	WriteFile(data / "win-capture" / "locale" / "en-US.ini", "ini");
	WriteFile(bin / "libcef.dll", "cef");
	WriteFile(bin / "helper.dll", "no data folder");
	WriteFile(bin / "distroav.pdb", "symbols");

	auto isBundled = [](const std::string &name) {
		return name == "win-capture";
	};
	std::vector<CtdStockPlugin> found =
		CtdFindStockPlugins({{bin, ctd, CtdPluginLayout::LegacyDll, data}}, isBundled);
	CHECK(found.size() == 1);
	if (found.size() == 1) {
		CHECK(found[0].name == "distroav");
		CHECK(found[0].destination == ctd / "distroav");
		std::string error;
		CHECK(CtdCopyStockPlugin(found[0], error));
		CHECK(ReadFile(ctd / "distroav" / "bin" / "64bit" / "distroav.dll") == "ndi");
		CHECK(ReadFile(ctd / "distroav" / "data" / "locale" / "en-US.ini") == "ini");
		CHECK(fs::exists(bin / "distroav.dll"));
		CHECK(fs::exists(data / "distroav"));
	}

	/* without OBS Studio's data folder nothing is listed */
	found = CtdFindStockPlugins({{bin, ctd, CtdPluginLayout::LegacyDll, {}}}, isBundled);
	CHECK(found.empty());

	fs::remove_all(tmp);
}

static void TestSeveralRoots()
{
	const fs::path tmp = MakeTemp("roots");
	WriteFile(tmp / "user" / "obs-studio" / "plugins" / "b.plugin" / "Contents" / "MacOS" / "b", "b");
	WriteFile(tmp / "system" / "obs-studio" / "plugins" / "a.plugin" / "Contents" / "MacOS" / "a", "a");
	WriteFile(tmp / "system" / "obs-studio" / "plugins" / "b.plugin" / "Contents" / "MacOS" / "b", "b2");

	const fs::path ctd = tmp / "ctd";
	std::vector<CtdStockPlugin> found =
		CtdFindStockPlugins({{tmp / "user" / "obs-studio" / "plugins", ctd, CtdPluginLayout::Bundle, {}},
				     {tmp / "system" / "obs-studio" / "plugins", ctd, CtdPluginLayout::Bundle, {}}},
				    NoBundled);
	CHECK(found.size() == 3);
	if (found.size() == 3) {
		CHECK(found[0].name == "a");
		CHECK(found[1].name == "b" && found[2].name == "b");
		std::string error;
		CHECK(CtdCopyStockPlugin(found[1], error));
		/* the second b now goes to a taken destination */
		CHECK(!CtdCopyStockPlugin(found[2], error));
	}

	fs::remove_all(tmp);
}

int main()
{
	TestBundles();
	TestFolders();
	TestLegacyDlls();
	TestSeveralRoots();
	std::printf("%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
