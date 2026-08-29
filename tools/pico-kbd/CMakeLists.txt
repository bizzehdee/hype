cmake_minimum_required(VERSION 3.13)

# Pinned by the environment, not vendored: the SDK is ~100 MB and belongs outside this repo.
# See README.md for the one-line clone if PICO_SDK_PATH is unset.
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(hype_pico_kbd C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(hype_pico_kbd
    main.c
    usb_descriptors.c
)

target_include_directories(hype_pico_kbd PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(hype_pico_kbd pico_stdlib tinyusb_device tinyusb_board)

# The board is a keyboard, not a console. Leaving stdio on USB would add a CDC interface and
# make it a composite device, which is exactly what it must not be -- hype has to claim it
# through the single-interface boot-keyboard path.
pico_enable_stdio_usb(hype_pico_kbd 0)
pico_enable_stdio_uart(hype_pico_kbd 1)

pico_add_extra_outputs(hype_pico_kbd)
