// Copyright (c) 2026 ceil.dev
// SPDX-License-Identifier: MIT

// Portals for SensWear: the env "senswear", of type `persistence`, over Bluetooth LE. Its thread
// starts at boot.
#ifndef PORTALS_LBS_H_
#define PORTALS_LBS_H_

// Registers the battery's listener with the device manager, after device_manager_start(). 0, or
// its error. It registers once: a later call answers 0, and a call after an error tries again.
int portals_lbs_register_streams(void);

#endif
