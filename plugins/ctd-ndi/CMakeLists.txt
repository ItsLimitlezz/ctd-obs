cmake_minimum_required(VERSION 3.28...3.30)

# CTD OBS: NDI send in-process (program, vertical, any source), no system pieces (docs/ctd/ndi.md). The NDI library is loaded at run time
# (CTD OBS's own verified copy of the official NDI 6 Runtime's); nothing links against it. deps/ndi/include holds NDI's
# MIT-licensed headers.

# Off until Vizrt and CTD's legal call clear shipping the NDI library inside the GPL app (docs/ctd/ndi.md, "Licence
# finding"). ON: the loader takes only the library inside the app bundle / next to ctd-ndi.dll, which CI would then have
# to put there and sign.
option(CTD_NDI_BUNDLED "Load NDI only from a library shipped inside CTD OBS" OFF)

add_library(ctd-ndi MODULE)
add_library(OBS::ctd-ndi ALIAS ctd-ndi)

target_sources(
  ctd-ndi
  PRIVATE
    src/ndi-allowlist.cpp
    src/ndi-filter.cpp
    src/ndi-loader.cpp
    src/ndi-logic.cpp
    src/ndi-logic.hpp
    src/ndi-module.hpp
    src/ndi-output.cpp
    src/ndi-plugin.cpp
    src/ndi-sender.cpp
    src/ndi-sender.hpp
)
target_include_directories(ctd-ndi PRIVATE src)
target_include_directories(ctd-ndi SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/deps/ndi/include")
target_compile_features(ctd-ndi PRIVATE cxx_std_17)
target_compile_definitions(ctd-ndi PRIVATE $<$<BOOL:${CTD_NDI_BUNDLED}>:CTD_NDI_BUNDLED>)

target_link_libraries(
  ctd-ndi
  PRIVATE OBS::libobs OBS::frontend-api $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads> $<$<PLATFORM_ID:Linux>:${CMAKE_DL_LIBS}>
)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in ctd-ndi.rc)
  target_sources(ctd-ndi PRIVATE ctd-ndi.rc)
endif()

set_target_properties_obs(ctd-ndi PROPERTIES FOLDER plugins PREFIX "")
