cmake_minimum_required(VERSION 3.16)
project(romm_vita_sync_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

add_executable(game_matcher_tests
  game_matcher_tests.c
  ../src/sync/game_matcher.c
  ../src/sync/sync_types.c
  ../src/core/app_log.c
)

target_include_directories(game_matcher_tests PRIVATE
  ../src
  ../src/core
  ../src/sync
)

if(MSVC)
  target_compile_options(game_matcher_tests PRIVATE /W4 /O2)
else()
  target_compile_options(game_matcher_tests PRIVATE -Wall -Wextra -O2)
endif()

add_test(NAME game_matcher_tests COMMAND game_matcher_tests)
