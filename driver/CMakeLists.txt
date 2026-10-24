file(
    GLOB DRIVERS_FILE_LIST
    ${CMAKE_CURRENT_LIST_DIR}/*/*.c
)
target_sources(
    ${CMAKE_PROJECT_NAME} PRIVATE
    ${DRIVERS_FILE_LIST}
)
target_include_directories(
    ${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/
)

pico_generate_pio_header(${CMAKE_PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/swd/probe_oen.pio)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    hardware_pio
    hardware_dma
    hardware_i2c
    hardware_pwm
    hardware_adc
    hardware_spi
)