# SPDX-License-Identifier: Apache-2.0
#
# Zephyr sysbuild entry: https://docs.zephyrproject.org/latest/build/sysbuild/index.html
# This file is loaded as ${APP_DIR}/sysbuild/CMakeLists.txt (not app/sysbuild.cmake).

cmake_minimum_required(VERSION 3.20.0)

# Compute the signing key path BEFORE find_package(Sysbuild) so it is
# available when sysbuild_kconfig.cmake (loaded inside find_package) reads
# SB_EXTRA_CONF_FILE to add extra fragments to the sysbuild Kconfig.
cmake_path(SET APP_SIGN_KEY "${CMAKE_CURRENT_LIST_DIR}/../keys/mcuboot.pem")
cmake_path(ABSOLUTE_PATH APP_SIGN_KEY NORMALIZE)

if(NOT EXISTS "${APP_SIGN_KEY}")
	message(FATAL_ERROR
		"Missing MCUboot signing key: ${APP_SIGN_KEY}\n"
		"From the repository root run:\n"
		"  Windows: .\\scripts\\setup_dev_keys.ps1\n"
		"  Unix:    ./scripts/setup_dev_keys.sh\n")
endif()

# Write a sysbuild Kconfig fragment that overrides BOOT_SIGNATURE_KEY_FILE
# with our custom key. This must happen before find_package(Sysbuild) because
# sysbuild_kconfig.cmake (included by find_package) reads SB_EXTRA_CONF_FILE
# during Kconfig processing, which then feeds SB_CONFIG_BOOT_SIGNATURE_KEY_FILE
# into MAIN_image_default.cmake -> set_config_string -> FORCED_CONF_FILE for
# both the app (CONFIG_MCUBOOT_SIGNATURE_KEY_FILE) and mcuboot
# (CONFIG_BOOT_SIGNATURE_KEY_FILE). Setting it after find_package is too late.
string(REPLACE "\\" "/" APP_SIGN_KEY_FWD "${APP_SIGN_KEY}")
set(SB_SIGN_CONF "${CMAKE_CURRENT_BINARY_DIR}/mcuboot_signkey.conf")
file(WRITE "${SB_SIGN_CONF}"
  "SB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"${APP_SIGN_KEY_FWD}\"\n")
set(SB_EXTRA_CONF_FILE "${SB_SIGN_CONF}")

find_package(Sysbuild REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(sysbuild LANGUAGES)

message(STATUS "MCUboot signing key (ECDSA P-256): ${APP_SIGN_KEY}")
