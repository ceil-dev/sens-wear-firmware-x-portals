# Copyright (c) 2026 ceil.dev
# SPDX-License-Identifier: MIT

# Portals: the portals core, the env type `persistence`, and the Bluetooth LE ether with its Zephyr port.
# The portals service is their only user: src/bluetooth/services/services.cmake links them to it.

add_library(portals INTERFACE)

target_sources(portals INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/portals.c
)

target_include_directories(portals INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(portals INTERFACE
    # Envs the device exposes: senswear.
    PORTAL_MAX_ENVS=1
    # Portals connections at once, over the one Bluetooth connection.
    PORTAL_MAX_CONNECTIONS=2
    # The largest frame it sends, and so the queue of answers not confirmed yet for each visitor: the largest answer
    # leaves in 1,609 bytes at most and takes 1,553 of the queue; under 2,432 a frame leaves at any ATT MTU.
    PORTAL_FRAME_SIZE=2048
    # Bluetooth connections served at once: the firmware keeps one.
    ETHER_BLE_MAX_LINKS=1
    # Packets that wait for the portals thread, CONFIG_BT_L2CAP_TX_MTU and 3 bytes each: 4 halves their RAM.
    ETHER_BLE_MAX_WAITING=8
)
