# FreeRTOS
set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/freertos)
include(FreeRTOS_Kernel_import.cmake)

# CMSIS-DAP
# The upstream DAP.c quantizes DAP_SWJ_Clock into clock_delay. Generate a patched copy in the
# build tree so Set_Clock_Delay() forwards the requested Hz to the PIO divider, leaving the
# submodule untouched. The patch has no "diff --git" header on purpose: with it, git apply
# treats lib/CMSIS-DAP paths as a submodule and skips them.
set(DAP_SOURCE ${CMAKE_CURRENT_LIST_DIR}/CMSIS-DAP/Firmware/Source/DAP.c)
set(DAP_PATCH ${CMAKE_SOURCE_DIR}/patches/dap_swj_clock_hz.patch)
set(DAP_PATCH_SCRIPT ${CMAKE_SOURCE_DIR}/patches/apply_patch.cmake)
set(DAP_WORKDIR ${CMAKE_BINARY_DIR}/generated)
set(DAP_PATCHED ${DAP_WORKDIR}/lib/CMSIS-DAP/Firmware/Source/DAP.c)
find_package(Git REQUIRED)
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE=${DAP_SOURCE}"
            "-DPATCH=${DAP_PATCH}"
            "-DOUTPUT=${DAP_PATCHED}"
            "-DWORKDIR=${DAP_WORKDIR}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            -P "${DAP_PATCH_SCRIPT}"
    RESULT_VARIABLE DAP_PATCH_RESULT
)
if(NOT DAP_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate the patched DAP.c")
endif()
# Re-run configure when the upstream file or the patch changes
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${DAP_SOURCE}" "${DAP_PATCH}" "${DAP_PATCH_SCRIPT}"
)

add_library(cmsis_dap INTERFACE)
target_sources(
    cmsis_dap INTERFACE
    "${DAP_PATCHED}"
    #CMSIS-DAP/Firmware/Source/DAP_vendor.c # provided by module/dap/dap_vendor.c
    CMSIS-DAP/Firmware/Source/SWO.c
    #CMSIS-DAP/Firmware/Source/SW_DP.c   # provided by module/dap/sw_dp_pio.c
    #CMSIS-DAP/Firmware/Source/JTAG_DP.c # provided by module/dap/jtag_dp_pio.c
)
# These sources build in the root target, so properties must be set on the root scope
set_property(SOURCE "${DAP_PATCHED}" DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY COMPILE_OPTIONS -Wno-unused-variable)
target_include_directories(
    cmsis_dap INTERFACE
    CMSIS-DAP/Firmware/Include/
    CMSIS_6/CMSIS/Core/Include/
)

# Add all libraries
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
    cmsis_dap
)

# Add pico libraries
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    pico_multicore
    pico_stdlib
    pico_unique_id
    tinyusb_device
    tinyusb_board
)
