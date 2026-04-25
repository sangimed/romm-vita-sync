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

add_executable(sync_platform_tests
  sync_platform_tests.c
  ../src/sync/sync_types.c
)

target_include_directories(sync_platform_tests PRIVATE
  ../src
  ../src/core
  ../src/sync
)

if(MSVC)
  target_compile_options(sync_platform_tests PRIVATE /W4 /O2)
else()
  target_compile_options(sync_platform_tests PRIVATE -Wall -Wextra -O2)
endif()

add_executable(vita_native_save_policy_tests
  vita_native_save_policy_tests.c
  ../src/sync/vita_native_save_policy.c
)

target_include_directories(vita_native_save_policy_tests PRIVATE
  ../src
  ../src/core
  ../src/sync
)

if(MSVC)
  target_compile_options(vita_native_save_policy_tests PRIVATE /W4 /O2)
else()
  target_compile_options(vita_native_save_policy_tests PRIVATE -Wall -Wextra -O2)
endif()

add_test(NAME game_matcher_tests COMMAND game_matcher_tests)
add_test(NAME sync_platform_tests COMMAND sync_platform_tests)
add_test(NAME vita_native_save_policy_tests COMMAND vita_native_save_policy_tests)
