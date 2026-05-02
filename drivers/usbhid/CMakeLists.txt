add_library(usbhid INTERFACE)

target_sources(usbhid INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/hid_app.c
    ${CMAKE_CURRENT_LIST_DIR}/xinput_host.c
    ${CMAKE_CURRENT_LIST_DIR}/gamepad_cal.c
    # HID Report Item Parser (RIP) from fruit-bat/pico-hid-host (MIT).
    # Used as a fallback when tinyusb's stock tuh_hid_parse_report_descriptor
    # fails to find any reports in a descriptor.
    ${CMAKE_CURRENT_LIST_DIR}/hid_ri.c
    ${CMAKE_CURRENT_LIST_DIR}/hid_rip.c
)

target_include_directories(usbhid INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(usbhid INTERFACE
    USB_HID_ENABLED=1
)

# fatfs is needed by gamepad_cal.c for SD persistence of learned profiles.
target_link_libraries(usbhid INTERFACE
    tinyusb_host
    tinyusb_board
    fatfs
)
