# Copyright (c) 2021 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

# Out-of-tree drivers for custom classes
add_subdirectory_ifdef(CONFIG_BLINK blink)
add_subdirectory_ifdef(CONFIG_WEBOS_GPIO webos_gpio)
add_subdirectory_ifdef(CONFIG_WEBOS_RGBLED webos_rgbled)

# Out-of-tree drivers for existing driver classes
add_subdirectory_ifdef(CONFIG_SENSOR sensor)
