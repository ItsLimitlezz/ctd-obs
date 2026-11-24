/* CTD OBS: the CTD OBS version (CTD-VERSION at the repository root, see frontend/cmake/ctd-version.cmake). */

#pragma once

#include <string>

#ifndef CTD_OBS_VERSION
#define CTD_OBS_VERSION "0.0.0"
#endif
#ifndef CTD_OBS_BUILD
#define CTD_OBS_BUILD ""
#endif
#ifndef CTD_OBS_BASE_VERSION
#define CTD_OBS_BASE_VERSION ""
#endif
/* "stable" (built from a ctd-v<X.Y.Z> tag), "test" (a CI test build) or "" (a local build) */
#ifndef CTD_OBS_CHANNEL
#define CTD_OBS_CHANNEL ""
#endif
/* the test build number N of release ctd-v<X.Y.Z>-test.<N>, 0 unless CTD_OBS_CHANNEL is "test" */
#ifndef CTD_OBS_TEST_NUMBER
#define CTD_OBS_TEST_NUMBER 0
#endif

static inline bool CtdIsStableBuild()
{
	return std::string(CTD_OBS_CHANNEL) == "stable";
}

static inline bool CtdIsTestBuild()
{
	return std::string(CTD_OBS_CHANNEL) == "test" && CTD_OBS_TEST_NUMBER > 0;
}

/* "0.10.0", or "0.10.0-test.42" for test build 42: the version the update check compares */
static inline std::string CtdReleaseVersionString()
{
	std::string version = CTD_OBS_VERSION;
	if (CtdIsTestBuild()) {
		version += "-test." + std::to_string(CTD_OBS_TEST_NUMBER);
	}
	return version;
}

/* "0.10.0" for a stable release, "0.10.0-test.42 (5653d30)" for a test build, "0.10.0 (5653d30)" for a local build */
static inline std::string CtdVersionString()
{
	std::string version = CtdReleaseVersionString();
	if (*CTD_OBS_BUILD && !CtdIsStableBuild()) {
		version += " (";
		version += CTD_OBS_BUILD;
		version += ")";
	}
	return version;
}

/* The OBS Studio release CTD OBS is based on, e.g. "33.0.1" */
static inline const char *CtdBaseObsVersion()
{
	return CTD_OBS_BASE_VERSION;
}
