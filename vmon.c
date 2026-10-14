/*
 * vmon - keep a mutter virtual monitor alive
 *
 * usage: vmon WxH@HZ[*] [WxH@HZ[*] ...]
 *
 * The monitor is created with org.gnome.Mutter.ScreenCast.RecordVirtual and
 * lives as long as this process. Its stream must have a consumer, and vmon is
 * that consumer. vmon also drives the stream: left to its own clock, a virtual
 * monitor never paints for clients that time their commits (Vulkan FIFO
 * presentation), while with vmon as the PipeWire driver mutter paints it on
 * request. Each request becomes one cycle, no faster than the refresh rate of
 * the current mode, and every buffer goes straight back.
 *
 * Modes are given as WIDTHxHEIGHT@HZ; '*' marks the preferred one, which is
 * the first one when none is marked.
 */

#include <gio/gio.h>
#include <glib-unix.h>
#include <pipewire/pipewire.h>
#include <spa/node/command.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/dynamic.h>

#define SCREEN_CAST "org.gnome.Mutter.ScreenCast"
#define SCREEN_CAST_PATH "/org/gnome/Mutter/ScreenCast"
#define SESSION_IFACE SCREEN_CAST ".Session"
#define STREAM_IFACE SCREEN_CAST ".Stream"

/* Hidden or metadata cursors make mutter drop the cursor from the view. */
#define CURSOR_MODE_EMBEDDED 1

typedef struct {
  GMainLoop *main_loop;
  GDBusConnection *bus;
  int exit_status;

  struct pw_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct pw_registry *registry;
  struct spa_hook core_listener;

  struct pw_node *node;
  struct spa_hook node_listener;
  char *node_serial;
  GPtrArray *formats;
  int sync_seq;

  struct pw_stream *stream;
  struct spa_hook stream_listener;
  struct spa_source *timer;
  uint64_t frame_interval_ns;
  uint64_t next_cycle_ns;
  gboolean cycle_pending;
} Vmon;

static void
quit (Vmon *vmon, int status)
{
  vmon->exit_status = status;
  g_main_loop_quit (vmon->main_loop);
}

/* Pacing: one cycle per refresh interval, only when mutter asks for one. */

static uint64_t
now_ns (void)
{
  return (uint64_t) g_get_monotonic_time () * SPA_NSEC_PER_USEC;
}

static void
run_cycle (Vmon *vmon)
{
  vmon->cycle_pending = FALSE;
  vmon->next_cycle_ns = now_ns () + vmon->frame_interval_ns;
  pw_stream_trigger_process (vmon->stream);
}

static void
request_cycle (Vmon *vmon)
{
  struct timespec at;

  if (vmon->cycle_pending)
    return;

  if (now_ns () >= vmon->next_cycle_ns)
    {
      run_cycle (vmon);
      return;
    }

  at.tv_sec = vmon->next_cycle_ns / SPA_NSEC_PER_SEC;
  at.tv_nsec = vmon->next_cycle_ns % SPA_NSEC_PER_SEC;
  vmon->cycle_pending = TRUE;
  pw_loop_update_timer (vmon->loop, vmon->timer, &at, NULL, true);
}

static void
on_timer (void *data, uint64_t expirations)
{
  run_cycle (data);
}

/* Consumer stream */

static void
on_stream_param_changed (void *data, uint32_t id, const struct spa_pod *param)
{
  Vmon *vmon = data;
  struct spa_video_info_raw info = { 0 };

  if (id == SPA_PARAM_Format && param &&
      spa_format_video_raw_parse (param, &info) >= 0 &&
      info.max_framerate.num > 0)
    vmon->frame_interval_ns = SPA_NSEC_PER_SEC * info.max_framerate.denom /
                              info.max_framerate.num;
}

static void
on_stream_command (void *data, const struct spa_command *command)
{
  if (SPA_NODE_COMMAND_ID (command) == SPA_NODE_COMMAND_RequestProcess)
    request_cycle (data);
}

static void
on_stream_process (void *data)
{
  Vmon *vmon = data;
  struct pw_buffer *buffer;

  while ((buffer = pw_stream_dequeue_buffer (vmon->stream)))
    pw_stream_queue_buffer (vmon->stream, buffer);
}

static void
on_stream_state_changed (void *data, enum pw_stream_state old,
                         enum pw_stream_state state, const char *error)
{
  switch (state)
    {
    case PW_STREAM_STATE_STREAMING:
      /* PipeWire drops requests sent before the driver runs, and mutter
       * waits for its first one to be served; start with a cycle. */
      run_cycle (data);
      break;
    case PW_STREAM_STATE_ERROR:
    case PW_STREAM_STATE_UNCONNECTED:
      g_warning ("stream %s: %s", pw_stream_state_as_string (state),
                 error ? error : "disconnected");
      quit (data, 1);
      break;
    default:
      break;
    }
}

static const struct pw_stream_events stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = on_stream_state_changed,
  .param_changed = on_stream_param_changed,
  .command = on_stream_command,
  .process = on_stream_process,
};

/* Mutter's stream asks to drive too; the higher priority makes vmon the
 * driver, which is what puts the monitor's frame clock in passive mode. */
static void
connect_stream (Vmon *vmon)
{
  vmon->stream = pw_stream_new (vmon->core, "vmon",
                                pw_properties_new (PW_KEY_MEDIA_TYPE, "Video",
                                                   PW_KEY_MEDIA_CATEGORY, "Capture",
                                                   PW_KEY_MEDIA_ROLE, "Screen",
                                                   PW_KEY_TARGET_OBJECT, vmon->node_serial,
                                                   PW_KEY_PRIORITY_DRIVER, "1",
                                                   NULL));
  pw_stream_add_listener (vmon->stream, &vmon->stream_listener,
                          &stream_events, vmon);
  pw_stream_connect (vmon->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                     PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_DRIVER,
                     (const struct spa_pod **) vmon->formats->pdata,
                     vmon->formats->len);
}

/* Producer node: its serial and the formats (including DMA-BUF modifiers)
 * it offers, which the consumer accepts. */

static void
on_node_info (void *data, const struct pw_node_info *info)
{
  Vmon *vmon = data;
  const char *serial = spa_dict_lookup (info->props, PW_KEY_OBJECT_SERIAL);

  if (serial && !vmon->node_serial)
    vmon->node_serial = g_strdup (serial);
}

/* Mutter's formats pin the current mode's size and rate; leave both out so
 * the stream renegotiates instead of failing when the mode changes. */
static struct spa_pod *
copy_format_any_size (const struct spa_pod *param)
{
  const struct spa_pod_object *object = (const struct spa_pod_object *) param;
  const struct spa_pod_prop *prop;
  struct spa_pod_dynamic_builder builder;
  struct spa_pod_frame frame;
  struct spa_pod *copy;

  spa_pod_dynamic_builder_init (&builder, NULL, 0, 1024);
  spa_pod_builder_push_object (&builder.b, &frame, object->body.type, object->body.id);
  SPA_POD_OBJECT_FOREACH (object, prop)
    {
      if (prop->key == SPA_FORMAT_VIDEO_size ||
          prop->key == SPA_FORMAT_VIDEO_framerate ||
          prop->key == SPA_FORMAT_VIDEO_maxFramerate)
        continue;
      spa_pod_builder_raw_padded (&builder.b, prop, SPA_POD_PROP_SIZE (prop));
    }
  copy = spa_pod_copy (spa_pod_builder_pop (&builder.b, &frame));
  spa_pod_dynamic_builder_clean (&builder);
  return copy;
}

static void
on_node_param (void *data, int seq, uint32_t id, uint32_t index,
               uint32_t next, const struct spa_pod *param)
{
  Vmon *vmon = data;

  if (id == SPA_PARAM_EnumFormat)
    g_ptr_array_add (vmon->formats, copy_format_any_size (param));
}

static const struct pw_node_events node_events = {
  PW_VERSION_NODE_EVENTS,
  .info = on_node_info,
  .param = on_node_param,
};

static void
inspect_node (Vmon *vmon, uint32_t node_id)
{
  vmon->node = pw_registry_bind (vmon->registry, node_id,
                                 PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
  pw_node_add_listener (vmon->node, &vmon->node_listener, &node_events, vmon);
  pw_node_enum_params (vmon->node, 0, SPA_PARAM_EnumFormat, 0, UINT32_MAX, NULL);
  vmon->sync_seq = pw_core_sync (vmon->core, PW_ID_CORE, 0);
}

static void
on_core_done (void *data, uint32_t id, int seq)
{
  Vmon *vmon = data;

  if (id != PW_ID_CORE || seq != vmon->sync_seq || vmon->stream)
    return;

  if (!vmon->node_serial || vmon->formats->len == 0)
    {
      g_warning ("screen cast node did not describe itself");
      quit (vmon, 1);
      return;
    }

  connect_stream (vmon);
}

static void
on_core_error (void *data, uint32_t id, int seq, int res, const char *message)
{
  g_warning ("pipewire: %s", message);
  if (id == PW_ID_CORE)
    quit (data, 1);
}

static const struct pw_core_events core_events = {
  PW_VERSION_CORE_EVENTS,
  .done = on_core_done,
  .error = on_core_error,
};

static gboolean
on_pipewire_fd (int fd, GIOCondition condition, gpointer data)
{
  Vmon *vmon = data;

  pw_loop_iterate (vmon->loop, 0);
  return G_SOURCE_CONTINUE;
}

static void
connect_pipewire (Vmon *vmon)
{
  vmon->loop = pw_loop_new (NULL);
  pw_loop_enter (vmon->loop);
  g_unix_fd_add (pw_loop_get_fd (vmon->loop), G_IO_IN, on_pipewire_fd, vmon);

  vmon->context = pw_context_new (vmon->loop, NULL, 0);
  vmon->core = pw_context_connect (vmon->context, NULL, 0);
  if (!vmon->core)
    g_error ("cannot connect to pipewire");

  pw_core_add_listener (vmon->core, &vmon->core_listener, &core_events, vmon);
  vmon->registry = pw_core_get_registry (vmon->core, PW_VERSION_REGISTRY, 0);
  vmon->timer = pw_loop_add_timer (vmon->loop, on_timer, vmon);
  vmon->formats = g_ptr_array_new_with_free_func (free);
  vmon->frame_interval_ns = SPA_NSEC_PER_SEC / 60;
}

/* Mutter screen cast session */

static GVariant *
call (Vmon *vmon, const char *path, const char *iface, const char *method,
      GVariant *args, const GVariantType *reply_type)
{
  g_autoptr (GError) error = NULL;
  GVariant *reply;

  reply = g_dbus_connection_call_sync (vmon->bus, SCREEN_CAST, path, iface,
                                       method, args, reply_type,
                                       G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
  if (!reply)
    g_error ("%s: %s", method, error->message);
  return reply;
}

static void
on_stream_added (GDBusConnection *bus, const char *sender, const char *path,
                 const char *iface, const char *signal, GVariant *params,
                 gpointer data)
{
  uint32_t node_id;

  g_variant_get (params, "(u)", &node_id);
  inspect_node (data, node_id);
}

static void
on_session_closed (GDBusConnection *bus, const char *sender, const char *path,
                   const char *iface, const char *signal, GVariant *params,
                   gpointer data)
{
  g_warning ("screen cast session closed");
  quit (data, 1);
}

static GVariant *
parse_modes (int n_specs, char **specs)
{
  GVariantBuilder modes;
  gboolean any_marked = FALSE;
  int i;

  for (i = 0; i < n_specs; i++)
    any_marked |= g_str_has_suffix (specs[i], "*");

  g_variant_builder_init (&modes, G_VARIANT_TYPE ("aa{sv}"));
  for (i = 0; i < n_specs; i++)
    {
      unsigned int width, height;
      double refresh_rate;
      char star = 0;
      gboolean preferred;

      if (sscanf (specs[i], "%ux%u@%lf%c", &width, &height, &refresh_rate,
                  &star) < 3 || (star && star != '*'))
        g_error ("bad mode '%s', expected WxH@HZ[*]", specs[i]);

      preferred = any_marked ? star == '*' : i == 0;
      g_variant_builder_add_parsed (&modes,
                                    "{'size': <(%u, %u)>,"
                                    " 'refresh-rate': <%d>,"
                                    " 'is-preferred': <%b>}",
                                    width, height, refresh_rate, preferred);
    }

  return g_variant_builder_end (&modes);
}

static void
start_session (Vmon *vmon, GVariant *modes)
{
  g_autoptr (GVariant) session = NULL;
  g_autoptr (GVariant) stream = NULL;
  const char *session_path, *stream_path;

  session = call (vmon, SCREEN_CAST_PATH, SCREEN_CAST, "CreateSession",
                  g_variant_new_parsed ("(@a{sv} {},)"), G_VARIANT_TYPE ("(o)"));
  g_variant_get (session, "(&o)", &session_path);

  stream = call (vmon, session_path, SESSION_IFACE, "RecordVirtual",
                 g_variant_new_parsed ("({'cursor-mode': <%u>,"
                                       "  'is-platform': <true>,"
                                       "  'modes': <%@aa{sv}>},)",
                                       CURSOR_MODE_EMBEDDED, modes),
                 G_VARIANT_TYPE ("(o)"));
  g_variant_get (stream, "(&o)", &stream_path);

  g_dbus_connection_signal_subscribe (vmon->bus, SCREEN_CAST, STREAM_IFACE,
                                      "PipeWireStreamAdded", stream_path, NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      on_stream_added, vmon, NULL);
  g_dbus_connection_signal_subscribe (vmon->bus, SCREEN_CAST, SESSION_IFACE,
                                      "Closed", session_path, NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      on_session_closed, vmon, NULL);

  g_variant_unref (call (vmon, session_path, SESSION_IFACE, "Start", NULL, NULL));
}

static gboolean
on_signal (gpointer data)
{
  quit (data, 0);
  return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
  Vmon vmon = { 0 };
  g_autoptr (GError) error = NULL;

  if (argc < 2)
    {
      g_printerr ("usage: %s WxH@HZ[*] [WxH@HZ[*] ...]\n", argv[0]);
      return 2;
    }

  pw_init (&argc, &argv);
  vmon.main_loop = g_main_loop_new (NULL, FALSE);
  vmon.bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!vmon.bus)
    g_error ("session bus: %s", error->message);

  g_unix_signal_add (SIGINT, on_signal, &vmon);
  g_unix_signal_add (SIGTERM, on_signal, &vmon);

  connect_pipewire (&vmon);
  start_session (&vmon, parse_modes (argc - 1, argv + 1));
  g_main_loop_run (vmon.main_loop);

  /* Mutter removes the monitor when this process leaves the bus. */
  return vmon.exit_status;
}
