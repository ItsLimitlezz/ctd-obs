# CTD OBS: the CTD OBS version, from CTD-VERSION at the repository root (Major.Minor.Patch, per the hub's
# CONVENTIONS.md; a Major bump is Juan's call). Shown in the title bar, About, the log and the Windows file details.
# OBS_VERSION stays OBS's own version (libobs, plugins and the log still report it).
# file(STRINGS) skips a UTF-8 BOM (Notepad adds one); a changed CTD-VERSION re-runs the configure step
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/CTD-VERSION")
file(STRINGS "${CMAKE_SOURCE_DIR}/CTD-VERSION" _ctd_version_file LIMIT_COUNT 1)
string(STRIP "${_ctd_version_file}" _ctd_version_file)
if(NOT _ctd_version_file MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)$")
  message(FATAL_ERROR "CTD-VERSION must hold one line <MAJOR>.<MINOR>.<PATCH>, not '${_ctd_version_file}'")
endif()
set(CTD_OBS_VERSION "${CMAKE_MATCH_1}")
unset(_ctd_version_file)

# Test builds also name their commit: git describe gives 33.0.1-18-g5653d30 (or ...-modified)
if(OBS_VERSION MATCHES "-g([0-9a-f]+)")
  set(CTD_OBS_BUILD "${CMAKE_MATCH_1}")
else()
  set(CTD_OBS_BUILD "")
endif()

# Release channel and test build number, set by CI (.github/workflows/ctd-build.yaml) from the environment, so the update
# check (frontend/ctd/ctd-update.cpp, docs/ctd/updates.md) knows what it is: "stable" (a ctd-v<X.Y.Z> tag), "test" with
# CTD_OBS_TEST_NUMBER (the CI run number: release ctd-v<X.Y.Z>-test.<N>), or empty for a local build
set(CTD_OBS_CHANNEL "$ENV{CTD_OBS_CHANNEL}")
set(CTD_OBS_TEST_NUMBER "$ENV{CTD_OBS_TEST_NUMBER}")
if(NOT CTD_OBS_CHANNEL MATCHES "^(stable|test|)$")
  message(FATAL_ERROR "CTD_OBS_CHANNEL must be stable, test or empty, not '${CTD_OBS_CHANNEL}'")
endif()
if(CTD_OBS_CHANNEL STREQUAL "test")
  if(NOT CTD_OBS_TEST_NUMBER MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "A test build needs CTD_OBS_TEST_NUMBER (a positive number), not '${CTD_OBS_TEST_NUMBER}'")
  endif()
else()
  set(CTD_OBS_TEST_NUMBER 0)
endif()

message(
  STATUS
  "CTD OBS ${CTD_OBS_VERSION} ${CTD_OBS_BUILD} (based on OBS ${OBS_VERSION}), channel '${CTD_OBS_CHANNEL}' test ${CTD_OBS_TEST_NUMBER}"
)

target_compile_definitions(
  obs-studio
  PRIVATE CTD_OBS_VERSION="${CTD_OBS_VERSION}" CTD_OBS_BUILD="${CTD_OBS_BUILD}"
          CTD_OBS_BASE_VERSION="${OBS_VERSION_CANONICAL}" CTD_OBS_CHANNEL="${CTD_OBS_CHANNEL}"
          CTD_OBS_TEST_NUMBER=${CTD_OBS_TEST_NUMBER}
)
target_sources(obs-studio PRIVATE ctd/ctd-version.hpp)
