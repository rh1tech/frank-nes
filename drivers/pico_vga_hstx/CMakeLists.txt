add_library(pico_vga_hstx INTERFACE)

target_sources(pico_vga_hstx INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/video_output.c
)

target_include_directories(pico_vga_hstx INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/..
)

target_link_libraries(pico_vga_hstx INTERFACE
    pico_stdlib
    hardware_clocks
    hardware_dma
    hardware_gpio
    hardware_irq
    disphstx
)
