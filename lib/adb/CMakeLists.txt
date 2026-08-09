# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_include_directories(.)
zephyr_library_sources(adb.c adb_protocol.c)
zephyr_library_sources_ifdef(CONFIG_WEBOS_ADB_USB transports/adb_usb.c)
