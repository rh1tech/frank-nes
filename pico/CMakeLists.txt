cmake_minimum_required(VERSION 3.13)

# RP2350 Raspberry Pi Pico 2
set(PICO_BOARD pico2 CACHE STRING "Board type")

include(${CMAKE_CURRENT_LIST_DIR}/../pico_sdk_import.cmake)

project(murmnes C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

#=============================================================================
# Source directories
#=============================================================================
set(PROJECT_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(DRIVERSDIR ${PROJECT_ROOT}/drivers)
set(PICODIR ${PROJECT_ROOT}/src/pico)
set(COREDIR ${PROJECT_ROOT}/src/core)
set(UTILDIR ${PROJECT_ROOT}/src/util)
set(INCCOREDIR ${PROJECT_ROOT}/include/core)
set(INCUTILDIR ${PROJECT_ROOT}/include/util)

#=============================================================================
# Libraries (same pattern as bouncing_box)
#=============================================================================
# sys_clk=252MHz, HSTX needs 126MHz → divider=2, pixel_clk=126/5=25.2MHz
add_compile_definitions(MODE_HSTX_CLK_DIV=2)
add_subdirectory(${DRIVERSDIR}/pico_hdmi pico_hdmi)
add_subdirectory(${DRIVERSDIR}/nespad nespad)

#=============================================================================
# ROM embedding
#=============================================================================
set(NES_ROM_PATH "" CACHE FILEPATH "Path to .nes ROM file to embed in flash")

set(ROM_SOURCES "")
if(NES_ROM_PATH AND EXISTS "${NES_ROM_PATH}")
    get_filename_component(ROM_FILENAME ${NES_ROM_PATH} NAME)
    message(STATUS "Embedding ROM: ${ROM_FILENAME}")

    file(READ "${NES_ROM_PATH}" ROM_HEX HEX)
    string(LENGTH "${ROM_HEX}" ROM_HEX_LEN)
    math(EXPR ROM_SIZE "${ROM_HEX_LEN} / 2")

    set(ROM_BYTES "")
    set(COL 0)
    math(EXPR LAST_IDX "${ROM_HEX_LEN} - 2")
    foreach(IDX RANGE 0 ${LAST_IDX} 2)
        string(SUBSTRING "${ROM_HEX}" ${IDX} 2 BYTE)
        if(COL EQUAL 0)
            string(APPEND ROM_BYTES "\n    ")
        endif()
        string(APPEND ROM_BYTES "0x${BYTE},")
        math(EXPR COL "(${COL} + 1) % 16")
    endforeach()

    file(WRITE "${CMAKE_BINARY_DIR}/embedded_rom.h"
        "#ifndef EMBEDDED_ROM_H\n#define EMBEDDED_ROM_H\n#include <stdint.h>\n"
        "extern const uint8_t nes_rom_data[];\nextern const uint32_t nes_rom_size;\n#endif\n")
    file(WRITE "${CMAKE_BINARY_DIR}/embedded_rom.c"
        "#include \"embedded_rom.h\"\n"
        "const uint8_t nes_rom_data[] = {${ROM_BYTES}\n};\n"
        "const uint32_t nes_rom_size = ${ROM_SIZE};\n")

    set(ROM_SOURCES ${CMAKE_BINARY_DIR}/embedded_rom.c)
    set(HAS_EMBEDDED_ROM ON)
else()
    set(HAS_EMBEDDED_ROM OFF)
endif()

#=============================================================================
# Executable
#=============================================================================
add_executable(murmnes
    ${PICODIR}/main_pico.c
    ${COREDIR}/cart.c
    ${COREDIR}/cpu.c
    ${COREDIR}/mappers.c
    ${COREDIR}/ppu.c
    ${COREDIR}/cpu_ppu_interface.c
    ${COREDIR}/cpu_mapper_interface.c
    ${UTILDIR}/bits_and_bytes.c
    ${ROM_SOURCES}
)

target_include_directories(murmnes PRIVATE
    ${INCCOREDIR}
    ${INCUTILDIR}
    ${PICODIR}
)

if(HAS_EMBEDDED_ROM)
    target_include_directories(murmnes PRIVATE ${CMAKE_BINARY_DIR})
    target_compile_definitions(murmnes PRIVATE HAS_EMBEDDED_ROM)
endif()

target_compile_definitions(murmnes PRIVATE
    PICO_BUILD
    PICO_CORE1_STACK_SIZE=0xC00
    NESPAD_GPIO_CLK=20
    NESPAD_GPIO_LATCH=21
    NESPAD_GPIO_DATA=26
)

# Aggressive optimization for emulator performance
target_compile_options(murmnes PRIVATE -O3)

target_link_libraries(murmnes
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_vreg
    pico_hdmi
    nespad
)

pico_enable_stdio_usb(murmnes 1)
pico_enable_stdio_uart(murmnes 0)

pico_add_extra_outputs(murmnes)
