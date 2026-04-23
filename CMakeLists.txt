cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(my_blink C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

option(RUN_FS_TESTS "Run filesystem tests on boot" ON)

set(PROJECT_SOURCES
  main.c
  flash_ops.c
  filesystem.c
)

if(RUN_FS_TESTS)
  list(APPEND PROJECT_SOURCES fs_tests.c)
endif()

add_executable(my_blink
  ${PROJECT_SOURCES}
)

pico_enable_stdio_usb(my_blink 1)
pico_enable_stdio_uart(my_blink 0)

pico_add_extra_outputs(my_blink)

if(RUN_FS_TESTS)
  target_compile_definitions(my_blink PRIVATE RUN_FS_TESTS=1 FS_ENABLE_TEST_HOOKS=1)
endif()

target_link_libraries(my_blink pico_stdlib hardware_flash hardware_sync)
