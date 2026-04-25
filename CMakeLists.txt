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
  src/core
  src/sync
  src/network
  src/convert
  src/ui
  tools/vita-mcr2vmp/include
)

add_executable(${PROJECT_NAME}
  src/main.c
  src/core/app_config.c
  src/core/app_log.c
  src/core/backup_manager.c
  src/core/sfo_parser.c
  src/sync/conflict_resolver.c
  src/sync/game_matcher.c
  src/sync/save_scanner.c
  src/sync/vita_native_save_scanner.c
  src/sync/vita_native_save_policy.c
  src/sync/scan_to_sync_adapter.c
  src/sync/sync_engine.c
  src/sync/sync_state_store.c
  src/sync/sync_types.c
  src/network/romm_client.c
  src/network/romm_http_client.c
  src/convert/vmp_signer.c
  src/convert/vmp_srm_converter.c
  src/ui/ui_common.c
  src/ui/ui_render.c
  src/ui/ui_navigation.c
  src/ui/ui_screens.c
  src/ui/ui_sync_modal.c
  src/ui/ui_sync_orchestrator.c
  src/ui/ui_config_editor.c
  src/ui/ui_dialogs.c
  src/ui/ui_inventory.c
  tools/vita-mcr2vmp/src/aes.c
  tools/vita-mcr2vmp/src/sha1.c
  common/debugScreen.c
)

set_source_files_properties(
  tools/vita-mcr2vmp/src/sha1.c
  PROPERTIES
    COMPILE_DEFINITIONS "LITTLE_ENDIAN=1234;BIG_ENDIAN=4321;BYTE_ORDER=LITTLE_ENDIAN"
)

target_link_libraries(${PROJECT_NAME}
  vita2d
  SceAppUtil_stub
  SceIme_stub
  SceLibKernel_stub
  SceAppMgr_stub
  SceCommonDialog_stub
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  SceIofilemgr_stub
  SceSysmodule_stub
  SceGxm_stub
  ScePgf_stub
  SceKernelDmacMgr_stub
  SceShaccCg_stub
  SceNet_stub
  SceNetCtl_stub
  SceHttp_stub
  SceSsl_stub
  m
  z
  jpeg
  png
  freetype
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
  FILE samples/vmp-templates/SCEVMC0.VMP templates/SCEVMC0.VMP
  FILE samples/vmp-templates/SCEVMC1.VMP templates/SCEVMC1.VMP
)
