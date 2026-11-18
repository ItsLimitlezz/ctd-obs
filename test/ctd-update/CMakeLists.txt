# CTD OBS: unit tests for the update check logic (frontend/ctd/ctd-update-logic.cpp). A standalone project, so it
# builds without OBS's dependencies:
#   cmake -S test/ctd-update -B build_ctd_update_test && cmake --build build_ctd_update_test && ctest --test-dir build_ctd_update_test
cmake_minimum_required(VERSION 3.16)
project(ctd-update-test LANGUAGES CXX)

find_package(nlohmann_json 3.11 REQUIRED)

add_executable(ctd-update-test ctd-update-test.cpp ../../frontend/ctd/ctd-update-logic.cpp)
target_include_directories(ctd-update-test PRIVATE ../../frontend/ctd)
target_compile_features(ctd-update-test PRIVATE cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(ctd-update-test PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
if(MSVC)
  target_compile_options(ctd-update-test PRIVATE /W4 /WX)
else()
  target_compile_options(ctd-update-test PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME ctd-update-test COMMAND ctd-update-test)
