# CTD OBS: the CTD OBS version, from CTD-VERSION at the repository root (Major.Minor.Patch, per the hub's
# CONVENTIONS.md; a Major bump is Juan's call). Shown in the title bar, About, the log and the Windows file details.
# OBS_VERSION stays OBS's own version (libobs, plugins and the log still report it).
file(READ "${CMAKE_SOURCE_DIR}/CTD-VERSION" _ctd_version_file)
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

message(STATUS "CTD OBS ${CTD_OBS_VERSION} ${CTD_OBS_BUILD} (based on OBS ${OBS_VERSION})")

target_compile_definitions(
  obs-studio
  PRIVATE CTD_OBS_VERSION="${CTD_OBS_VERSION}" CTD_OBS_BUILD="${CTD_OBS_BUILD}"
          CTD_OBS_BASE_VERSION="${OBS_VERSION_CANONICAL}"
)
target_sources(obs-studio PRIVATE ctd/ctd-version.hpp)
