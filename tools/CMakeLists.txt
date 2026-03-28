cmake_minimum_required(VERSION 3.16)
project(ps1_conversion_tools C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(vmp2srm
  vmp2srm_cli.c
  ../src/vmp_srm_converter.c
)

add_executable(srm2vmp
  srm2vmp_cli.c
  ../src/vmp_srm_converter.c
)

target_include_directories(vmp2srm PRIVATE ../src)
target_include_directories(srm2vmp PRIVATE ../src)

if(MSVC)
  target_compile_options(vmp2srm PRIVATE /W4 /O2)
  target_compile_options(srm2vmp PRIVATE /W4 /O2)
else()
  target_compile_options(vmp2srm PRIVATE -Wall -Wextra -O2)
  target_compile_options(srm2vmp PRIVATE -Wall -Wextra -O2)
endif()

set(VITA_MCR2VMP_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/vita-mcr2vmp/src)
set(VITA_MCR2VMP_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/vita-mcr2vmp/include)

if(NOT EXISTS ${VITA_MCR2VMP_SRC_DIR}/main.c)
  message(FATAL_ERROR "vita-mcr2vmp submodule missing. Run: git submodule update --init tools/vita-mcr2vmp")
endif()

add_executable(vita-mcr2vmp
  ${VITA_MCR2VMP_SRC_DIR}/main.c
  ${VITA_MCR2VMP_SRC_DIR}/aes.c
  ${VITA_MCR2VMP_SRC_DIR}/sha1.c
)
target_include_directories(vita-mcr2vmp PRIVATE ${VITA_MCR2VMP_INCLUDE_DIR})
if(MSVC)
  target_compile_options(vita-mcr2vmp PRIVATE /W4 /O2)
else()
  target_compile_options(vita-mcr2vmp PRIVATE -Wall -Wextra -O2)
endif()
