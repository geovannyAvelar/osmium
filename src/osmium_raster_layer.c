/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_raster_layer.h"

#include "raster/raster.h"
#include "tms/tms.h"
#include <math.h>
#include <gio/gio.h>

#define OSMIUM_RASTER_LAYER_RELOAD_DELAY_MS 150
#define OSMIUM_RASTER_LAYER_MAX_SIZE 4096
#define WEB_MERCATOR_HALF_WORLD 20037508.342789244
#define WEB_MERCATOR_RADIUS 6378137.0

typedef struct {
  char *path;
  char *user_agent;
  guint generation;
} OpenJob;

typedef struct {
  OsmiumRaster *raster;
  guint id;
  guint zoom;
  double x;
  double y;
  int width;
  int height;
} RenderJob;

typedef struct {
  GdkTexture *texture;
  guint id;
  guint zoom;
  double x;
  double y;
  int width;
  int height;
} RasterImage;

struct _OsmiumRasterLayer {
  GtkWidget parent_instance;

  OsmiumMap *map; /* weak */
  gulong view_handler;

  char *path;
  char *user_agent;
  gboolean loading;

  guint generation;
  OsmiumRaster *raster;
  guint reload_source;
  guint last_request_id;
  RasterImage *image;
  GCancellable *cancellable;
};

OSMIUM_API GType osmium_raster_layer_get_type(void);

G_DEFINE_TYPE(OsmiumRasterLayer, osmium_raster_layer, GTK_TYPE_WIDGET)

enum { PROP_0, PROP_MAP, PROP_PATH, PROP_USER_AGENT, PROP_LOADING, PROP_HAS_EXTENT, N_PROPS };

enum { SIGNAL_LOAD_FAILED, N_SIGNALS };

static GParamSpec *properties[N_PROPS];
static guint signals[N_SIGNALS];

static void osmium_raster_layer_schedule_render(OsmiumRasterLayer *self);

static void image_free(gpointer data) {
  RasterImage *image = data;

  if (image == NULL) {
    return;
  }
  g_clear_object(&image->texture);
  g_free(image);
}

static void open_job_free(gpointer data) {
  OpenJob *job = data;

  g_free(job->path);
  g_free(job->user_agent);
  g_free(job);
}

static void render_job_free(gpointer data) {
  RenderJob *job = data;

  osmium_raster_unref(job->raster);
  g_free(job);
}

static void set_loading(OsmiumRasterLayer *self, gboolean loading) {
  if (self->loading == loading) {
    return;
  }

  self->loading = loading;
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LOADING]);
}

static double world_pixel_to_mercator_x(double px, guint zoom) {
  const double world = 256.0 * ldexp(1.0, (int)zoom);

  return px / world * 2.0 * WEB_MERCATOR_HALF_WORLD - WEB_MERCATOR_HALF_WORLD;
}

static double world_pixel_to_mercator_y(double py, guint zoom) {
  const double world = 256.0 * ldexp(1.0, (int)zoom);

  return WEB_MERCATOR_HALF_WORLD - py / world * 2.0 * WEB_MERCATOR_HALF_WORLD;
}

static void open_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
  OpenJob *job = task_data;
  OsmiumRaster *raster = NULL;

  osmium_error err = osmium_raster_open(job->path, job->user_agent, &raster);
  if (err == OSMIUM_CRS_ERROR) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "the raster has no usable georeferencing");
  } else if (err != OSMIUM_OK) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "could not read the raster");
  } else {
    g_task_return_pointer(task, raster, (GDestroyNotify)osmium_raster_unref);
  }
}

static void on_opened(GObject *source, GAsyncResult *result, gpointer user_data) {
  OsmiumRasterLayer *self = OSMIUM_RASTER_LAYER(source);
  OpenJob *job = g_task_get_task_data(G_TASK(result));
  g_autoptr(GError) error = NULL;
  OsmiumRaster *raster = g_task_propagate_pointer(G_TASK(result), &error);

  if (job->generation != self->generation) {
    osmium_raster_unref(raster);
    return;
  }

  set_loading(self, FALSE);

  if (raster == NULL) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("Raster %s: %s", job->path, error->message);
      g_signal_emit(self, signals[SIGNAL_LOAD_FAILED], 0, error->message);
    }
    return;
  }

  osmium_raster_unref(self->raster);
  self->raster = raster;

  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_HAS_EXTENT]);
  osmium_raster_layer_schedule_render(self);
}

static void start_open(OsmiumRasterLayer *self, gboolean keep_image) {
  self->generation++;

  if (!keep_image) {
    g_clear_pointer(&self->image, image_free);
    g_clear_pointer(&self->raster, osmium_raster_unref);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_HAS_EXTENT]);
    gtk_widget_queue_draw(GTK_WIDGET(self));
  }

  if (self->path == NULL || *self->path == '\0') {
    set_loading(self, FALSE);
    return;
  }

  set_loading(self, TRUE);

  OpenJob *job = g_new0(OpenJob, 1);
  job->path = g_strdup(self->path);
  job->user_agent = g_strdup(self->user_agent);
  job->generation = self->generation;

  GTask *task = g_task_new(self, self->cancellable, on_opened, NULL);
  g_task_set_task_data(task, job, open_job_free);
  g_task_run_in_thread(task, open_worker);
  g_object_unref(task);
}

static void render_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
  RenderJob *job = task_data;
  unsigned char *rgba = NULL;

  if (g_cancellable_is_cancelled(cancellable)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
    return;
  }

  osmium_error err = osmium_raster_render(
      job->raster, world_pixel_to_mercator_x(job->x, job->zoom),
      world_pixel_to_mercator_y(job->y + job->height, job->zoom),
      world_pixel_to_mercator_x(job->x + job->width, job->zoom),
      world_pixel_to_mercator_y(job->y, job->zoom), job->width, job->height, &rgba);
  if (err != OSMIUM_OK) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "could not render the raster");
    return;
  }

  g_autoptr(GBytes) bytes = g_bytes_new_with_free_func(rgba, (gsize)job->width * job->height * 4, free, rgba);
  GdkTexture *texture =
      gdk_memory_texture_new(job->width, job->height, GDK_MEMORY_R8G8B8A8, bytes, (gsize)job->width * 4);

  g_task_return_pointer(task, texture, g_object_unref);
}

static void on_rendered(GObject *source, GAsyncResult *result, gpointer user_data) {
  OsmiumRasterLayer *self = OSMIUM_RASTER_LAYER(source);
  RenderJob *job = g_task_get_task_data(G_TASK(result));
  g_autoptr(GError) error = NULL;
  GdkTexture *texture = g_task_propagate_pointer(G_TASK(result), &error);

  if (texture == NULL) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("Raster: %s", error->message);
    }
    return;
  }

  if (self->map == NULL || job->raster != self->raster ||
      (self->image != NULL && self->image->id > job->id)) {
    g_object_unref(texture);
    return;
  }

  image_free(self->image);
  self->image = g_new0(RasterImage, 1);
  self->image->texture = texture;
  self->image->id = job->id;
  self->image->zoom = job->zoom;
  self->image->x = job->x;
  self->image->y = job->y;
  self->image->width = job->width;
  self->image->height = job->height;

  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void render_now(OsmiumRasterLayer *self) {
  GtkWidget *widget = GTK_WIDGET(self);
  const int width = gtk_widget_get_width(widget);
  const int height = gtk_widget_get_height(widget);

  if (self->map == NULL || self->raster == NULL || width <= 0 || height <= 0) {
    return;
  }

  const guint zoom = osmium_map_get_zoom(self->map);
  const double rad = osmium_map_get_bearing(self->map) * G_PI / 180.0;
  const double view_width = width * fabs(cos(rad)) + height * fabs(sin(rad));
  const double view_height = width * fabs(sin(rad)) + height * fabs(cos(rad));

  double center_x, center_y;
  osmium_world_pixel(osmium_map_get_lat(self->map), osmium_map_get_lon(self->map), zoom, &center_x,
                     &center_y);

  RenderJob *job = g_new0(RenderJob, 1);
  job->raster = osmium_raster_ref(self->raster);
  job->id = ++self->last_request_id;
  job->zoom = zoom;
  job->width = CLAMP((int)ceil(view_width), 1, OSMIUM_RASTER_LAYER_MAX_SIZE);
  job->height = CLAMP((int)ceil(view_height), 1, OSMIUM_RASTER_LAYER_MAX_SIZE);
  job->x = floor(center_x - job->width / 2.0);
  job->y = floor(center_y - job->height / 2.0);

  double min_x, min_y, max_x, max_y;
  osmium_raster_get_extent(self->raster, &min_x, &min_y, &max_x, &max_y);

  const double view_min_x = world_pixel_to_mercator_x(job->x, zoom);
  const double view_max_x = world_pixel_to_mercator_x(job->x + job->width, zoom);
  const double view_min_y = world_pixel_to_mercator_y(job->y + job->height, zoom);
  const double view_max_y = world_pixel_to_mercator_y(job->y, zoom);

  if (max_x < view_min_x || min_x > view_max_x || max_y < view_min_y || min_y > view_max_y) {
    g_clear_pointer(&self->image, image_free);
    gtk_widget_queue_draw(widget);
    render_job_free(job);
    return;
  }

  GTask *task = g_task_new(self, self->cancellable, on_rendered, NULL);
  g_task_set_task_data(task, job, render_job_free);
  g_task_run_in_thread(task, render_worker);
  g_object_unref(task);
}

static gboolean on_reload_timeout(gpointer user_data) {
  OsmiumRasterLayer *self = OSMIUM_RASTER_LAYER(user_data);

  self->reload_source = 0;
  render_now(self);
  return G_SOURCE_REMOVE;
}

static void osmium_raster_layer_schedule_render(OsmiumRasterLayer *self) {
  if (self->reload_source != 0) {
    g_source_remove(self->reload_source);
  }
  self->reload_source = g_timeout_add(OSMIUM_RASTER_LAYER_RELOAD_DELAY_MS, on_reload_timeout, self);
}

static void on_map_view_changed(OsmiumMap *map, gpointer user_data) {
  OsmiumRasterLayer *self = OSMIUM_RASTER_LAYER(user_data);

  osmium_raster_layer_schedule_render(self);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void osmium_raster_layer_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  OsmiumRasterLayer *self = OSMIUM_RASTER_LAYER(widget);

  if (self->map == NULL || self->image == NULL) {
    return;
  }

  const float width = gtk_widget_get_width(widget);
  const float height = gtk_widget_get_height(widget);
  const double bearing = osmium_map_get_bearing(self->map);
  const guint zoom = osmium_map_get_zoom(self->map);

  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
  gtk_snapshot_push_clip(snapshot, &bounds);

  double center_x, center_y;
  osmium_world_pixel(osmium_map_get_lat(self->map), osmium_map_get_lon(self->map), zoom, &center_x,
                     &center_y);

  const double scale = ldexp(1.0, (int)zoom - (int)self->image->zoom);

  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(width / 2.0f, height / 2.0f));
  gtk_snapshot_rotate(snapshot, -(float)bearing);

  graphene_rect_t rect = GRAPHENE_RECT_INIT(self->image->x * scale - center_x,
                                            self->image->y * scale - center_y,
                                            self->image->width * scale,
                                            self->image->height * scale);
  gtk_snapshot_append_texture(snapshot, self->image->texture, &rect);

  gtk_snapshot_restore(snapshot);
  gtk_snapshot_pop(snapshot);
}

static void osmium_raster_layer_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
  osmium_raster_layer_schedule_render(OSMIUM_RASTER_LAYER(widget));
}

OsmiumMap *osmium_raster_layer_get_map(OsmiumRasterLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_RASTER_LAYER(self), NULL);

  return self->map;
}

void osmium_raster_layer_set_map(OsmiumRasterLayer *self, OsmiumMap *map) {
  g_return_if_fail(OSMIUM_IS_RASTER_LAYER(self));
  g_return_if_fail(map == NULL || OSMIUM_IS_MAP(map));

  if (self->map == map) {
    return;
  }

  if (self->map != NULL) {
    g_signal_handler_disconnect(self->map, self->view_handler);
    self->view_handler = 0;
    g_clear_weak_pointer(&self->map);
  }

  if (map != NULL) {
    g_set_weak_pointer(&self->map, map);
    self->view_handler = g_signal_connect(map, "view-changed", G_CALLBACK(on_map_view_changed), self);
  }

  g_clear_pointer(&self->image, image_free);
  osmium_raster_layer_schedule_render(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MAP]);
}

const char *osmium_raster_layer_get_path(OsmiumRasterLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_RASTER_LAYER(self), NULL);

  return self->path;
}

void osmium_raster_layer_set_path(OsmiumRasterLayer *self, const char *path) {
  g_return_if_fail(OSMIUM_IS_RASTER_LAYER(self));

  if (g_strcmp0(self->path, path) == 0) {
    return;
  }

  g_free(self->path);
  self->path = g_strdup(path);

  start_open(self, FALSE);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PATH]);
}

const char *osmium_raster_layer_get_user_agent(OsmiumRasterLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_RASTER_LAYER(self), NULL);

  return self->user_agent;
}

void osmium_raster_layer_set_user_agent(OsmiumRasterLayer *self, const char *user_agent) {
  g_return_if_fail(OSMIUM_IS_RASTER_LAYER(self));

  if (g_strcmp0(self->user_agent, user_agent) == 0) {
    return;
  }

  g_free(self->user_agent);
  self->user_agent = g_strdup(user_agent);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_USER_AGENT]);
}

void osmium_raster_layer_reload(OsmiumRasterLayer *self) {
  g_return_if_fail(OSMIUM_IS_RASTER_LAYER(self));

  start_open(self, TRUE);
}

gboolean osmium_raster_layer_get_loading(OsmiumRasterLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_RASTER_LAYER(self), FALSE);

  return self->loading;
}

gboolean osmium_raster_layer_get_extent(OsmiumRasterLayer *self,
                                        double *lat_min,
                                        double *lon_min,
                                        double *lat_max,
                                        double *lon_max) {
  g_return_val_if_fail(OSMIUM_IS_RASTER_LAYER(self), FALSE);

  if (self->raster == NULL) {
    return FALSE;
  }

  double min_x, min_y, max_x, max_y;
  osmium_raster_get_extent(self->raster, &min_x, &min_y, &max_x, &max_y);

  *lon_min = min_x / WEB_MERCATOR_HALF_WORLD * 180.0;
  *lon_max = max_x / WEB_MERCATOR_HALF_WORLD * 180.0;
  *lat_min = atan(sinh(min_y / WEB_MERCATOR_RADIUS)) * 180.0 / G_PI;
  *lat_max = atan(sinh(max_y / WEB_MERCATOR_RADIUS)) * 180.0 / G_PI;
  return TRUE;
}

static void osmium_raster_layer_dispose(GObject *object) {
  OsmiumRasterLayer *self = OSMIUM_RASTER_LAYER(object);

  if (self->cancellable != NULL) {
    g_cancellable_cancel(self->cancellable);
  }

  if (self->reload_source != 0) {
    g_source_remove(self->reload_source);
    self->reload_source = 0;
  }

  if (self->map != NULL) {
    g_signal_handler_disconnect(self->map, self->view_handler);
    self->view_handler = 0;
    g_clear_weak_pointer(&self->map);
  }

  g_clear_object(&self->cancellable);
  g_clear_pointer(&self->image, image_free);
  g_clear_pointer(&self->raster, osmium_raster_unref);
  g_clear_pointer(&self->path, g_free);
  g_clear_pointer(&self->user_agent, g_free);

  G_OBJECT_CLASS(osmium_raster_layer_parent_class)->dispose(object);
}

static void osmium_raster_layer_set_property(GObject *object,
                                             guint prop_id,
                                             const GValue *value,
                                             GParamSpec *pspec) {
  OsmiumRasterLayer *self = OSMIUM_RASTER_LAYER(object);

  switch (prop_id) {
  case PROP_MAP:
    osmium_raster_layer_set_map(self, g_value_get_object(value));
    break;
  case PROP_PATH:
    osmium_raster_layer_set_path(self, g_value_get_string(value));
    break;
  case PROP_USER_AGENT:
    osmium_raster_layer_set_user_agent(self, g_value_get_string(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void osmium_raster_layer_get_property(GObject *object,
                                             guint prop_id,
                                             GValue *value,
                                             GParamSpec *pspec) {
  OsmiumRasterLayer *self = OSMIUM_RASTER_LAYER(object);

  switch (prop_id) {
  case PROP_MAP:
    g_value_set_object(value, self->map);
    break;
  case PROP_PATH:
    g_value_set_string(value, self->path);
    break;
  case PROP_USER_AGENT:
    g_value_set_string(value, self->user_agent);
    break;
  case PROP_LOADING:
    g_value_set_boolean(value, self->loading);
    break;
  case PROP_HAS_EXTENT:
    g_value_set_boolean(value, self->raster != NULL);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void osmium_raster_layer_class_init(OsmiumRasterLayerClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  const GParamFlags flags = G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;
  const GParamFlags read_only = G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;

  object_class->dispose = osmium_raster_layer_dispose;
  object_class->set_property = osmium_raster_layer_set_property;
  object_class->get_property = osmium_raster_layer_get_property;

  properties[PROP_MAP] =
      g_param_spec_object("map", NULL, "The map this layer draws on", OSMIUM_TYPE_MAP, flags);
  properties[PROP_PATH] = g_param_spec_string(
      "path", NULL, "File path or http(s) URL of a georeferenced raster (GeoTIFF, COG, ...)", NULL,
      flags);
  properties[PROP_USER_AGENT] = g_param_spec_string(
      "user-agent", NULL, "User-Agent sent when the raster is read from a URL", "Osmium/0.1", flags);
  properties[PROP_LOADING] =
      g_param_spec_boolean("loading", NULL, "Whether the raster is being opened", FALSE, read_only);
  properties[PROP_HAS_EXTENT] =
      g_param_spec_boolean("has-extent", NULL, "Whether the raster extent is known", FALSE, read_only);

  g_object_class_install_properties(object_class, N_PROPS, properties);

  signals[SIGNAL_LOAD_FAILED] = g_signal_new("load-failed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                             0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

  widget_class->snapshot = osmium_raster_layer_snapshot;
  widget_class->size_allocate = osmium_raster_layer_size_allocate;
  gtk_widget_class_set_css_name(widget_class, "osmium-raster-layer");
}

static void osmium_raster_layer_init(OsmiumRasterLayer *self) {
  GtkWidget *widget = GTK_WIDGET(self);

  self->user_agent = g_strdup("Osmium/0.1");
  self->cancellable = g_cancellable_new();

  gtk_widget_set_can_target(widget, FALSE);
  gtk_widget_set_can_focus(widget, FALSE);
  gtk_widget_set_overflow(widget, GTK_OVERFLOW_HIDDEN);
}

GtkWidget *osmium_raster_layer_new(OsmiumMap *map) {
  return g_object_new(OSMIUM_TYPE_RASTER_LAYER, "map", map, NULL);
}
