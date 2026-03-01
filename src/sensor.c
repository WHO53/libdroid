/* sensor.c
 *
 * Copyright (C) 2013 Jolla Ltd.
 * Copyright (C) 2025 Jollyboys Ltd.
 * Copyright (C) 2026 Deepak Meena <who53@disroot.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#define G_LOG_DOMAIN "droid-sensor"

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>

#include <errno.h>
#include <string.h>

#include <libdroid/sensor.h>

#include "binder.h"
#include "sensor-binder-types.h"

#define BINDER_SENSOR_DEVICE "/dev/hwbinder"

#define BINDER_SENSOR_IFACE_1_0 "android.hardware.sensors@1.0::ISensors"
#define BINDER_SENSOR_NAME_1_0  BINDER_SENSOR_IFACE_1_0 "/default"
#define BINDER_SENSOR_IFACE_2_0 "android.hardware.sensors@2.0::ISensors"
#define BINDER_SENSOR_NAME_2_0  BINDER_SENSOR_IFACE_2_0 "/default"
#define BINDER_SENSOR_CALLBACK_IFACE_2_0 "android.hardware.sensors@2.0::ISensorsCallback"
#define BINDER_SENSOR_IFACE_2_1 "android.hardware.sensors@2.1::ISensors"
#define BINDER_SENSOR_NAME_2_1  BINDER_SENSOR_IFACE_2_1 "/default"
#define BINDER_SENSOR_CALLBACK_IFACE_2_1 "android.hardware.sensors@2.1::ISensorsCallback"

#define FMQ_EVENT_CAPACITY 128
#define FMQ_WAKELOCK_CAPACITY 128
#define MAX_RECEIVE_BUFFER_EVENT_COUNT 128
#define DEFAULT_SENSOR_DELAY_NS (50 * 1000 * 1000LL)

struct _DroidSensor
{
  GObject parent_instance;

  GBinderServiceManager *service_manager;
  GBinderRemoteObject   *remote;
  GBinderClient         *client;
  gulong                 death_id;
  gulong                 poll_transact_id;

  DroidSensorInterface   iface;

  GBinderLocalObject    *sensor_callback;
  GBinderFmq            *event_queue;
  GBinderFmq            *wake_lock_queue;

  GThread               *event_thread;
  gint                   stop_thread;
  GAsyncQueue           *event_queue_local;

  DroidSensorInfo       *sensors;
  uint32_t               sensor_count;
};

static void initable_interface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (DroidSensor, droid_sensor, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_interface_init))

static const GBinderClientIfaceInfo sensor_2_client_ifaces[] = {
  { BINDER_SENSOR_IFACE_2_1, INJECT_SENSOR_DATA_2_1 },
  { BINDER_SENSOR_IFACE_2_0, CONFIG_DIRECT_REPORT },
};

static const char* const sensor_callback_ifaces[] = {
  BINDER_SENSOR_CALLBACK_IFACE_2_1,
  BINDER_SENSOR_CALLBACK_IFACE_2_0,
  NULL
};

static void droid_sensor_clear_sensors (DroidSensor *self);
static gboolean droid_sensor_load_sensors (DroidSensor *self);
static gboolean droid_sensor_connect (DroidSensor *self);
static void droid_sensor_stop_event_thread (DroidSensor *self);
static void droid_sensor_disconnect (DroidSensor *self,
                                     gboolean     clear_events);
static gboolean droid_sensor_try_connect_2x (DroidSensor *self,
                                             const char  *service_name,
                                             DroidSensorInterface iface,
                                             guint        initialize_code);
static gboolean droid_sensor_try_connect_1_0 (DroidSensor *self);
static gboolean droid_sensor_set_delay_internal (DroidSensor *self,
                                                 int32_t      handle,
                                                 int64_t      sampling_period_ns,
                                                 int64_t      max_report_latency_ns);
static gboolean droid_sensor_start_connect (DroidSensor *self);
static gboolean droid_sensor_ensure_connected (DroidSensor *self);
static void droid_sensor_poll_events_1_0 (DroidSensor *self);

static guint32
sensor_flags_for_handle (DroidSensor *self,
                         int32_t      handle)
{
  uint32_t i;

  for (i = 0; i < self->sensor_count; i++) {
    if (self->sensors[i].handle == handle)
      return self->sensors[i].flags;
  }

  return 0;
}

static void
droid_sensor_info_clear (DroidSensorInfo *info)
{
  g_free (info->name);
  g_free (info->vendor);
  g_free (info->type_as_string);
  g_free (info->required_permission);
}

static void
droid_sensor_clear_queued_events (DroidSensor *self)
{
  for (;;) {
    DroidSensorEvent *ev;

    ev = g_async_queue_try_pop (self->event_queue_local);
    if (!ev)
      break;
    g_free (ev);
  }
}

static void
droid_sensor_clear_sensors (DroidSensor *self)
{
  uint32_t i;

  for (i = 0; i < self->sensor_count; i++) {
    droid_sensor_info_clear (&self->sensors[i]);
  }

  g_clear_pointer (&self->sensors, g_free);
  self->sensor_count = 0;
}

static gboolean
droid_sensor_decode_status_error_reply (GBinderRemoteReply *reply)
{
  GBinderReader reader;
  int32_t status = -1;
  int32_t error = -1;

  if (!reply)
    return FALSE;

  gbinder_remote_reply_init_reader (reply, &reader);
  gbinder_reader_read_int32 (&reader, &status);
  gbinder_reader_read_int32 (&reader, &error);

  return status == STATUS_OK && error == STATUS_OK;
}

static void
droid_sensor_queue_events (DroidSensor                   *self,
                           const struct sensors_event_t  *buffer,
                           gsize                          num_events)
{
  gsize i;

  for (i = 0; i < num_events; i++) {
    DroidSensorEvent *ev;

    ev = g_new0 (DroidSensorEvent, 1);
    ev->timestamp_ns = buffer[i].timestamp;
    ev->sensor_handle = buffer[i].sensor;
    ev->sensor_type = buffer[i].type;
    memcpy (ev->data, buffer[i].u.data, sizeof (ev->data));

    g_async_queue_push (self->event_queue_local, ev);
  }
}

static GBinderLocalReply*
sensor_callback_handler (GBinderLocalObject*  obj,
                         GBinderRemoteRequest* req,
                         guint                code,
                         guint                flags,
                         int*                 status,
                         void*                user_data)
{
  const char *iface;

  (void)obj;
  (void)flags;
  (void)user_data;

  iface = gbinder_remote_request_interface (req);
  if (iface && (!strcmp (iface, BINDER_SENSOR_CALLBACK_IFACE_2_0) ||
    !strcmp (iface, BINDER_SENSOR_CALLBACK_IFACE_2_1))) {
    switch (code) {
      case DYNAMIC_SENSORS_CONNECTED_2_0:
      case DYNAMIC_SENSORS_CONNECTED_2_1:
        g_debug ("Dynamic sensor connected callback");
        break;
      case DYNAMIC_SENSORS_DISCONNECTED_2_0:
        g_debug ("Dynamic sensor disconnected callback");
        break;
      default:
        g_debug ("Unknown sensor callback code %u", code);
        break;
    }
  }

  *status = GBINDER_STATUS_OK;
  return NULL;
}

static void
sensor_binder_died (GBinderRemoteObject *obj,
                    void                *user_data)
{
  DroidSensor *self;

  (void)obj;

  self = DROID_SENSOR (user_data);
  g_warning ("Sensor binder service died");
  self->death_id = 0;
  droid_sensor_disconnect (self, FALSE);
  droid_sensor_start_connect (self);
}

static void
droid_sensor_poll_events_1_0_callback (GBinderClient      *client,
                                       GBinderRemoteReply *reply,
                                       int                 status,
                                       void               *user_data)
{
  DroidSensor *self;

  (void)client;

  self = DROID_SENSOR (user_data);
  self->poll_transact_id = 0;

  if (status != GBINDER_STATUS_OK) {
    g_warning ("Sensor poll failed status %d", status);
    g_usleep (50 * 1000);
  } else if (reply) {
    GBinderReader reader;
    int32_t reader_status;
    int32_t result;
    gsize event_count;
    gsize struct_size;
    const struct sensors_event_t *buffer;

    reader_status = -1;
    result = -1;
    event_count = 0;
    struct_size = 0;

    gbinder_remote_reply_init_reader (reply, &reader);
    gbinder_reader_read_int32 (&reader, &reader_status);
    gbinder_reader_read_int32 (&reader, &result);
    buffer = gbinder_reader_read_hidl_vec (&reader, &event_count, &struct_size);

    if (reader_status == STATUS_OK && result >= 0 && buffer && event_count > 0) {
      droid_sensor_queue_events (self, buffer, event_count);
    }
  }

  droid_sensor_poll_events_1_0 (self);
}

static void
droid_sensor_poll_events_1_0 (DroidSensor *self)
{
  GBinderLocalRequest *req;

  if (!self->client || self->iface != DROID_SENSOR_INTERFACE_1_0)
    return;

  req = gbinder_client_new_request2 (self->client, POLL);
  gbinder_local_request_append_int32 (req, 16);
  self->poll_transact_id = gbinder_client_transact (self->client,
                                                    POLL,
                                                    0,
                                                    req,
                                                    droid_sensor_poll_events_1_0_callback,
                                                    NULL,
                                                    self);
  gbinder_local_request_unref (req);
}

static gboolean
droid_sensor_load_sensors (DroidSensor *self)
{
  GBinderReader reader;
  GBinderRemoteReply *reply;
  int32_t tx_status = 0;
  int32_t status = -1;
  gsize count = 0;
  gsize vec_size =0;
  const struct sensor_t *vec;
  guint code;
  uint32_t i;

  droid_sensor_clear_sensors (self);

  code = (self->iface == DROID_SENSOR_INTERFACE_2_1)
    ? GET_SENSORS_LIST_2_1
    : GET_SENSORS_LIST;

  reply = gbinder_client_transact_sync_reply (self->client, code, NULL, &tx_status);
  if (tx_status != GBINDER_STATUS_OK || !reply) {
    g_warning ("Unable to get sensor list");
    return FALSE;
  }

  gbinder_remote_reply_init_reader (reply, &reader);
  if (!gbinder_reader_read_int32 (&reader, &status) || status != STATUS_OK) {
    g_warning ("Sensor list call returned status %d", status);
    gbinder_remote_reply_unref (reply);
    return FALSE;
  }

  vec = gbinder_reader_read_hidl_vec (&reader, &count, &vec_size);
  if (!vec || count == 0) {
    gbinder_remote_reply_unref (reply);
    return TRUE;
  }

  self->sensors = g_new0 (DroidSensorInfo, count);
  self->sensor_count = (uint32_t) count;

  for (i = 0; i < self->sensor_count; i++) {
    GBinderBuffer *buffer;

    self->sensors[i].handle = vec[i].handle;
    self->sensors[i].version = vec[i].version;
    self->sensors[i].type = vec[i].type;
    self->sensors[i].max_range = vec[i].maxRange;
    self->sensors[i].resolution = vec[i].resolution;
    self->sensors[i].power = vec[i].power;
    self->sensors[i].min_delay_us = vec[i].minDelay;
    self->sensors[i].fifo_reserved_event_count = vec[i].fifoReservedEventCount;
    self->sensors[i].fifo_max_event_count = vec[i].fifoMaxEventCount;
    self->sensors[i].max_delay_us = vec[i].maxDelay;
    self->sensors[i].flags = vec[i].flags;

    buffer = gbinder_reader_read_buffer (&reader);
    if (buffer) {
      self->sensors[i].name = g_strdup ((const gchar*) buffer->data);
      gbinder_buffer_free (buffer);
    }

    buffer = gbinder_reader_read_buffer (&reader);
    if (buffer) {
      self->sensors[i].vendor = g_strdup ((const gchar*) buffer->data);
      gbinder_buffer_free (buffer);
    }

    buffer = gbinder_reader_read_buffer (&reader);
    if (buffer) {
      self->sensors[i].type_as_string = g_strdup ((const gchar*) buffer->data);
      gbinder_buffer_free (buffer);
    }

    buffer = gbinder_reader_read_buffer (&reader);
    if (buffer) {
      self->sensors[i].required_permission = g_strdup ((const gchar*) buffer->data);
      gbinder_buffer_free (buffer);
    }
  }

  gbinder_remote_reply_unref (reply);
  return TRUE;
}

static gboolean
droid_sensor_init_2x (DroidSensor *self,
                      guint        initialize_code)
{
  GBinderLocalRequest *req;
  GBinderRemoteReply *reply;
  GBinderWriter writer;
  int32_t tx_status = 0;
  gboolean ok;

  self->sensor_callback = gbinder_servicemanager_new_local_object2 (self->service_manager,
                                                                    sensor_callback_ifaces,
                                                                    sensor_callback_handler,
                                                                    self);
  if (!self->sensor_callback) {
    g_warning ("Failed to create sensor callback object");
    return FALSE;
  }

  self->event_queue = gbinder_fmq_new (sizeof (struct sensors_event_t),
                                       FMQ_EVENT_CAPACITY,
                                       GBINDER_FMQ_TYPE_SYNC_READ_WRITE,
                                       GBINDER_FMQ_FLAG_CONFIGURE_EVENT_FLAG,
                                       -1, 0);
  self->wake_lock_queue = gbinder_fmq_new (sizeof (guint32),
                                           FMQ_WAKELOCK_CAPACITY,
                                           GBINDER_FMQ_TYPE_SYNC_READ_WRITE,
                                           GBINDER_FMQ_FLAG_CONFIGURE_EVENT_FLAG,
                                           -1, 0);
  if (!self->event_queue || !self->wake_lock_queue) {
    g_warning ("Failed to create sensor FMQs");
    return FALSE;
  }

  req = gbinder_client_new_request2 (self->client, initialize_code);
  gbinder_local_request_init_writer (req, &writer);
  gbinder_writer_append_fmq_descriptor (&writer, self->event_queue);
  gbinder_writer_append_fmq_descriptor (&writer, self->wake_lock_queue);
  gbinder_writer_append_local_object (&writer, self->sensor_callback);

  reply = gbinder_client_transact_sync_reply (self->client, initialize_code, req, &tx_status);
  gbinder_local_request_unref (req);

  if (tx_status != GBINDER_STATUS_OK || !reply) {
    g_warning ("Sensor initialize transaction failed (%d)", tx_status);
    return FALSE;
  }

  ok = droid_sensor_decode_status_error_reply (reply);
  gbinder_remote_reply_unref (reply);

  if (!ok) {
    g_warning ("Sensor initialize returned error");
    return FALSE;
  }

  return TRUE;
}

static gpointer
droid_sensor_event_reader_thread (gpointer user_data)
{
  DroidSensor *self;
  struct sensors_event_t buffer[MAX_RECEIVE_BUFFER_EVENT_COUNT];

  self = DROID_SENSOR (user_data);

  while (!g_atomic_int_get (&self->stop_thread)) {
    gsize available;
    gsize num_events;
    guint32 wakeup_count;
    gsize i;

    available = gbinder_fmq_available_to_read (self->event_queue);
    if (available == 0) {
      guint32 state;
      gint32 ret;

      state = 0;
      ret = gbinder_fmq_wait (self->event_queue,
                              EVENT_QUEUE_FLAG_READ_AND_PROCESS,
                              &state);
      if (g_atomic_int_get (&self->stop_thread))
        break;

      if (ret < 0 && ret != -ETIMEDOUT && ret != -EAGAIN) {
        g_warning ("sensor FMQ wait failed: %s", g_strerror (-ret));
      }
      continue;
    }

    num_events = MIN (available, (gsize) MAX_RECEIVE_BUFFER_EVENT_COUNT);
    if (!gbinder_fmq_read (self->event_queue, buffer, num_events)) {
      g_warning ("sensor FMQ read failed");
      continue;
    }

    gbinder_fmq_wake (self->event_queue, EVENT_QUEUE_FLAG_EVENTS_READ);

    wakeup_count = 0;
    for (i = 0; i < num_events; i++) {
      guint32 flags;

      flags = sensor_flags_for_handle (self, buffer[i].sensor);
      if (flags & SENSOR_FLAG_WAKE_UP)
        wakeup_count++;
    }

    droid_sensor_queue_events (self, buffer, num_events);

    if (wakeup_count > 0) {
      if (gbinder_fmq_write (self->wake_lock_queue, &wakeup_count, 1)) {
        gbinder_fmq_wake (self->wake_lock_queue, WAKE_LOCK_QUEUE_DATA_WRITTEN);
      } else {
        g_warning ("sensor wakelock FMQ write failed");
      }
    }
  }

  return NULL;
}

static gboolean
droid_sensor_start_event_thread (DroidSensor *self)
{
  if (self->event_thread)
    return TRUE;

  g_atomic_int_set (&self->stop_thread, 0);
  self->event_thread = g_thread_new ("droid-sensor-events",
                                     droid_sensor_event_reader_thread,
                                     self);

  if (!self->event_thread) {
    g_warning ("Failed to start sensor event thread");
    return FALSE;
  }

  return TRUE;
}

static void
droid_sensor_stop_event_thread (DroidSensor *self)
{
  if (!self->event_thread)
    return;

  g_atomic_int_set (&self->stop_thread, 1);

  if (self->event_queue)
    gbinder_fmq_wake (self->event_queue, EVENT_QUEUE_FLAG_READ_AND_PROCESS);

  g_thread_join (self->event_thread);
  self->event_thread = NULL;
}

static void
droid_sensor_disconnect (DroidSensor *self,
                         gboolean     clear_events)
{
  if (self->client && self->poll_transact_id) {
    gbinder_client_cancel (self->client, self->poll_transact_id);
    self->poll_transact_id = 0;
  }

  droid_sensor_stop_event_thread (self);

  if (self->remote && self->death_id) {
    gbinder_remote_object_remove_handler (self->remote, self->death_id);
    self->death_id = 0;
  }

  if (clear_events) {
    droid_sensor_clear_queued_events (self);
  }

  if (self->sensor_callback) {
    gbinder_local_object_unref (self->sensor_callback);
  }

  if (self->wake_lock_queue) {
    gbinder_fmq_unref (self->wake_lock_queue);
  }

  if (self->event_queue) {
    gbinder_fmq_unref (self->event_queue);
  }

  if (self->client) {
    gbinder_client_unref (self->client);
  }

  if (self->remote) {
    gbinder_remote_object_unref (self->remote);
  }
}

static gboolean
droid_sensor_try_connect_2x (DroidSensor *self,
                             const char  *service_name,
                             DroidSensorInterface iface,
                             guint        initialize_code)
{
  gboolean init_ok;

  self->remote = gbinder_servicemanager_get_service_sync (self->service_manager,
                                                          service_name,
                                                          NULL);
  if (!self->remote)
    return FALSE;
  gbinder_remote_object_ref (self->remote);

  self->iface = iface;
  self->client = gbinder_client_new2 (self->remote,
                                      sensor_2_client_ifaces,
                                      G_N_ELEMENTS (sensor_2_client_ifaces));
  if (!self->client)
    return FALSE;

  init_ok = droid_sensor_init_2x (self, initialize_code);
  if (!init_ok || !droid_sensor_load_sensors (self))
    return FALSE;

  self->death_id = gbinder_remote_object_add_death_handler (self->remote,
                                                            sensor_binder_died,
                                                            self);
  return droid_sensor_start_event_thread (self);
}

static gboolean
droid_sensor_try_connect_1_0 (DroidSensor *self)
{
  int32_t tx_status = 0;
  GBinderLocalRequest *req;
  GBinderRemoteReply *reply;

  self->remote = gbinder_servicemanager_get_service_sync (self->service_manager,
                                                          BINDER_SENSOR_NAME_1_0,
                                                          NULL);
  if (!self->remote)
    return FALSE;
  gbinder_remote_object_ref (self->remote);

  self->iface = DROID_SENSOR_INTERFACE_1_0;
  self->client = gbinder_client_new (self->remote, BINDER_SENSOR_IFACE_1_0);
  if (!self->client)
    return FALSE;

  req = gbinder_client_new_request2 (self->client, POLL);
  gbinder_local_request_append_int32 (req, 0);
  reply = gbinder_client_transact_sync_reply (self->client, POLL, req, &tx_status);
  gbinder_local_request_unref (req);
  if (reply)
    gbinder_remote_reply_unref (reply);

  if (tx_status != GBINDER_STATUS_OK || !droid_sensor_load_sensors (self))
    return FALSE;

  self->death_id = gbinder_remote_object_add_death_handler (self->remote,
                                                            sensor_binder_died,
                                                            self);
  droid_sensor_poll_events_1_0 (self);
  return TRUE;
}

static gboolean
droid_sensor_connect (DroidSensor *self)
{
  droid_sensor_disconnect (self, FALSE);

  if (!self->service_manager) {
    self->service_manager = gbinder_servicemanager_new (BINDER_SENSOR_DEVICE);
    if (!self->service_manager) {
      g_warning ("Failed to create sensor service manager");
      return FALSE;
    }
  }

  if (droid_sensor_try_connect_2x (self,
                                   BINDER_SENSOR_NAME_2_1,
                                   DROID_SENSOR_INTERFACE_2_1,
                                   INITIALIZE_2_1))
    return TRUE;

  droid_sensor_disconnect (self, FALSE);

  if (droid_sensor_try_connect_2x (self,
                                   BINDER_SENSOR_NAME_2_0,
                                   DROID_SENSOR_INTERFACE_2_0,
                                   INITIALIZE_2_0))
    return TRUE;

  droid_sensor_disconnect (self, FALSE);

  if (droid_sensor_try_connect_1_0 (self))
    return TRUE;

  droid_sensor_disconnect (self, FALSE);
  return FALSE;
}

static gboolean
droid_sensor_start_connect (DroidSensor *self)
{
  if (!self->service_manager) {
    self->service_manager = gbinder_servicemanager_new (BINDER_SENSOR_DEVICE);
    if (!self->service_manager)
      return FALSE;
  }

  if (!gbinder_servicemanager_wait (self->service_manager, -1)) {
    g_warning ("Could not get sensor service manager");
    return FALSE;
  }

  while (!droid_sensor_connect (self)) {
    g_usleep (1000 * 1000);
  }
  return TRUE;
}

static gboolean
droid_sensor_ensure_connected (DroidSensor *self)
{
  if (self->client && self->remote)
    return TRUE;

  return droid_sensor_start_connect (self);
}

static gboolean
initable_init (GInitable    *initable,
               GCancellable *cancellable,
               GError      **error)
{
  DroidSensor *self;

  (void)cancellable;

  self = DROID_SENSOR (initable);

  g_debug ("Initializing droid sensor");

  if (!droid_sensor_ensure_connected (self)) {
    g_set_error (error,
                 G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Failed to initialize binder sensor service");
    return FALSE;
  }

  return TRUE;
}

static void
droid_sensor_constructed (GObject *obj)
{
  DroidSensor *self;

  self = DROID_SENSOR (obj);

  G_OBJECT_CLASS (droid_sensor_parent_class)->constructed (obj);

  self->event_queue_local = g_async_queue_new ();
}

static void
droid_sensor_dispose (GObject *obj)
{
  DroidSensor *self;

  self = DROID_SENSOR (obj);

  g_debug ("Disposing droid sensor");

  droid_sensor_disconnect (self, TRUE);

  droid_sensor_clear_sensors (self);

  g_clear_pointer (&self->event_queue_local, g_async_queue_unref);
  g_clear_pointer (&self->service_manager, gbinder_servicemanager_unref);

  G_OBJECT_CLASS (droid_sensor_parent_class)->dispose (obj);
}

static void
droid_sensor_finalize (GObject *obj)
{
  G_OBJECT_CLASS (droid_sensor_parent_class)->finalize (obj);
}

static void
droid_sensor_class_init (DroidSensorClass *klass)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = droid_sensor_constructed;
  object_class->dispose = droid_sensor_dispose;
  object_class->finalize = droid_sensor_finalize;
}

static void
initable_interface_init (GInitableIface *iface)
{
  iface->init = initable_init;
}

static void
droid_sensor_init (DroidSensor *self)
{
  (void)self;
}

DroidSensor *
droid_sensor_new (GError **error)
{
  return DROID_SENSOR (
    g_initable_new (DROID_TYPE_SENSOR,
                    NULL,
                    error,
                    NULL));
}

uint32_t
droid_sensor_get_count (DroidSensor *self)
{
  g_return_val_if_fail (DROID_IS_SENSOR (self), 0);
  if (!droid_sensor_ensure_connected (self))
    return 0;

  return self->sensor_count;
}

const DroidSensorInfo *
droid_sensor_get_info (DroidSensor *self,
                       uint32_t     index)
{
  g_return_val_if_fail (DROID_IS_SENSOR (self), NULL);
  if (!droid_sensor_ensure_connected (self))
    return NULL;
  g_return_val_if_fail (index < self->sensor_count, NULL);

  return &self->sensors[index];
}

gboolean
droid_sensor_set_active (DroidSensor *self,
                         int32_t      handle,
                         gboolean     active)
{
  GBinderLocalRequest *req;
  GBinderRemoteReply *reply;
  GBinderWriter writer;
  int32_t tx_status = 0;
  gboolean ok;

  g_return_val_if_fail (DROID_IS_SENSOR (self), FALSE);
  if (!droid_sensor_ensure_connected (self))
    return FALSE;
  if (active && !droid_sensor_set_delay_internal (self, handle, DEFAULT_SENSOR_DELAY_NS, 0))
    return FALSE;

  req = gbinder_client_new_request2 (self->client, ACTIVATE);
  gbinder_local_request_init_writer (req, &writer);
  gbinder_writer_append_int32 (&writer, handle);
  gbinder_writer_append_int32 (&writer, active);

  reply = gbinder_client_transact_sync_reply (self->client, ACTIVATE, req, &tx_status);
  gbinder_local_request_unref (req);

  if (tx_status != GBINDER_STATUS_OK || !reply)
    return FALSE;

  ok = droid_sensor_decode_status_error_reply (reply);
  gbinder_remote_reply_unref (reply);

  return ok;
}

static gboolean
droid_sensor_set_delay_internal (DroidSensor *self,
                                 int32_t      handle,
                                 int64_t      sampling_period_ns,
                                 int64_t      max_report_latency_ns)
{
  GBinderLocalRequest *req;
  GBinderRemoteReply *reply;
  GBinderWriter writer;
  int32_t tx_status = 0;
  gboolean ok;

  g_return_val_if_fail (DROID_IS_SENSOR (self), FALSE);
  if (!droid_sensor_ensure_connected (self))
    return FALSE;

  req = gbinder_client_new_request2 (self->client, BATCH);
  gbinder_local_request_init_writer (req, &writer);
  gbinder_writer_append_int32 (&writer, handle);
  gbinder_writer_append_int64 (&writer, sampling_period_ns);
  gbinder_writer_append_int64 (&writer, max_report_latency_ns);

  reply = gbinder_client_transact_sync_reply (self->client, BATCH, req, &tx_status);
  gbinder_local_request_unref (req);

  if (tx_status != GBINDER_STATUS_OK || !reply)
    return FALSE;

  ok = droid_sensor_decode_status_error_reply (reply);
  gbinder_remote_reply_unref (reply);

  return ok;
}

int
droid_sensor_poll (DroidSensor      *self,
                   DroidSensorEvent *events,
                   uint32_t          max_events)
{
  uint32_t out_count;

  g_return_val_if_fail (DROID_IS_SENSOR (self), -1);
  g_return_val_if_fail (events != NULL, -1);
  if (!droid_sensor_ensure_connected (self))
    return -1;

  out_count = 0;

  while (out_count < max_events) {
    DroidSensorEvent *ev;

    ev = g_async_queue_try_pop (self->event_queue_local);
    if (!ev)
      break;
    events[out_count] = *ev;
    g_free (ev);
    out_count++;
  }

  return (int) out_count;
}

int
droid_sensor_poll_blocking (DroidSensor      *self,
                            DroidSensorEvent *events,
                            uint32_t          max_events,
                            int               timeout_ms)
{
  uint32_t out_count;
  DroidSensorEvent *ev;

  g_return_val_if_fail (DROID_IS_SENSOR (self), -1);
  g_return_val_if_fail (events != NULL, -1);
  if (!droid_sensor_ensure_connected (self))
    return -1;
  if (max_events == 0)
    return 0;

  if (timeout_ms < 0) {
    ev = g_async_queue_pop (self->event_queue_local);
  } else if (timeout_ms == 0) {
    ev = g_async_queue_try_pop (self->event_queue_local);
  } else {
    ev = g_async_queue_timeout_pop (self->event_queue_local,
                                    (guint64) timeout_ms * 1000);
  }

  if (!ev)
    return 0;

  out_count = 0;
  events[out_count++] = *ev;
  g_free (ev);

  while (out_count < max_events) {
    ev = g_async_queue_try_pop (self->event_queue_local);
    if (!ev)
      break;
    events[out_count++] = *ev;
    g_free (ev);
  }

  return (int) out_count;
}

DroidSensorHalVersion
droid_sensor_get_hal_version (DroidSensor *self)
{
  g_return_val_if_fail (DROID_IS_SENSOR (self), DROID_SENSOR_HAL_UNKNOWN);
  if (!droid_sensor_ensure_connected (self))
    return DROID_SENSOR_HAL_UNKNOWN;

  switch (self->iface) {
    case DROID_SENSOR_INTERFACE_1_0:
      return DROID_SENSOR_HAL_1_0;
    case DROID_SENSOR_INTERFACE_2_0:
      return DROID_SENSOR_HAL_2_0;
    case DROID_SENSOR_INTERFACE_2_1:
      return DROID_SENSOR_HAL_2_1;
    case DROID_SENSOR_INTERFACE_COUNT:
      return DROID_SENSOR_HAL_UNKNOWN;
    default:
      return DROID_SENSOR_HAL_UNKNOWN;
  }
}

const char *
droid_sensor_hal_version_name (DroidSensorHalVersion version)
{
  switch (version) {
    case DROID_SENSOR_HAL_UNKNOWN:
      return "unknown";
    case DROID_SENSOR_HAL_1_0:
      return "1.0";
    case DROID_SENSOR_HAL_2_0:
      return "2.0";
    case DROID_SENSOR_HAL_2_1:
      return "2.1";
    default:
      return "unknown";
  }
}

const char *
droid_sensor_type_name (int32_t type)
{
  switch (type) {
    case DROID_SENSOR_TYPE_ACCELEROMETER:
      return "accelerometer";
    case DROID_SENSOR_TYPE_MAGNETIC_FIELD:
      return "magnetic_field";
    case DROID_SENSOR_TYPE_ORIENTATION:
      return "orientation";
    case DROID_SENSOR_TYPE_GYROSCOPE:
      return "gyroscope";
    case DROID_SENSOR_TYPE_LIGHT:
      return "light";
    case DROID_SENSOR_TYPE_PRESSURE:
      return "pressure";
    case DROID_SENSOR_TYPE_TEMPERATURE:
      return "temperature";
    case DROID_SENSOR_TYPE_PROXIMITY:
      return "proximity";
    case DROID_SENSOR_TYPE_GRAVITY:
      return "gravity";
    case DROID_SENSOR_TYPE_LINEAR_ACCELERATION:
      return "linear_acceleration";
    case DROID_SENSOR_TYPE_ROTATION_VECTOR:
      return "rotation_vector";
    case DROID_SENSOR_TYPE_RELATIVE_HUMIDITY:
      return "relative_humidity";
    case DROID_SENSOR_TYPE_AMBIENT_TEMPERATURE:
      return "ambient_temperature";
    case DROID_SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED:
      return "magnetic_field_uncalibrated";
    case DROID_SENSOR_TYPE_GAME_ROTATION_VECTOR:
      return "game_rotation_vector";
    case DROID_SENSOR_TYPE_GYROSCOPE_UNCALIBRATED:
      return "gyroscope_uncalibrated";
    case DROID_SENSOR_TYPE_SIGNIFICANT_MOTION:
      return "significant_motion";
    case DROID_SENSOR_TYPE_STEP_DETECTOR:
      return "step_detector";
    case DROID_SENSOR_TYPE_STEP_COUNTER:
      return "step_counter";
    case DROID_SENSOR_TYPE_GEOMAGNETIC_ROTATION_VECTOR:
      return "geomagnetic_rotation_vector";
    case DROID_SENSOR_TYPE_HEART_RATE:
      return "heart_rate";
    case DROID_SENSOR_TYPE_TILT_DETECTOR:
      return "tilt_detector";
    case DROID_SENSOR_TYPE_WAKE_GESTURE:
      return "wake_gesture";
    case DROID_SENSOR_TYPE_GLANCE_GESTURE:
      return "glance_gesture";
    case DROID_SENSOR_TYPE_PICK_UP_GESTURE:
      return "pick_up_gesture";
    case DROID_SENSOR_TYPE_WRIST_TILT_GESTURE:
      return "wrist_tilt_gesture";
    case DROID_SENSOR_TYPE_DEVICE_ORIENTATION:
      return "device_orientation";
    case DROID_SENSOR_TYPE_POSE_6DOF:
      return "pose_6dof";
    case DROID_SENSOR_TYPE_STATIONARY_DETECT:
      return "stationary_detect";
    case DROID_SENSOR_TYPE_MOTION_DETECT:
      return "motion_detect";
    case DROID_SENSOR_TYPE_HEART_BEAT:
      return "heart_beat";
    case DROID_SENSOR_TYPE_DYNAMIC_SENSOR_META:
      return "dynamic_sensor_meta";
    case DROID_SENSOR_TYPE_ADDITIONAL_INFO:
      return "additional_info";
    case DROID_SENSOR_TYPE_LOW_LATENCY_OFFBODY_DETECT:
      return "low_latency_offbody_detect";
    case DROID_SENSOR_TYPE_ACCELEROMETER_UNCALIBRATED:
      return "accelerometer_uncalibrated";
    case DROID_SENSOR_TYPE_HINGE_ANGLE:
      return "hinge_angle";
    case DROID_SENSOR_TYPE_HEAD_TRACKER:
      return "head_tracker";
    default:
      return "other";
  }
}

gboolean
droid_sensor_type_is_orientation (int32_t type)
{
  return (type == DROID_SENSOR_TYPE_ORIENTATION ||
          type == DROID_SENSOR_TYPE_DEVICE_ORIENTATION);
}
