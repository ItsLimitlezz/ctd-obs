# CTD OBS: NDI in the app (docs/ctd/ndi.md): Settings ▸ CTD ▸ NDI (in ctd-settings-page.cpp), CTD ▸ NDI, --ctd-no-ndi and
# the memory watchdog. The NDI work itself is the core module plugins/ctd-ndi. ctd-memory-watchdog-logic.cpp has no Qt or
# OBS dependency; test/ctd-ndi unit-tests it.
target_sources(
  obs-studio
  PRIVATE
    ctd/ctd-memory-watchdog-logic.cpp
    ctd/ctd-memory-watchdog-logic.hpp
    ctd/ctd-memory-watchdog.cpp
    ctd/ctd-memory-watchdog.hpp
    ctd/ctd-ndi.cpp
    ctd/ctd-ndi.hpp
)
# the watchdog reads the process's video memory through DXGI
target_link_libraries(obs-studio PRIVATE $<$<PLATFORM_ID:Windows>:dxgi>)
