# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# west build app -- -DAPP_FIRMWARE_VERSION=2.0.0 -DAPP_BLE_NAME=...
# Note: in sysbuild mode the app cmake runs as a separate ExternalProject and
# does not inherit top-level cmake -D args. Set APP_FIRMWARE_VERSION /
# APP_BLE_NAME as env vars before invoking west to pass them through:
#   $env:APP_FIRMWARE_VERSION = "2.0.0"; west build --sysbuild ...
if(NOT DEFINED APP_FIRMWARE_VERSION AND NOT DEFINED CACHE{APP_FIRMWARE_VERSION})
  if(DEFINED ENV{APP_FIRMWARE_VERSION})
    set(APP_FIRMWARE_VERSION "$ENV{APP_FIRMWARE_VERSION}" CACHE STRING "Image version in logs; also passed to imgtool sign")
  else()
    set(APP_FIRMWARE_VERSION "1.0.0" CACHE STRING "Image version in logs; also passed to imgtool sign")
  endif()
endif()
if(NOT DEFINED APP_BLE_NAME AND NOT DEFINED CACHE{APP_BLE_NAME})
  if(DEFINED ENV{APP_BLE_NAME})
    set(APP_BLE_NAME "$ENV{APP_BLE_NAME}" CACHE STRING "GATT/BLE GAP name")
  else()
    set(APP_BLE_NAME "Dax_BLE_v1" CACHE STRING "GATT/BLE GAP name")
  endif()
endif()

# Kconfig's CONFIG_MCUBOOT_SIGNATURE_KEY_FILE is relative to *west* workspace topdir, not
# this app. A mis-resolved key makes imgtool use the wrong PEM while MCUboot was built
# (sysbuild) with app/keys/mcuboot.pem — upload "succeeds" but swap never uses the image.
cmake_path(SET MCUBOOT_PEM "${CMAKE_CURRENT_SOURCE_DIR}/keys/mcuboot.pem" NORMALIZE)
if(NOT EXISTS "${MCUBOOT_PEM}")
  message(FATAL_ERROR
    "Missing ${MCUBOOT_PEM} — run scripts/setup_dev_keys.ps1 (or .sh) from the repo root.")
endif()
string(REPLACE "\\" "/" MCUBOOT_PEM_FWD "${MCUBOOT_PEM}")
set(IMGTOOL_VER "${APP_FIRMWARE_VERSION}+0")
set(GEN_KCONF "${CMAKE_BINARY_DIR}/mcuboot_ota_kconf.conf")
file(WRITE "${GEN_KCONF}"
  "CONFIG_MCUBOOT_SIGNATURE_KEY_FILE=\"${MCUBOOT_PEM_FWD}\"\n"
  "CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION=\"${IMGTOOL_VER}\"\n"
  )
set(EXTRA_CONF_FILE "${GEN_KCONF}")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(secure_ble_ota)

target_sources(app PRIVATE src/main.c)

option(CRASH_DEMO "Panic before boot_write_img_confirmed() for rollback demo" OFF)
if(CRASH_DEMO)
  target_compile_definitions(app PRIVATE CRASH_DEMO=1)
endif()

target_compile_definitions(
  app
  PRIVATE
  "APP_FIRMWARE_VERSION=\"${APP_FIRMWARE_VERSION}\""
  "APP_BLE_NAME=\"${APP_BLE_NAME}\""
)
