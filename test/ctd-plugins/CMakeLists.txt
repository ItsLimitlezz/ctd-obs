# CTD OBS: unit tests for finding and copying OBS Studio's third-party plugins (frontend/ctd/ctd-plugin-isolation-logic.cpp).
# A standalone project, so it builds without OBS's dependencies:
#   cmake -S test/ctd-plugins -B build_ctd_plugins_test && cmake --build build_ctd_plugins_test && ctest --test-dir build_ctd_plugins_test
cmake_minimum_required(VERSION 3.16)
project(ctd-plugins-test LANGUAGES CXX)

add_executable(ctd-plugins-test ctd-plugins-test.cpp ../../frontend/ctd/ctd-plugin-isolation-logic.cpp)
target_include_directories(ctd-plugins-test PRIVATE ../../frontend/ctd)
target_compile_features(ctd-plugins-test PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(ctd-plugins-test PRIVATE /W4 /WX)
else()
  target_compile_options(ctd-plugins-test PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME ctd-plugins-test COMMAND ctd-plugins-test)
