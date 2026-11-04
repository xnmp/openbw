# Native SDL2 replay viewer for OpenBW (Linux/macOS/Windows with SDL2).
#
#   cmake -S viewer -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#   ./build/openbw-player <replay.rep> <dir-with-StarDat.mpq-BrooDat.mpq-patch_rt.mpq>
cmake_minimum_required(VERSION 3.10)
project(openbw_player CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPENBW_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
list(APPEND CMAKE_MODULE_PATH "${OPENBW_DIR}/cmake")

find_package(SDL2 REQUIRED)
# SDL2's CMake config exports an imported target on some distros and plain
# variables on others; accept whichever is present.
if(NOT SDL2_LIBRARIES)
  set(SDL2_LIBRARIES SDL2::SDL2)
endif()

add_executable(openbw-player
  "${OPENBW_DIR}/ui/gfxtest.cpp"
  "${OPENBW_DIR}/ui/sdl2.cpp"
)
target_include_directories(openbw-player PRIVATE
  "${OPENBW_DIR}"
  "${OPENBW_DIR}/ui"
  ${SDL2_INCLUDE_DIRS}
)
# SDL_image is only used for screenshot export and stays off.
target_compile_definitions(openbw-player PRIVATE OPENBW_ENABLE_UI OPENBW_NO_SDL_IMAGE)
target_link_libraries(openbw-player PRIVATE ${SDL2_LIBRARIES})

# Sound is optional: uses SDL2_mixer when installed, otherwise builds silent.
find_package(SDL2_mixer CONFIG QUIET)
if(SDL2_mixer_FOUND)
  target_link_libraries(openbw-player PRIVATE SDL2_mixer::SDL2_mixer)
  message(STATUS "openbw-player: sound enabled (SDL2_mixer)")
else()
  target_compile_definitions(openbw-player PRIVATE OPENBW_NO_SDL_MIXER)
  message(STATUS "openbw-player: SDL2_mixer not found, building without sound")
endif()
