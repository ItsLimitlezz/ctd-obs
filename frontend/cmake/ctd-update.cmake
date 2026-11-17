# CTD OBS: the update check against GitHub Releases (docs/ctd/updates.md). ctd-update-logic.cpp has no Qt or OBS
# dependency; test/ctd-update unit-tests it.
find_package(nlohmann_json 3.11 REQUIRED)

target_sources(
  obs-studio
  PRIVATE ctd/ctd-update.cpp ctd/ctd-update-logic.cpp ctd/ctd-update-logic.hpp
)
target_link_libraries(obs-studio PRIVATE nlohmann_json::nlohmann_json)
