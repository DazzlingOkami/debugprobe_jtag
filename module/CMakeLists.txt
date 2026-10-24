# USB
target_sources(
    ${CMAKE_PROJECT_NAME} PRIVATE
    usb/usb_descriptors.c
    usb/usb.c
)

# dap port
target_sources(
    ${CMAKE_PROJECT_NAME} PRIVATE
    dap/sw_dp_pio.c
    dap/jtag_dp_pio.c
    dap/dap_vendor.c
    dap/tusb_edpt_handler.c
)
target_include_directories(
    ${CMAKE_PROJECT_NAME} PRIVATE
    dap/
)

# utils
target_sources(
    ${CMAKE_PROJECT_NAME} PRIVATE
    utils/get_serial.c
    utils/itohn.c
    utils/ringbuf.c
)

target_include_directories(
    ${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/
    utils/
)
