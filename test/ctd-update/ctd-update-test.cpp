/* CTD OBS: unit tests for frontend/ctd/ctd-update-logic.cpp (version compare, release JSON, SHA256SUMS). */

#include "ctd-update-logic.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

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

static CtdVersion V(const char *text)
{
	CtdVersion v;
	if (!CtdParseVersion(text, v)) {
		std::printf("FAIL: could not parse '%s'\n", text);
		failures++;
	}
	return v;
}

static int Cmp(const char *a, const char *b)
{
	return CtdCompareVersions(V(a), V(b));
}

static void TestParseVersion()
{
	CtdVersion v;
	CHECK(CtdParseVersion("0.10.0", v) && v.major == 0 && v.minor == 10 && v.patch == 0 && v.test == 0);
	CHECK(CtdParseVersion("ctd-v0.10.0", v) && v.minor == 10 && !v.IsTest());
	CHECK(CtdParseVersion("ctd-v0.10.0-test.42", v) && v.patch == 0 && v.test == 42 && v.IsTest());
	CHECK(CtdParseVersion("1.2.3-test.7", v) && v.major == 1 && v.minor == 2 && v.patch == 3 && v.test == 7);
	CHECK(V("ctd-v0.10.0-test.42").ToString() == "0.10.0-test.42");
	CHECK(V("12.0.3").ToString() == "12.0.3");

	CHECK(!CtdParseVersion("", v));
	CHECK(!CtdParseVersion("0.10", v));
	CHECK(!CtdParseVersion("0.10.", v));
	CHECK(!CtdParseVersion("v0.10.0", v));
	CHECK(!CtdParseVersion("ctd-v", v));
	CHECK(!CtdParseVersion("0.10.0-test.", v));
	CHECK(!CtdParseVersion("0.10.0-test.0", v));
	CHECK(!CtdParseVersion("0.10.0-test.4x", v));
	CHECK(!CtdParseVersion("0.10.0-beta1", v));
	CHECK(!CtdParseVersion("0.10.0 ", v));
	CHECK(!CtdParseVersion("0.10.0.1", v));
	CHECK(!CtdParseVersion("1234567890.0.0", v));
	CHECK(!CtdParseVersion("a.b.c", v));
}

static void TestCompare()
{
	CHECK(Cmp("0.10.0", "0.10.0") == 0);
	CHECK(Cmp("0.10.0-test.5", "0.10.0-test.5") == 0);
	CHECK(Cmp("0.10.1", "0.10.0") > 0);
	CHECK(Cmp("0.10.0", "0.10.1") < 0);
	CHECK(Cmp("0.11.0", "0.10.9") > 0);
	CHECK(Cmp("1.0.0", "0.99.99") > 0);
	/* numeric, not string, comparison */
	CHECK(Cmp("0.10.0", "0.9.0") > 0);
	CHECK(Cmp("0.10.0-test.10", "0.10.0-test.9") > 0);
	/* a stable release is newer than its own test builds */
	CHECK(Cmp("0.10.0", "0.10.0-test.99") > 0);
	CHECK(Cmp("0.10.0-test.99", "0.10.0") < 0);
	/* a test build of the next version is newer than the stable release before it */
	CHECK(Cmp("0.10.1-test.1", "0.10.0") > 0);
	CHECK(Cmp("0.10.0-test.200", "0.10.1-test.1") < 0);
}

static const char *kReleasesJson = R"json([
  {
    "html_url": "https://github.com/ItsLimitlezz/ctd-obs/releases/tag/ctd-v0.11.0-test.57",
    "tag_name": "ctd-v0.11.0-test.57",
    "name": "Test build 0.11.0-test.57",
    "draft": false,
    "prerelease": true,
    "body": "Test build from ctd/main",
    "assets": [
      {"name": "CTD-OBS-Setup-0.11.0-test.57-x64.exe", "size": 123,
       "browser_download_url": "https://github.com/ItsLimitlezz/ctd-obs/releases/download/ctd-v0.11.0-test.57/CTD-OBS-Setup-0.11.0-test.57-x64.exe"},
      {"name": "ctd-obs-0.11.0-test.57-windows-x64.zip", "size": 456,
       "browser_download_url": "https://github.com/ItsLimitlezz/ctd-obs/releases/download/ctd-v0.11.0-test.57/ctd-obs-0.11.0-test.57-windows-x64.zip"},
      {"name": "SHA256SUMS.txt", "size": 300,
       "browser_download_url": "https://github.com/ItsLimitlezz/ctd-obs/releases/download/ctd-v0.11.0-test.57/SHA256SUMS.txt"}
    ]
  },
  {
    "html_url": "https://github.com/ItsLimitlezz/ctd-obs/releases/tag/ctd-v0.11.0-test.56",
    "tag_name": "ctd-v0.11.0-test.56",
    "name": "Test build 0.11.0-test.56",
    "draft": false,
    "prerelease": true,
    "body": null,
    "assets": [
      {"name": "CTD-OBS-Setup-0.11.0-test.56-x64.exe", "size": 123,
       "browser_download_url": "https://github.com/ItsLimitlezz/ctd-obs/releases/download/ctd-v0.11.0-test.56/CTD-OBS-Setup-0.11.0-test.56-x64.exe"},
      {"name": "ctd-obs-0.11.0-test.56-macos-arm64.zip", "size": 789,
       "browser_download_url": "https://github.com/ItsLimitlezz/ctd-obs/releases/download/ctd-v0.11.0-test.56/ctd-obs-0.11.0-test.56-macos-arm64.zip"},
      {"name": "SHA256SUMS.txt", "size": 300,
       "browser_download_url": "https://github.com/ItsLimitlezz/ctd-obs/releases/download/ctd-v0.11.0-test.56/SHA256SUMS.txt"}
    ]
  },
  {
    "html_url": "https://github.com/ItsLimitlezz/ctd-obs/releases/tag/ctd-v0.11.0",
    "tag_name": "ctd-v0.11.0",
    "name": "CTD OBS 0.11.0",
    "draft": true,
    "prerelease": false,
    "body": "Draft: waiting for the SR check",
    "assets": [
      {"name": "CTD-OBS-Setup-0.11.0-x64.exe", "size": 1, "browser_download_url": "https://github.com/x/y/releases/download/untagged-1/CTD-OBS-Setup-0.11.0-x64.exe"},
      {"name": "ctd-obs-0.11.0-macos-arm64.zip", "size": 1, "browser_download_url": "https://github.com/x/y/releases/download/untagged-1/ctd-obs-0.11.0-macos-arm64.zip"},
      {"name": "SHA256SUMS.txt", "size": 1, "browser_download_url": "https://github.com/x/y/releases/download/untagged-1/SHA256SUMS.txt"}
    ]
  },
  {
    "html_url": "https://github.com/ItsLimitlezz/ctd-obs/releases/tag/ctd-v0.10.1",
    "tag_name": "ctd-v0.10.1",
    "name": "CTD OBS 0.10.1",
    "draft": false,
    "prerelease": false,
    "body": "## Fixes\n- one",
    "assets": [
      {"name": "CTD-OBS-Setup-0.10.1-x64.exe", "size": 150000000,
       "browser_download_url": "https://github.com/ItsLimitlezz/ctd-obs/releases/download/ctd-v0.10.1/CTD-OBS-Setup-0.10.1-x64.exe"},
      {"name": "ctd-obs-0.10.1-windows-x64.zip", "size": 2,
       "browser_download_url": "https://github.com/ItsLimitlezz/ctd-obs/releases/download/ctd-v0.10.1/ctd-obs-0.10.1-windows-x64.zip"},
      {"name": "ctd-obs-0.10.1-macos-arm64.zip", "size": 3,
       "browser_download_url": "https://github.com/ItsLimitlezz/ctd-obs/releases/download/ctd-v0.10.1/ctd-obs-0.10.1-macos-arm64.zip"},
      {"name": "SHA256SUMS.txt", "size": 4,
       "browser_download_url": "https://github.com/ItsLimitlezz/ctd-obs/releases/download/ctd-v0.10.1/SHA256SUMS.txt"}
    ]
  },
  {
    "tag_name": "33.0.1",
    "name": "OBS Studio 33.0.1",
    "draft": false,
    "prerelease": false,
    "assets": []
  },
  {
    "tag_name": "ctd-v0.10.0",
    "name": "CTD OBS 0.10.0",
    "draft": false,
    "prerelease": false,
    "assets": [
      {"name": "CTD-OBS-Setup-0.10.0-x64.exe", "browser_download_url": "http://insecure.example/setup.exe"},
      {"name": "SHA256SUMS.txt", "browser_download_url": "https://github.com/ItsLimitlezz/ctd-obs/releases/download/ctd-v0.10.0/SHA256SUMS.txt"}
    ]
  },
  "not an object",
  {"tag_name": 5}
])json";

static void TestParseReleases()
{
	std::vector<CtdRelease> releases;
	std::string error;
	CHECK(CtdParseReleases(kReleasesJson, releases, error));
	CHECK(releases.size() == 5); /* OBS's own tag, the string and the bad tag are left out */
	if (releases.size() != 5) {
		return;
	}

	const CtdRelease &test57 = releases[0];
	CHECK(test57.tag == "ctd-v0.11.0-test.57");
	CHECK(test57.version.test == 57 && test57.version.minor == 11);
	CHECK(test57.prerelease && !test57.draft);
	CHECK(test57.notes == "Test build from ctd/main");
	CHECK(test57.assets.size() == 3);
	CHECK(test57.PlatformAsset(CtdUpdatePlatform::Windows) &&
	      test57.PlatformAsset(CtdUpdatePlatform::Windows)->name == "CTD-OBS-Setup-0.11.0-test.57-x64.exe");
	CHECK(test57.PlatformAsset(CtdUpdatePlatform::MacOS) == nullptr);
	CHECK(test57.PlatformAsset(CtdUpdatePlatform::Other) == nullptr);
	CHECK(test57.ChecksumAsset() != nullptr);

	CHECK(releases[1].notes.empty()); /* "body": null */
	CHECK(releases[2].draft);

	const CtdRelease &stable = releases[3];
	CHECK(stable.pageUrl == "https://github.com/ItsLimitlezz/ctd-obs/releases/tag/ctd-v0.10.1");
	CHECK(stable.PlatformAsset(CtdUpdatePlatform::Windows)->size == 150000000);
	CHECK(stable.PlatformAsset(CtdUpdatePlatform::MacOS)->name == "ctd-obs-0.10.1-macos-arm64.zip");

	/* a non-https asset URL is dropped */
	CHECK(releases[4].assets.size() == 1);

	/* errors */
	CHECK(!CtdParseReleases("<html>", releases, error) && !error.empty());
	CHECK(!CtdParseReleases(R"({"message": "API rate limit exceeded"})", releases, error) &&
	      error == "API rate limit exceeded");
	CHECK(CtdParseReleases("[]", releases, error) && releases.empty());
}

static void TestPickUpdate()
{
	std::vector<CtdRelease> releases;
	std::string error;
	CtdParseReleases(kReleasesJson, releases, error);

	const auto W = CtdUpdatePlatform::Windows;
	const auto M = CtdUpdatePlatform::MacOS;
	const auto S = CtdUpdateChannel::Stable;
	const auto T = CtdUpdateChannel::Test;

	/* Stable: the published 0.10.1, never the draft 0.11.0 or test builds */
	const CtdRelease *pick = CtdPickUpdate(releases, V("0.10.0"), S, W);
	CHECK(pick && pick->tag == "ctd-v0.10.1");
	pick = CtdPickUpdate(releases, V("0.10.0"), S, M);
	CHECK(pick && pick->tag == "ctd-v0.10.1");
	CHECK(CtdPickUpdate(releases, V("0.10.1"), S, W) == nullptr);
	CHECK(CtdPickUpdate(releases, V("0.11.0"), S, W) == nullptr);
	/* a test build on the Stable channel is offered the stable release */
	pick = CtdPickUpdate(releases, V("0.10.1-test.3"), S, W);
	CHECK(pick && pick->tag == "ctd-v0.10.1");

	/* Test builds: the newest build with this platform's asset */
	pick = CtdPickUpdate(releases, V("0.10.0"), T, W);
	CHECK(pick && pick->tag == "ctd-v0.11.0-test.57");
	pick = CtdPickUpdate(releases, V("0.10.0"), T, M); /* test.57 has no macOS zip */
	CHECK(pick && pick->tag == "ctd-v0.11.0-test.56");
	pick = CtdPickUpdate(releases, V("0.11.0-test.56"), T, W);
	CHECK(pick && pick->tag == "ctd-v0.11.0-test.57");
	CHECK(CtdPickUpdate(releases, V("0.11.0-test.57"), T, W) == nullptr);
	CHECK(CtdPickUpdate(releases, V("0.11.0-test.56"), T, M) == nullptr);
	/* the draft 0.11.0 is never offered, even though it is newer than every test build */
	CHECK(CtdPickUpdate(releases, V("0.11.0-test.57"), T, M) == nullptr);

	/* no installer for this platform */
	CHECK(CtdPickUpdate(releases, V("0.1.0"), T, CtdUpdatePlatform::Other) == nullptr);

	/* a release without SHA256SUMS.txt is not offered */
	std::vector<CtdRelease> noSums = releases;
	for (CtdRelease &r : noSums) {
		r.assets.erase(std::remove_if(r.assets.begin(), r.assets.end(),
					      [](const CtdReleaseAsset &a) { return a.name == "SHA256SUMS.txt"; }),
			       r.assets.end());
	}
	CHECK(CtdPickUpdate(noSums, V("0.1.0"), T, W) == nullptr);
}

static void TestSha256Sums()
{
	const std::string text =
		"E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855  CTD-OBS-Setup-0.10.1-x64.exe\n"
		"a665a45920422f9d417e4867efdc4fb8a04a1f3fff1fa07e998e86f7f7a27ae3 *ctd-obs-0.10.1-macos-arm64.zip\r\n"
		"not a line\n"
		"\n"
		"0123  short\n"
		"g3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  bad-hex.zip\n"
		"b5bb9d8014a0f9b1d61e21e796d78dccdf1352f23cd32812f4850b878ae4944c  ctd-obs 0.10.1 windows-x64.zip";
	auto sums = CtdParseSha256Sums(text);
	CHECK(sums.size() == 3);
	CHECK(sums["CTD-OBS-Setup-0.10.1-x64.exe"] ==
	      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(sums["ctd-obs-0.10.1-macos-arm64.zip"] ==
	      "a665a45920422f9d417e4867efdc4fb8a04a1f3fff1fa07e998e86f7f7a27ae3");
	CHECK(sums.count("ctd-obs 0.10.1 windows-x64.zip") == 1);
	CHECK(sums.count("bad-hex.zip") == 0);
	CHECK(CtdParseSha256Sums("").empty());
}

int main()
{
	TestParseVersion();
	TestCompare();
	TestParseReleases();
	TestPickUpdate();
	TestSha256Sums();
	std::printf("%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
