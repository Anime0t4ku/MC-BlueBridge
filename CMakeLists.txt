cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_BOARD)
  set(PICO_BOARD pico2_w)
endif()

if(NOT PICO_BOARD STREQUAL "pico2_w")
  message(FATAL_ERROR "MC BlueBridge requires PICO_BOARD=pico2_w")
endif()

if(DEFINED ENV{BTSTACK_ROOT})
  set(PICO_BTSTACK_PATH $ENV{BTSTACK_ROOT} CACHE PATH "" FORCE)
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/deps/btstack/src/btstack.h")
  set(PICO_BTSTACK_PATH "${CMAKE_CURRENT_LIST_DIR}/deps/btstack" CACHE PATH "" FORCE)
endif()

include(pico_sdk_import.cmake)
project(mc_bluebridge C CXX ASM)

if(DEFINED ENV{LIBXSM3_ROOT})
  set(LIBXSM3_SOURCE_DIR $ENV{LIBXSM3_ROOT})
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/deps/libxsm3/xsm3.h")
  set(LIBXSM3_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/deps/libxsm3")
else()
  include(FetchContent)
  FetchContent_Declare(
    libxsm3
    GIT_REPOSITORY https://github.com/InvoxiPlayGames/libxsm3.git
    GIT_TAG master
    GIT_SHALLOW TRUE
  )
  FetchContent_GetProperties(libxsm3)
  if(NOT libxsm3_POPULATED)
    FetchContent_Populate(libxsm3)
  endif()
  set(LIBXSM3_SOURCE_DIR ${libxsm3_SOURCE_DIR})
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(mc_bluebridge
  src/main.c
  src/bluebridge_state.c
  src/bluebridge_platform.c
  src/bt_host.c
  src/controller_input.c
  src/controller_driver.c
  src/hid_feedback.c
  src/xinput_device.c
  src/dualsense_device.c
  src/xbox360_auth.c
  ${LIBXSM3_SOURCE_DIR}/xsm3.c
  ${LIBXSM3_SOURCE_DIR}/usbdsec.c
  ${LIBXSM3_SOURCE_DIR}/excrypt_des.c
  ${LIBXSM3_SOURCE_DIR}/excrypt_parve.c
  src/xsm3_excrypt_sha.c
  src/usb_device.c
  src/usb_descriptors.c
  src/cdc_protocol.c
  src/config_store.c
  src/led_status.c
  src/bootsel_button.c
)

target_include_directories(mc_bluebridge PRIVATE
  src
  ${LIBXSM3_SOURCE_DIR}
)

target_compile_definitions(mc_bluebridge PRIVATE XSM3_NO_DEBUGGING=1)

target_link_libraries(mc_bluebridge PUBLIC
  pico_stdlib
  pico_multicore
  pico_flash
  pico_unique_id
  hardware_flash
  hardware_watchdog
  hardware_sync
  tinyusb_device
  tinyusb_board
  pico_cyw43_arch_none
  pico_btstack_classic
  pico_btstack_ble
  pico_btstack_cyw43
  pico_btstack_flash_bank
)

pico_enable_stdio_usb(mc_bluebridge 0)
pico_enable_stdio_uart(mc_bluebridge 0)
pico_add_extra_outputs(mc_bluebridge)
