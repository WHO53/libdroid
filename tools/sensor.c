/* sensor.c
 *
 * Copyright 2026 Deepak Meena <who53@disroot.org>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <libdroid/libdroid.h>

#include <glib.h>
#include <signal.h>
#include <stdio.h>

typedef struct _SelectedHandle {
  int32_t handle;
  int32_t type;
} SelectedHandle;

static volatile sig_atomic_t keep_running = 1;

static void
signal_handler (int signo)
{
  (void)signo;
  keep_running = 0;
}

static const char *
sensor_type_name (int32_t type)
{
  if (type == DROID_SENSOR_TYPE_MAGNETIC_FIELD)
    return "compass";
  if (droid_sensor_type_is_orientation (type))
    return "orientation";
  return droid_sensor_type_name (type);
}

static gboolean
type_selected (int32_t type,
               gboolean proximity,
               gboolean compass,
               gboolean orientation)
{
  if (type == DROID_SENSOR_TYPE_PROXIMITY)
    return proximity;
  if (type == DROID_SENSOR_TYPE_MAGNETIC_FIELD)
    return compass;
  if (droid_sensor_type_is_orientation (type))
    return orientation;
  return FALSE;
}

int
main (int argc, char **argv)
{
  gboolean show_list = FALSE;
  gboolean dump = FALSE;
  gboolean proximity = FALSE;
  gboolean compass = FALSE;
  gboolean orientation = FALSE;
  gint count = 0;
  g_autoptr (GError) err = NULL;
  g_autoptr (GOptionContext) context = NULL;
  g_autoptr (DroidSensor) sensor = NULL;
  GArray *selected = NULL;
  uint32_t sensor_count;
  gint seen = 0;
  uint32_t i;

  const GOptionEntry entries[] = {
    {
      "list", 'l', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &show_list,
      "List all available sensors and exit", NULL,
    },
    {
      "dump", 'd', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &dump,
      "Activate and dump all available sensors", NULL,
    },
    {
      "proximity", 'p', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &proximity,
      "Enable proximity sensor testing", NULL,
    },
    {
      "compass", 'c', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &compass,
      "Enable compass (magnetic field) sensor testing", NULL,
    },
    {
      "orientation", 'o', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &orientation,
      "Enable orientation sensor testing", NULL,
    },
    {
      "count", 'n', G_OPTION_FLAG_NONE, G_OPTION_ARG_INT, &count,
      "Stop after N printed samples (0 = unlimited)", "N",
    },
    {NULL},
  };

  context = g_option_context_new ("--proximity --compass --orientation --dump");
  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_parse (context, &argc, &argv, &err);

  if (err != NULL) {
    g_error ("Unable to parse arguments: %s", err->message);
    return EXIT_FAILURE;
  }

  if (!show_list && !dump && !proximity && !compass && !orientation) {
    proximity = TRUE;
    compass = TRUE;
    orientation = TRUE;
  }

  sensor = droid_sensor_new (&err);
  if (err != NULL) {
    g_error ("Unable to initialize DroidSensor: %s", err->message);
    return EXIT_FAILURE;
  }

  sensor_count = droid_sensor_get_count (sensor);
  printf ("Found %u sensors\n", sensor_count);
  printf ("Sensor HAL interface: %s\n",
          droid_sensor_hal_version_name (droid_sensor_get_hal_version (sensor)));
  for (i = 0; i < sensor_count; i++) {
    const DroidSensorInfo *info = droid_sensor_get_info (sensor, i);
    if (!info)
      continue;
    printf ("[%u] handle=%d type=%d (%s) name=\"%s\" vendor=\"%s\"\n",
            i,
            info->handle,
            info->type,
            sensor_type_name (info->type),
            info->name ? info->name : "unknown",
            info->vendor ? info->vendor : "unknown");
  }

  if (show_list)
    return EXIT_SUCCESS;

  selected = g_array_new (FALSE, FALSE, sizeof (SelectedHandle));
  for (i = 0; i < sensor_count; i++) {
    SelectedHandle sel;
    const DroidSensorInfo *info = droid_sensor_get_info (sensor, i);

    if (!info || (!dump && !type_selected (info->type, proximity, compass, orientation)))
      continue;

    sel.handle = info->handle;
    sel.type = info->type;
    g_array_append_val (selected, sel);

    if (!droid_sensor_set_active (sensor, info->handle, TRUE))
      g_warning ("Failed to activate handle %d", info->handle);
    else
      g_message ("Activated handle=%d type=%d (%s)",
                 info->handle, info->type, sensor_type_name (info->type));
  }

  if (selected->len == 0) {
    g_warning ("No matching sensors found for requested test set");
    g_array_unref (selected);
    return EXIT_FAILURE;
  }

  signal (SIGINT, signal_handler);
  signal (SIGTERM, signal_handler);

  while (keep_running) {
    DroidSensorEvent events[16];
    int n;
    int j;

    n = droid_sensor_poll_blocking (sensor, events, G_N_ELEMENTS (events), -1);
    if (n < 0) {
      g_warning ("poll failed");
      continue;
    }
    if (n == 0)
      continue;

    for (j = 0; j < n; j++) {
      gboolean print = FALSE;
      uint32_t k;
      const char *name = sensor_type_name (events[j].sensor_type);

      for (k = 0; k < selected->len; k++) {
        SelectedHandle *sel = &g_array_index (selected, SelectedHandle, k);
        if (sel->handle == events[j].sensor_handle) {
          print = TRUE;
          break;
        }
      }
      if (!print)
        continue;

      printf ("ts=%" G_GINT64_FORMAT " handle=%d type=%d (%s) data=[%.6f %.6f %.6f %.6f]\n",
              events[j].timestamp_ns,
              events[j].sensor_handle,
              events[j].sensor_type,
              name,
              events[j].data[0],
              events[j].data[1],
              events[j].data[2],
              events[j].data[3]);
      fflush (stdout);

      seen++;
      if (count > 0 && seen >= count) {
        keep_running = 0;
        break;
      }
    }
  }

  for (i = 0; i < selected->len; i++) {
    SelectedHandle *sel = &g_array_index (selected, SelectedHandle, i);
    droid_sensor_set_active (sensor, sel->handle, FALSE);
  }

  g_array_unref (selected);
  return EXIT_SUCCESS;
}
