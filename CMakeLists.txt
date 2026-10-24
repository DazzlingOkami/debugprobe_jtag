cmake_minimum_required(VERSION 3.22)

# Setup compiler settings
# set(CMAKE_C_STANDARD 11)
# set(CMAKE_C_STANDARD_REQUIRED ON)
# set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_C_COMPILER   "arm-none-eabi-gcc")
set(CMAKE_CXX_COMPILER "arm-none-eabi-g++")
set(CMAKE_ASM_COMPILER "arm-none-eabi-gcc")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Enable CMake support for ASM and C languages
# enable_language(C ASM)

# Set the project name
set(CMAKE_PROJECT_NAME daplink)

# Pico SDK
set(PICO_BOARD dap_board)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/config)
set(PICO_PLATFORM "rp2350-arm-s")
set(PICO_FLASH_SIZE_BYTES "2 * 1024 * 1024")
# Disable coprocessor disassembly
set(PICO_NO_COPRO_DIS 1)
include(bsp/pico_sdk_import.cmake)
IF (CMAKE_HOST_WIN32)
    set(picotool_DIR ${CMAKE_CURRENT_LIST_DIR}/tools/picotool/lib/cmake/picotool)
ENDIF ()

# Core project settings
project(${CMAKE_PROJECT_NAME})

# Pico SDK init
pico_sdk_init()
if (${PICO_SDK_VERSION_MAJOR} LESS 2)
    message(SEND_ERROR
        "Version 2 of the Pico SDK is required to compile this project.\n"
        "Please update your installation at ${PICO_SDK_PATH}\n"
    )
endif ()

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Config
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE config/)

# Add third library
add_subdirectory(lib)

# Add drivers
add_subdirectory(driver)

# Add modules
add_subdirectory(module)

# Add APP
add_subdirectory(app)

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -Wall)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -Wl,--print-memory-usage)

# Create target
pico_add_extra_outputs(${CMAKE_PROJECT_NAME})
