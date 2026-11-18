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

/* "0.10.0", or "0.10.0 (5653d30)" for a test build that isn't on a tag */
static inline std::string CtdVersionString()
{
	std::string version = CTD_OBS_VERSION;
	if (*CTD_OBS_BUILD) {
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
