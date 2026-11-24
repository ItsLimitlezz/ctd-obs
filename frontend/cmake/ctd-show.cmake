# CTD OBS: Show Profiles core API (C1) and switch guard (C2)
target_sources(obs-studio PRIVATE ctd/ctd-show-api.cpp)

# CTD OBS: the top-level CTD menu (gaps 3, 19)
target_sources(obs-studio PRIVATE ctd/ctd-menu.cpp)

# CTD OBS: plugins installed for OBS Studio, which CTD OBS doesn't load (docs/ctd/plugins.md); the logic has no Qt or
# OBS dependency and is unit-tested in test/ctd-plugins
target_sources(
  obs-studio
  PRIVATE ctd/ctd-plugins.cpp ctd/ctd-plugin-isolation-logic.cpp ctd/ctd-plugin-isolation-logic.hpp
)

# CTD OBS: the Settings ▸ CTD page (gaps 7, 19)
target_sources(obs-studio PRIVATE ctd/ctd-settings-page.cpp)
