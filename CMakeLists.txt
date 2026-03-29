cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(romm_vita_sync)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "RomM Vita Sync")
set(VITA_TITLEID  "ROMM00001")
set(VITA_VERSION  "00.01")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2")

include_directories(
  common
  src
)

add_executable(${PROJECT_NAME}
  src/main.c
  src/backup_manager.c
  src/conflict_resolver.c
  src/game_matcher.c
  src/romm_client.c
  src/scan_to_sync_adapter.c
  src/save_scanner.c
  src/sfo_parser.c
  src/sync_engine.c
  src/sync_state_store.c
  src/sync_types.c
  src/ui_inventory.c
  src/vmp_srm_converter.c
  common/debugScreen.c
)

target_link_libraries(${PROJECT_NAME}
  SceLibKernel_stub
  SceDisplay_stub
  SceIofilemgr_stub
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME}
  UNSAFE
)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
