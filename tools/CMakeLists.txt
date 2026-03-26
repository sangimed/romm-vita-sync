cmake_minimum_required(VERSION 3.16)
project(vmp2srm_cli C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(vmp2srm
  vmp2srm_cli.c
  ../src/vmp_srm_converter.c
)

target_include_directories(vmp2srm PRIVATE ../src)

if(MSVC)
  target_compile_options(vmp2srm PRIVATE /W4 /O2)
else()
  target_compile_options(vmp2srm PRIVATE -Wall -Wextra -O2)
endif()