cmake_minimum_required(VERSION 3.25)

# Set project data
set(PROJECT_NAME "OpenAnnealerController")
set(TARGET_NAME "app")

message("CMAKE_SOURCE_DIR: ${CMAKE_SOURCE_DIR}")

# Set env variable 'PICO_SDK_PATH' to the local Pico SDK
# Comment out the set() if you have a global copy of the
# SDK set and $PICO_SDK_PATH defined in your $PATH
set(ENV{PICO_SDK_PATH} "${CMAKE_SOURCE_DIR}/library/pico-sdk")
set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})

# Include the Pico SDK
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

# Name the project
project(${PROJECT_NAME}
        LANGUAGES C CXX ASM
        DESCRIPTION "Firmware for OpenAnnealer Controller"
)

# Initialise the Pico SDK
pico_sdk_init()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Set flags and directory variables
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -DDEBUG")
set(SRC_DIRECTORY "${CMAKE_SOURCE_DIR}/src")
set(FREERTOS_SRC_DIRECTORY "${CMAKE_SOURCE_DIR}/library/FreeRTOS-Kernel")
set(U8G2_SRC_DIRECYTORY "${CMAKE_SOURCE_DIR}/library/u8g2")
set(U8G2_MUI_DIRECTORY "${U8G2_SRC_DIRECYTORY}/csrc")
set(TMC_SRC_DIRECTORY "${CMAKE_SOURCE_DIR}/library/Trinamic-library")
set(SCRIPTS_DIRECTORY "${CMAKE_SOURCE_DIR}/scripts")
set(PICO_BOARD_HEADER_DIRS "${CMAKE_SOURCE_DIR}/targets")

# Append compiler flags
SET(CMAKE_C_FLAGS  "${CMAKE_C_FLAGS} -fdata-sections -ffunction-sections -Wall")
SET(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -fdata-sections -ffunction-sections -Wall")
# SET(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")

# OTA (Milestone 3): the app is built as a TBYB-flagged, versioned image so it
# can be flashed into either half of the A/B partition table (targets/partition_table.json)
# and safely rolled back by the RP2350 bootrom if an update never confirms itself.
# The TBYB flag and the version stamp are both baked in at compile time (there is no
# post-build way to add them) - see project memory "project-ota-tbyb-findings" for why
# this is easy to silently get wrong.
set(PICOTOOL_EXECUTABLE "picotool" CACHE STRING "picotool executable used to build the OTA partition table")

# Derive a numeric major/minor version from the nearest "annealer-vX.Y" git tag for
# the picobin image version block. Uses its own "annealer-v" prefix, not plain "vX.Y",
# because this repo inherited OpenTrickler's old release tags (v0.1..v1.20) from
# before the fork - reusing that numbering would either collide outright or make an
# OpenAnnealer pre-release look like a continuation of OpenTrickler's version history.
# --match also makes `git describe` itself ignore those old tags when picking the
# nearest one, not just the regex below.
#
# The version scheme is intentionally coarse - the picobin version item only stores
# major/minor, no patch/build number - so only builds cut from a bumped tag actually
# count as a newer OTA version. Untagged/dirty dev builds keep reusing the same tag's
# major.minor, which is fine since OTA is for tagged releases, not the physical-reflash
# dev-iteration loop used day to day.
execute_process(
    COMMAND git describe --tags --long --always --match "annealer-v*"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_DESCRIBE_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(GIT_DESCRIBE_OUTPUT MATCHES "^annealer-v([0-9]+)\\.([0-9]+)-")
    set(APP_FW_VERSION_MAJOR ${CMAKE_MATCH_1})
    set(APP_FW_VERSION_MINOR ${CMAKE_MATCH_2})
else()
    message(WARNING "Could not parse an annealer-vX.Y git tag from '${GIT_DESCRIBE_OUTPUT}' - falling back to version 0.0 for the OTA image version. Untagged builds will not be treated as newer than a previous OTA update.")
    set(APP_FW_VERSION_MAJOR 0)
    set(APP_FW_VERSION_MINOR 0)
endif()
message("OTA image version (from git tag): ${APP_FW_VERSION_MAJOR}.${APP_FW_VERSION_MINOR}")


# Application
add_executable("${TARGET_NAME}")

# Include source
include_directories(${SRC_DIRECTORY})

# Pull in FreeRTOS
include(${FREERTOS_SRC_DIRECTORY}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake)

# Pull in u8g2
add_subdirectory(${U8G2_SRC_DIRECYTORY})

# Pull in Trinamic library (we don't use CMakeLists provided by TMC library as I don't want hal stuff)
add_library(trinamic INTERFACE)
target_sources(trinamic INTERFACE
    ${TMC_SRC_DIRECTORY}/common.c
    ${TMC_SRC_DIRECTORY}/tmc_interface.c
    ${TMC_SRC_DIRECTORY}/tmc2130.c
    ${TMC_SRC_DIRECTORY}/tmc2209.c
    ${TMC_SRC_DIRECTORY}/tmc5160.c
)
target_include_directories(trinamic INTERFACE ${TMC_SRC_DIRECTORY})



# Pull in u8g2 mui
add_library(u8g2_mui
    ${U8G2_MUI_DIRECTORY}/mui_u8g2.c
    ${U8G2_MUI_DIRECTORY}/mui.c
    ${U8G2_MUI_DIRECTORY}/u8g2_button.c
    ${U8G2_MUI_DIRECTORY}/u8g2_box.c
)
target_link_libraries(u8g2_mui u8g2)

# Pull in src
add_subdirectory(${SRC_DIRECTORY})

# Collect all source files
file(GLOB SRC ${SRC_DIRECTORY}/*.c 
              ${SRC_DIRECTORY}/*.cpp)

# Include application source file
target_sources("${TARGET_NAME}" PUBLIC
    ${SRC}
)

# Include additional headers
target_include_directories("${TARGET_NAME}" PUBLIC ${CMAKE_SOURCE_DIR}/targets)

# Include libraries1
target_link_libraries("${TARGET_NAME}"
    pico_stdlib
    hardware_pio
    hardware_spi
    hardware_i2c
    hardware_pwm
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
    u8g2
    u8g2_mui
    trinamic
    app_version

    # OTA (Milestone 3): pico_bootrom for rom_reboot()/rom_explicit_buy()/
    # rom_get_boot_info(); hardware_flash + pico_flash for multicore-safe erase/
    # program of the inactive A/B partition - see src/ota_update.c.
    pico_bootrom
    hardware_flash
    pico_flash

    # Required by cyw43 wireless module
    pico_cyw43_arch_lwip_sys_freertos
    pico_lwip_freertos
    pico_lwip_mdns
    # pico_lwip_http
)

target_link_options("${TARGET_NAME}" PUBLIC -Wl,--gc-sections -Wl,--print-memory-usage)
# set( CMAKE_VERBOSE_MAKEFILE on )

# Opt into TBYB (Try Before You Buy) so a flash-update boot into this image is
# tentative until it calls rom_explicit_buy(), and stamp the git-tag-derived
# version so the bootrom can tell an update apart from a downgrade. Both are
# required for the A/B partition table to actually provide rollback safety -
# see the comment above where APP_FW_VERSION_MAJOR/MINOR are computed.
#
# OFF by default: a TBYB-flagged image that's never been "bought" (confirmed via
# rom_explicit_buy(), which only happens via the real A/B flash-update flow) is
# refused by the bootrom on a normal boot when there's no partition table / no other
# partition to fall back to - confirmed the hard way when a plain BOOTSEL+`picotool
# load -x` dev-reflash (this project's everyday workflow since Milestone 1, no
# partition table involved) left the board stuck back in BOOTSEL instead of running.
# Only enable this (-DOTA_TBYB_BUILD=ON) when building an image that will actually go
# through the OTA path - either uploaded via /ota_upload, or deliberately flashed into
# a partition of an already-partitioned, OTA-converted device.
option(OTA_TBYB_BUILD "Build with TBYB + version stamp for OTA deployment - breaks plain single-image BOOTSEL dev reflash, only use for an image going through the real A/B OTA path" OFF)
if(OTA_TBYB_BUILD)
    message("OTA_TBYB_BUILD=ON: building a TBYB-flagged, versioned image (${APP_FW_VERSION_MAJOR}.${APP_FW_VERSION_MINOR}) - this will NOT run from a plain BOOTSEL dev-reflash without a partition table")
    target_compile_definitions("${TARGET_NAME}" PRIVATE
        PICO_CRT0_IMAGE_TYPE_TBYB=1
        PICO_CRT0_VERSION_MAJOR=${APP_FW_VERSION_MAJOR}
        PICO_CRT0_VERSION_MINOR=${APP_FW_VERSION_MINOR}
    )
endif()

# Optional USB CDC serial console for debugging - the real app has none by default
# (see README/system design), so this exists purely to diagnose boot issues without
# changing normal builds. Must explicitly disable UART too, or pico-sdk links both
# (UART is on by default project-wide) and CDC output can go missing in practice -
# found the hard way debugging an OTA/partition boot issue where enabling USB alone
# reported "UART stdin/stdout" in `picotool info`, not USB.
option(OTA_DEBUG_SERIAL "Enable a USB CDC serial console on the app target for debugging (disables the default UART stdio)" OFF)
if(OTA_DEBUG_SERIAL)
    message("OTA_DEBUG_SERIAL=ON: enabling USB CDC stdio for debugging, disabling UART stdio")
    pico_enable_stdio_usb("${TARGET_NAME}" 1)
    pico_enable_stdio_uart("${TARGET_NAME}" 0)
    target_compile_definitions("${TARGET_NAME}" PRIVATE OTA_DEBUG_SERIAL=1)
endif()

# Generate extra outputs
pico_add_extra_outputs("${TARGET_NAME}")

# Build the flashable A/B partition table (targets/partition_table.json) alongside
# app.uf2. This only needs to be flashed once, when first converting a device to
# use OTA updates (picotool load -F partition_table.uf2) - re-flashing it onto a
# device already running from a partition would erase the app currently there.
# After that, app.uf2 itself is loaded into whichever partition is inactive
# (picotool load -p <partition> --ignore-partitions app.uf2 during bring-up, or
# via the OTA update path in src/ota_update.c once that lands).
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/partition_table.uf2"
    DEPENDS "${CMAKE_SOURCE_DIR}/targets/partition_table.json"
    COMMAND "${PICOTOOL_EXECUTABLE}" partition create
        "${CMAKE_SOURCE_DIR}/targets/partition_table.json"
        "${CMAKE_BINARY_DIR}/partition_table.uf2"
    COMMENT "Building OTA A/B partition table (partition_table.uf2)"
)
add_custom_target(partition_table ALL
    DEPENDS "${CMAKE_BINARY_DIR}/partition_table.uf2"
)
