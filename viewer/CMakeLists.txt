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

# Remastered graphics (F2) read a StarCraft: Remastered install's CASC store, which needs
# zlib; without zlib the player builds with the classic graphics only. Decoding the art
# uses a few threads.
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
  find_package(Threads REQUIRED)
  target_compile_definitions(openbw-player PRIVATE OPENBW_REMASTERED)
  target_link_libraries(openbw-player PRIVATE ZLIB::ZLIB Threads::Threads)
  message(STATUS "openbw-player: remastered graphics enabled (zlib)")

  # Behaviour tests for the remastered asset readers and raster operations.
  add_executable(openbw-scr-test EXCLUDE_FROM_ALL "${CMAKE_CURRENT_LIST_DIR}/tests/scr_test.cpp")
  target_include_directories(openbw-scr-test PRIVATE "${OPENBW_DIR}/ui")
  target_link_libraries(openbw-scr-test PRIVATE ZLIB::ZLIB Threads::Threads)
else()
  message(STATUS "openbw-player: zlib not found, building without remastered graphics")
endif()

# Behaviour tests for the seek-bar markers and the Pluto win estimate (pure, no game data needed).
add_executable(openbw-timeline-test EXCLUDE_FROM_ALL "${CMAKE_CURRENT_LIST_DIR}/tests/timeline_test.cpp")
target_include_directories(openbw-timeline-test PRIVATE "${OPENBW_DIR}/ui")
add_executable(openbw-pluto-win-test EXCLUDE_FROM_ALL "${CMAKE_CURRENT_LIST_DIR}/tests/pluto_win_test.cpp")
target_include_directories(openbw-pluto-win-test PRIVATE "${OPENBW_DIR}/ui")
add_executable(openbw-minimap-clumps-test EXCLUDE_FROM_ALL "${CMAKE_CURRENT_LIST_DIR}/tests/minimap_clumps_test.cpp")
target_include_directories(openbw-minimap-clumps-test PRIVATE "${OPENBW_DIR}/ui")
add_executable(openbw-ui-theme-test EXCLUDE_FROM_ALL "${CMAKE_CURRENT_LIST_DIR}/tests/ui_theme_test.cpp")
target_include_directories(openbw-ui-theme-test PRIVATE "${OPENBW_DIR}/ui")
add_executable(openbw-music-test EXCLUDE_FROM_ALL "${CMAKE_CURRENT_LIST_DIR}/tests/music_test.cpp")
target_include_directories(openbw-music-test PRIVATE "${OPENBW_DIR}/ui")
add_executable(openbw-replay-result-test EXCLUDE_FROM_ALL "${CMAKE_CURRENT_LIST_DIR}/tests/replay_result_test.cpp")
target_include_directories(openbw-replay-result-test PRIVATE "${OPENBW_DIR}/ui")
add_executable(openbw-supply-state-test EXCLUDE_FROM_ALL "${CMAKE_CURRENT_LIST_DIR}/tests/supply_state_test.cpp")
target_include_directories(openbw-supply-state-test PRIVATE "${OPENBW_DIR}/ui")
add_executable(openbw-modal-layout-test EXCLUDE_FROM_ALL "${CMAKE_CURRENT_LIST_DIR}/tests/modal_layout_test.cpp")
target_include_directories(openbw-modal-layout-test PRIVATE "${OPENBW_DIR}/ui")
add_executable(openbw-skin-frame-test EXCLUDE_FROM_ALL "${CMAKE_CURRENT_LIST_DIR}/tests/skin_frame_test.cpp")
target_include_directories(openbw-skin-frame-test PRIVATE "${OPENBW_DIR}/ui")

# Sound is optional: uses SDL2_mixer when installed, otherwise builds silent.
find_package(SDL2_mixer CONFIG QUIET)
if(SDL2_mixer_FOUND)
  target_link_libraries(openbw-player PRIVATE SDL2_mixer::SDL2_mixer)
  message(STATUS "openbw-player: sound enabled (SDL2_mixer)")
else()
  target_compile_definitions(openbw-player PRIVATE OPENBW_NO_SDL_MIXER)
  message(STATUS "openbw-player: SDL2_mixer not found, building without sound")
endif()

# Headless auto-observer evaluation (see observer-eval/); not built by default.
add_executable(openbw-observer-eval EXCLUDE_FROM_ALL
  "${CMAKE_CURRENT_LIST_DIR}/observer-eval/observer_eval.cpp"
  "${OPENBW_DIR}/ui/sdl2.cpp"
)
target_include_directories(openbw-observer-eval PRIVATE
  "${OPENBW_DIR}"
  "${OPENBW_DIR}/ui"
  ${SDL2_INCLUDE_DIRS}
)
target_compile_definitions(openbw-observer-eval PRIVATE OPENBW_ENABLE_UI OPENBW_NO_SDL_IMAGE OPENBW_NO_SDL_MIXER)
target_link_libraries(openbw-observer-eval PRIVATE ${SDL2_LIBRARIES})
