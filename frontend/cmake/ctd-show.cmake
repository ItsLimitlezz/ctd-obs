# CTD OBS: Show Profiles core API (C1) and switch guard (C2)
target_sources(obs-studio PRIVATE ctd/ctd-show-api.cpp)

# CTD OBS: the top-level CTD menu (gaps 3, 19)
target_sources(obs-studio PRIVATE ctd/ctd-menu.cpp)

# CTD OBS: the Settings ▸ CTD page (gaps 7, 19)
target_sources(obs-studio PRIVATE ctd/ctd-settings-page.cpp)
