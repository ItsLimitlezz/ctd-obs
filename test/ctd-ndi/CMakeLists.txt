# CTD OBS: unit tests for the NDI module's logic (plugins/ctd-ndi/src/ndi-logic.cpp, ndi-allowlist.cpp) and the memory
# watchdog (frontend/ctd/ctd-memory-watchdog-logic.cpp); docs/ctd/ndi.md. A standalone project, so it builds without
# OBS's dependencies or the NDI SDK:
#   cmake -S test/ctd-ndi -B build_ctd_ndi_test && cmake --build build_ctd_ndi_test && ctest --test-dir build_ctd_ndi_test
cmake_minimum_required(VERSION 3.16)
project(ctd-ndi-test LANGUAGES CXX)

add_executable(
  ctd-ndi-test
  ctd-ndi-test.cpp
  ../../plugins/ctd-ndi/src/ndi-logic.cpp
  ../../plugins/ctd-ndi/src/ndi-allowlist.cpp
  ../../frontend/ctd/ctd-memory-watchdog-logic.cpp
)
target_include_directories(ctd-ndi-test PRIVATE ../../plugins/ctd-ndi/src ../../frontend/ctd)
target_compile_features(ctd-ndi-test PRIVATE cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(ctd-ndi-test PRIVATE Threads::Threads)
if(MSVC)
  target_compile_options(ctd-ndi-test PRIVATE /W4 /WX)
else()
  target_compile_options(ctd-ndi-test PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME ctd-ndi-test COMMAND ctd-ndi-test)
