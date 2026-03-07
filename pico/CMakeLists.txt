cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")

include(${CMAKE_CURRENT_LIST_DIR}/../pico_sdk_import.cmake)

project(murmnes C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

set(PROJECT_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(DRIVERSDIR ${PROJECT_ROOT}/drivers)
set(PICODIR ${PROJECT_ROOT}/src/pico)

# sys_clk=252MHz, HSTX needs 126MHz → divider=2
add_compile_definitions(MODE_HSTX_CLK_DIV=2)
add_subdirectory(${DRIVERSDIR}/pico_hdmi pico_hdmi)

add_executable(murmnes
    ${PICODIR}/main_pico.c
)

target_include_directories(murmnes PRIVATE ${PICODIR})

target_compile_definitions(murmnes PRIVATE
    PICO_CORE1_STACK_SIZE=0xC00
)

target_compile_options(murmnes PRIVATE -O3)

target_link_libraries(murmnes
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_irq
    pico_hdmi
)

pico_enable_stdio_usb(murmnes 1)
pico_enable_stdio_uart(murmnes 0)

pico_add_extra_outputs(murmnes)
