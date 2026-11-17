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
    PORTAL_ENVS=1
    # Portals connections at once, over the one Bluetooth connection.
    PORTAL_CONNECTIONS=2
    # The largest frame it sends: the largest answer leaves in 1,609 bytes at most; under 2,432 one leaves at any ATT MTU.
    PORTAL_FRAME=2048
    # For each visitor, the bytes of answers not confirmed yet: the largest answer takes 1,553 of them.
    PORTAL_QUEUE=2048
    # The largest answer: the schema, 1,127 bytes, is the largest now.
    PORTAL_PERSISTENCE_ROOM=1536
    # Bluetooth connections served at once: the firmware keeps one.
    ETHER_BLE_LINKS=1
    # Packets that wait for the portals thread, CONFIG_BT_L2CAP_TX_MTU and 3 bytes each: 4 halves their RAM.
    ETHER_BLE_WAITING=8
)
