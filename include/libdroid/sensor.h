/* sensor.h
 *
 * Copyright (C) 2013 Jolla Ltd.
 * Copyright (C) 2025 Jollyboys Ltd.
 * Copyright (C) 2026 Deepak Meena <who53@disroot.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#pragma once

#include <glib-object.h>
#include <stdint.h>

G_BEGIN_DECLS

#define DROID_TYPE_SENSOR droid_sensor_get_type ()
G_DECLARE_FINAL_TYPE (DroidSensor, droid_sensor, DROID, SENSOR, GObject)

typedef struct _DroidSensorInfo {
  int32_t handle;
  char   *name;
  char   *vendor;
  int32_t version;
  int32_t type;
  char   *type_as_string;
  float   max_range;
  float   resolution;
  float   power;
  int32_t min_delay_us;
  uint32_t fifo_reserved_event_count;
  uint32_t fifo_max_event_count;
  char   *required_permission;
  int32_t max_delay_us;
  uint32_t flags;
} DroidSensorInfo;

typedef struct _DroidSensorEvent {
  int64_t timestamp_ns;
  int32_t sensor_handle;
  int32_t sensor_type;
  float   data[16];
} DroidSensorEvent;

typedef enum _DroidSensorType {
  DROID_SENSOR_TYPE_ACCELEROMETER = 1,
  DROID_SENSOR_TYPE_MAGNETIC_FIELD = 2,
  DROID_SENSOR_TYPE_ORIENTATION = 3, /* deprecated in Android, kept for compatibility */
  DROID_SENSOR_TYPE_GYROSCOPE = 4,
  DROID_SENSOR_TYPE_LIGHT = 5,
  DROID_SENSOR_TYPE_PRESSURE = 6,
  DROID_SENSOR_TYPE_TEMPERATURE = 7, /* deprecated in Android */
  DROID_SENSOR_TYPE_PROXIMITY = 8,
  DROID_SENSOR_TYPE_GRAVITY = 9,
  DROID_SENSOR_TYPE_LINEAR_ACCELERATION = 10,
  DROID_SENSOR_TYPE_ROTATION_VECTOR = 11,
  DROID_SENSOR_TYPE_RELATIVE_HUMIDITY = 12,
  DROID_SENSOR_TYPE_AMBIENT_TEMPERATURE = 13,
  DROID_SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED = 14,
  DROID_SENSOR_TYPE_GAME_ROTATION_VECTOR = 15,
  DROID_SENSOR_TYPE_GYROSCOPE_UNCALIBRATED = 16,
  DROID_SENSOR_TYPE_SIGNIFICANT_MOTION = 17,
  DROID_SENSOR_TYPE_STEP_DETECTOR = 18,
  DROID_SENSOR_TYPE_STEP_COUNTER = 19,
  DROID_SENSOR_TYPE_GEOMAGNETIC_ROTATION_VECTOR = 20,
  DROID_SENSOR_TYPE_HEART_RATE = 21,
  DROID_SENSOR_TYPE_TILT_DETECTOR = 22,
  DROID_SENSOR_TYPE_WAKE_GESTURE = 23,
  DROID_SENSOR_TYPE_GLANCE_GESTURE = 24,
  DROID_SENSOR_TYPE_PICK_UP_GESTURE = 25,
  DROID_SENSOR_TYPE_WRIST_TILT_GESTURE = 26,
  DROID_SENSOR_TYPE_DEVICE_ORIENTATION = 27,
  DROID_SENSOR_TYPE_POSE_6DOF = 28,
  DROID_SENSOR_TYPE_STATIONARY_DETECT = 29,
  DROID_SENSOR_TYPE_MOTION_DETECT = 30,
  DROID_SENSOR_TYPE_HEART_BEAT = 31,
  DROID_SENSOR_TYPE_DYNAMIC_SENSOR_META = 32,
  DROID_SENSOR_TYPE_ADDITIONAL_INFO = 33,
  DROID_SENSOR_TYPE_LOW_LATENCY_OFFBODY_DETECT = 34,
  DROID_SENSOR_TYPE_ACCELEROMETER_UNCALIBRATED = 35,
  DROID_SENSOR_TYPE_HINGE_ANGLE = 36,
  DROID_SENSOR_TYPE_HEAD_TRACKER = 37,
} DroidSensorType;

typedef enum _DroidSensorHalVersion {
  DROID_SENSOR_HAL_UNKNOWN = 0,
  DROID_SENSOR_HAL_1_0,
  DROID_SENSOR_HAL_2_0,
  DROID_SENSOR_HAL_2_1,
} DroidSensorHalVersion;

DroidSensor           *droid_sensor_new        (GError **error);
uint32_t               droid_sensor_get_count  (DroidSensor *self);
const DroidSensorInfo *droid_sensor_get_info   (DroidSensor *self,
                                                 uint32_t     index);
gboolean               droid_sensor_set_active (DroidSensor *self,
                                                 int32_t      handle,
                                                 gboolean     active);
int                    droid_sensor_poll       (DroidSensor      *self,
                                                 DroidSensorEvent *events,
                                                 uint32_t          max_events);
int                    droid_sensor_poll_blocking (DroidSensor      *self,
                                                    DroidSensorEvent *events,
                                                    uint32_t          max_events,
                                                    int               timeout_ms);

DroidSensorHalVersion  droid_sensor_get_hal_version (DroidSensor *self);
const char            *droid_sensor_hal_version_name (DroidSensorHalVersion version);

const char            *droid_sensor_type_name  (int32_t type);
gboolean               droid_sensor_type_is_orientation (int32_t type);

G_END_DECLS
