// Copyright (c) 2026 ceil.dev
// SPDX-License-Identifier: MIT

// Portals for SensWear: the env "senswear", of type `persistence`, reached over the Bluetooth LE
// ether of libs/portals. A visitor connects, finds the portals service, enters the env, and reads
// or sets the properties of the table below. The device sends a frame only to answer one that
// arrived, or to say an answer again that was not confirmed, so the thread only goes round the
// loop of libs/portals/portals.h.
#include <string.h>
#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zbus/zbus.h>

#include "device_manager.h"
#include "portals.h"
#include "portals_lbs.h"

LOG_MODULE_REGISTER(SENSE_WEAR_PORTALS_BLUETOOTH_LOGGER);

// The stack holds the largest answer, which portal_persistence_call keeps on it, and the calls
// around it: a property's function runs here with the device manager and the driver it calls,
// and with CONFIG_LOG_MODE_IMMEDIATE a log line is printed by the thread that logs it.
#define PORTALS_LBS_THREAD_STACK_SIZE (PORTAL_PERSISTENCE_ROOM + 3072)
#define PORTALS_LBS_THREAD_PRIORITY 7
#define PORTALS_LBS_IMU_PERIOD_MS 100 // until one is set here: the same as imu_lbs.c's

// The latest battery message, copied by the listener in the device manager's thread.
static atomic_t battery_charge_dpct;
static atomic_t battery_voltage_mv;

// ───────────────────────────────── the properties ─────────────────────────────

static const char* read_firmware(void) {
	return APP_VERSION_EXTENDED_STRING;
}

#if defined(CONFIG_SENSWEAR_LP5562_DRIVER)
// The color as last set through portals: led_lbs.c keeps its own.
static uint32_t led_color; // 0x00RRGGBB

static const char* read_led_color(void) {
	static const char digits[] = "0123456789abcdef";
	static char text[8] = "#"; // "#rrggbb", kept for the library to read
	for (size_t i = 1; i < 7; i++) {
		text[i] = digits[(led_color >> (24 - 4 * i)) & 15];
	}
	return text;
}

// "#rrggbb", its digits in either case, and nothing else.
static bool write_led_color(const char* text, size_t length) {
	static const char digits[] = "0123456789abcdefABCDEF";
	uint32_t rgb = 0;
	if (length != 7 || text[0] != '#') {
		return false;
	}
	for (size_t i = 1; i < 7; i++) {
		const char* digit = memchr(digits, text[i], 22);
		if (!digit) {
			return false;
		}
		size_t at = (size_t) (digit - digits);
		rgb = rgb << 4 | (uint32_t) (at < 16 ? at : at - 6);
	}
	struct device_manager_led_color_t color = {
		.red = (uint8_t) (rgb >> 16),
		.green = (uint8_t) (rgb >> 8),
		.blue = (uint8_t) rgb,
	};
	if (device_manager_set_led_color(color) < 0) {
		return false;
	}
	led_color = rgb;
	return true;
}

static const persistence_property led[] = {
	{.key = "color",
	 .description = "its color as #rrggbb; #000000 is off",
	 .read_string = read_led_color,
	 .write_string = write_led_color},
};
#endif

#if defined(CONFIG_SENSWEAR_BQ27427_DRIVER)
// Rounded as power_lbs_soc_to_battery_level rounds it.
static int32_t read_battery_level(void) {
	int32_t dpct = (int32_t) atomic_get(&battery_charge_dpct);
	return dpct <= 0 ? 0 : dpct >= 1000 ? 100 : (dpct + 5) / 10;
}

static int32_t read_battery_voltage(void) {
	return (int32_t) atomic_get(&battery_voltage_mv);
}

static const persistence_property battery[] = {
	{.key = "level",
	 .unit = "%",
	 .description = "how full it is",
	 .read_number = read_battery_level},
	{.key = "voltage",
	 .unit = "mV",
	 .description = "its voltage",
	 .read_number = read_battery_voltage},
};
#endif

#if defined(CONFIG_SENSWEAR_BHI360_DRIVER)
// The streams and the period as last set through portals: imu_lbs.c keeps its own.
static bool imu_streams;
static int32_t imu_period_ms = PORTALS_LBS_IMU_PERIOD_MS;

static bool read_imu_streams(void) {
	return imu_streams;
}

static bool write_imu_streams(bool on) {
	if (device_manager_set_imu_phy_streams_enabled(on, (uint32_t) imu_period_ms) < 0) {
		return false;
	}
	imu_streams = on;
	return true;
}

static int32_t read_imu_period(void) {
	return imu_period_ms;
}

// 0 and below are refused. A period is applied at once only while the streams are on, as imu_lbs.c
// applies it: the device manager stops them and starts them again with it.
static bool write_imu_period(int32_t ms) {
	if (ms <= 0) {
		return false;
	}
	if (imu_streams && device_manager_set_imu_phy_streams_enabled(true, (uint32_t) ms) < 0) {
		return false;
	}
	imu_period_ms = ms;
	return true;
}

static const persistence_property imu[] = {
	{.key = "streams",
	 .description = "whether it sends its readings",
	 .read_boolean = read_imu_streams,
	 .write_boolean = write_imu_streams},
	{.key = "period",
	 .unit = "ms",
	 .description = "how often its readings are collected",
	 .read_number = read_imu_period,
	 .write_number = write_imu_period},
};
#endif

#if defined(CONFIG_SHIELD_SENSWEAR_HAPTIC)
// Takes { amplitude, duration }. A duration goes up to 65535 ms, as in the haptic service: the
// device manager turns it into microseconds in 32 bits, which hold no more than 4,294,967 ms.
static bool vibrate(const persistence_value* takes, persistence_value* gives) {
	int32_t amplitude = takes[0].number;
	int32_t duration_ms = takes[1].number;
	if (amplitude < 0 || amplitude > 255 || duration_ms < 1 || duration_ms > UINT16_MAX) {
		gives->string = "it takes an amplitude from 0 to 255 and a duration from 1 to 65535 ms";
	} else if (device_manager_haptic_vibrate((uint8_t) amplitude, (uint32_t) duration_ms) < 0) {
		gives->string = "the motor did not start";
	}
	return !gives->string;
}

static bool stop(const persistence_value* takes, persistence_value* gives) {
	ARG_UNUSED(takes);
	if (device_manager_haptic_stop() >= 0) {
		return true;
	}
	gives->string = "the motor did not stop";
	return false;
}

static const persistence_property vibration[] = {
	{.key = "amplitude", .type = "number", .description = "how strong, from 0 to 255"},
	{.key = "duration", .type = "number", .unit = "ms", .description = "how long, from 1 to 65535"},
};
static const persistence_property vibrate_takes = {.type = "object",
												   .children = vibration,
												   .count = ARRAY_SIZE(vibration)};
static const persistence_property haptic[] = {
	{.key = "vibrate", .description = "vibrates once", .call = vibrate, .input = &vibrate_takes},
	{.key = "stop", .description = "stops it", .call = stop},
	{.key = "active",
	 .description = "whether it vibrates now",
	 .read_boolean = device_manager_haptic_is_active},
};
#endif

static const persistence_property properties[] = {
	{.key = "firmware", .description = "the firmware's version", .read_string = read_firmware},
#if defined(CONFIG_SENSWEAR_LP5562_DRIVER)
	{.key = "led",
	 .description = "the light on the main board",
	 .children = led,
	 .count = ARRAY_SIZE(led)},
#endif
#if defined(CONFIG_SENSWEAR_BQ27427_DRIVER)
	{.key = "battery",
	 .description = "the battery",
	 .children = battery,
	 .count = ARRAY_SIZE(battery)},
#endif
#if defined(CONFIG_SENSWEAR_BHI360_DRIVER)
	{.key = "imu", .description = "the motion sensor", .children = imu, .count = ARRAY_SIZE(imu)},
#endif
#if defined(CONFIG_SHIELD_SENSWEAR_HAPTIC)
	{.key = "haptic",
	 .description = "the vibration motor",
	 .children = haptic,
	 .count = ARRAY_SIZE(haptic)},
#endif
};
static const persistence_env senswear = {properties, ARRAY_SIZE(properties)};

// ───────────────────────────────── the env and the core ───────────────────────

static void handle(const portal_event* event, portals_core* m) {
	static const portal_bytes none = {NULL, 0};
	if (event->type == PORTAL_OPEN) {
		LOG_INF("a visitor came in");
	} else if (event->type == PORTAL_CLOSE) {
		LOG_INF("a visitor left");
	} else if (event->type == PORTAL_CALL && !portal_persistence_call(m, &senswear, event)) {
		portal_reply(m, &event->ref, PORTAL_UNKNOWN, none);
	}
}

// Anyone in range may enter: the Bluetooth LE ether vouches for nobody.
static bool anyone(const portal_event* call, portals_core* m) {
	ARG_UNUSED(call);
	ARG_UNUSED(m);
	return true;
}

static const portal_api api = {ETHER_MEMBERS,
							   .allow = anyone,
							   .random = ether_ble_zephyr_random,
							   PORTAL_CBOR,
							   PORTAL_OPERATIONS};
// Its table is set when the thread starts. Set here, the whole core, 6 KB of zeros, would be
// stored in flash as well.
static portals_core core;

static void portals_lbs_thread(void* p1, void* p2, void* p3) {
	static uint8_t about[80]; // what @describe answers, packed
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	if (!portal_complete(&api)) {
		LOG_ERR("a member of the core's table is not set");
		return;
	}
	ether* e = ether_ble_zephyr();
	core.api = &api;
	ether_plug(e, &core, handle);
	if (!portal_persistence_expose(&core, "senswear", "a SensWear device", about, sizeof about)) {
		LOG_ERR("the env senswear was not exposed");
	}
	for (;;) {
		if (!e->poll(e, 1000)) {
			portal_repeat(&core); // a second of silence: what is not confirmed is said again
		}
		portal_flush(&core);
	}
}

K_THREAD_DEFINE(portals_lbs_thread_id,
				PORTALS_LBS_THREAD_STACK_SIZE,
				portals_lbs_thread,
				NULL,
				NULL,
				NULL,
				PORTALS_LBS_THREAD_PRIORITY,
				0,
				0);

// ───────────────────────────────── the battery ───────────────────────────────

// Called in the device manager's thread, which must not wait: it only copies two numbers.
static void portals_lbs_listener_cb(const struct zbus_channel* chan) {
	if (device_manager_stream_from_channel(chan) != device_manager_stream_Battery) {
		return;
	}
	const struct battery_msg_t* msg = zbus_chan_const_msg(chan);
	atomic_set(&battery_charge_dpct, msg->state_of_charge_dpct);
	atomic_set(&battery_voltage_mv, msg->voltage_mv);
}

ZBUS_LISTENER_DEFINE(portals_lbs_listener, portals_lbs_listener_cb);

int portals_lbs_register_streams(void) {
	static bool registered; // zbus answers -EALREADY to a listener it already has
	if (registered) {
		return 0;
	}
	int ret = device_manager_stream_register(device_manager_stream_Battery,
											 &portals_lbs_listener,
											 K_MSEC(100));
	if (ret < 0) {
		LOG_ERR("the battery listener was not registered: %d", ret);
	}
	registered = ret == 0;
	return ret;
}
