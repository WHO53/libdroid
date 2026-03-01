/* sensor-binder-types.h
 *
 * Copyright (C) 2019 Jolla Ltd
 * Copyright (C) 2026 Deepak Meena <who53@disroot.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#pragma once

#include <gbinder.h>
#include <stdint.h>

#define ALIGNED(x) __attribute__ ((aligned(x)))

enum {
  STATUS_OK = 0,
};

typedef enum _DroidSensorInterface {
  DROID_SENSOR_INTERFACE_1_0,
  DROID_SENSOR_INTERFACE_2_0,
  DROID_SENSOR_INTERFACE_2_1,
  DROID_SENSOR_INTERFACE_COUNT
} DroidSensorInterface;

enum {
  GET_SENSORS_LIST = GBINDER_FIRST_CALL_TRANSACTION,
  SET_OPERATION_MODE,
  ACTIVATE,
  POLL,
  BATCH,
  FLUSH,
  INJECT_SENSOR_DATA,
  REGISTER_DIRECT_CHANNEL,
  UNREGISTER_DIRECT_CHANNEL,
  CONFIG_DIRECT_REPORT,
};

enum {
  INITIALIZE_2_0 = POLL,
  GET_SENSORS_LIST_2_1 = CONFIG_DIRECT_REPORT + 1,
  INITIALIZE_2_1,
  INJECT_SENSOR_DATA_2_1,
};

enum {
  DYNAMIC_SENSORS_CONNECTED_2_0 = GBINDER_FIRST_CALL_TRANSACTION,
  DYNAMIC_SENSORS_DISCONNECTED_2_0,
  DYNAMIC_SENSORS_CONNECTED_2_1
};

enum {
  EVENT_QUEUE_FLAG_READ_AND_PROCESS = 1,
  EVENT_QUEUE_FLAG_EVENTS_READ = 2,
};

enum {
  WAKE_LOCK_QUEUE_DATA_WRITTEN = 1,
};

enum {
  SENSOR_FLAG_WAKE_UP = 1u,
};

struct sensor_t {
  int32_t handle ALIGNED(4);
  GBinderHidlString name ALIGNED(8);
  GBinderHidlString vendor ALIGNED(8);
  int32_t version ALIGNED(4);
  int32_t type ALIGNED(4);
  GBinderHidlString typeAsString ALIGNED(8);
  float maxRange ALIGNED(4);
  float resolution ALIGNED(4);
  float power ALIGNED(4);
  int32_t minDelay ALIGNED(4);
  uint32_t fifoReservedEventCount ALIGNED(4);
  uint32_t fifoMaxEventCount ALIGNED(4);
  GBinderHidlString requiredPermission ALIGNED(8);
  int32_t maxDelay ALIGNED(4);
  uint32_t flags ALIGNED(4);
} ALIGNED(8);

_Static_assert(sizeof(struct sensor_t) == 112, "wrong size");

union SensorEventPayload {
  float data[16] ALIGNED(4);
} ALIGNED(8);

_Static_assert(sizeof(union SensorEventPayload) == 64, "wrong size");

struct sensors_event_t {
  int64_t timestamp ALIGNED(8);
  int32_t sensor ALIGNED(4);
  int32_t type ALIGNED(4);
  union SensorEventPayload u ALIGNED(8);
} ALIGNED(8);

_Static_assert(sizeof(struct sensors_event_t) == 80, "wrong size");
