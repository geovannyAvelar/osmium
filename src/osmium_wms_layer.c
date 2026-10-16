/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_wms_layer.h"

#include "osmium_map_private.h"
#include "raster/raster.h"
#include "tms/tms.h"
#include "wms/wms.h"
#include <string.h>
#include <strings.h>
#include <math.h>
#include <gio/gio.h>

#define OSMIUM_WMS_LAYER_DEFAULT_USER_AGENT "Osmium/0.1"
#define OSMIUM_WMS_LAYER_DEFAULT_FORMAT "image/png"
#define OSMIUM_WMS_LAYER_DEFAULT_VERSION "1.3.0"
#define OSMIUM_WMS_LAYER_RELOAD_DELAY_MS 200
#define OSMIUM_WMS_LAYER_MAX_SIZE 4096

typedef struct {
  OsmiumProjection *projection;
  char *request_crs;
  guint id;
  guint zoom;
  double x;
  double y;
  int width;
  int height;
  char *url;
  char *version;
  char *layers;
  char *styles;
  char *format;
  char *user_agent;
  gboolean transparent;
} WmsJob;

typedef struct {
  GdkTexture *texture;
  guint id;
  guint zoom;
  double x;
  double y;
  int width;
  int height;
} WmsImage;

struct _OsmiumWmsLayer {
  GtkWidget parent_instance;

  OsmiumMap *map; /* weak */
  gulong view_handler;
  gulong crs_handler;
  char **supported_crs;

  char *url;
  char *layers;
  char *styles;
  char *format;
  char *version;
  char *user_agent;
  gboolean transparent;

  guint reload_source;
  guint last_request_id;
  WmsImage *image;
  GCancellable *cancellable;
};

OSMIUM_API GType osmium_wms_layer_get_type(void);

G_DEFINE_TYPE(OsmiumWmsLayer, osmium_wms_layer, GTK_TYPE_WIDGET)

enum {
  PROP_0,
  PROP_MAP,
  PROP_URL,
  PROP_LAYERS,
  PROP_STYLES,
  PROP_FORMAT,
  PROP_VERSION,
  PROP_USER_AGENT,
  PROP_TRANSPARENT,
  PROP_SUPPORTED_CRS,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void wms_job_free(gpointer data) {
  WmsJob *job = data;

  g_free(job->url);
  g_free(job->version);
  g_free(job->layers);
  g_free(job->styles);
  g_free(job->format);
  g_free(job->user_agent);
  g_free(job->request_crs);
  g_clear_pointer(&job->projection, osmium_projection_unref);
  g_free(job);
}

static void wms_image_free(gpointer data) {
  WmsImage *image = data;

  if (image == NULL) {
    return;
  }
  g_clear_object(&image->texture);
  g_free(image);
}

static gboolean crs_equal(const char *a, const char *b) {
  return a != NULL && b != NULL && g_ascii_strcasecmp(a, b) == 0;
}

static gboolean list_has(char **list, const char *crs) {
  for (guint i = 0; list != NULL && list[i] != NULL; i++) {
    if (crs_equal(list[i], crs)) {
      return TRUE;
    }
  }
  return FALSE;
}

/* the map's own CRS when the server offers it, else a common one that gets reprojected */
static char *choose_request_crs(OsmiumWmsLayer *self, OsmiumProjection *projection) {
  static char *default_list[] = {"EPSG:3857", NULL};
  char **offered = self->supported_crs != NULL ? self->supported_crs : default_list;
  const char *code = osmium_projection_get_code(projection);

  if (code != NULL && list_has(offered, code)) {
    return g_strdup(code);
  }
  if (list_has(offered, "EPSG:3857")) {
    return g_strdup("EPSG:3857");
  }
  if (list_has(offered, "EPSG:4326")) {
    return g_strdup("EPSG:4326");
  }
  return g_strdup(offered[0] != NULL ? offered[0] : "EPSG:3857");
}

static void bounds_in_crs(const char *map_crs,
                          const char *request_crs,
                          const double map_box[4],
                          double out[4]) {
  OsmiumTransform *transform = osmium_transform_new(map_crs, request_crs, NULL, NULL, 0);
  double points[2 * 20];
  size_t n = 0;

  memcpy(out, map_box, 4 * sizeof(double));
  if (transform == NULL) {
    return;
  }

  for (int i = 0; i <= 5; i++) {
    const double fx = map_box[0] + (map_box[2] - map_box[0]) * i / 5.0;
    const double fy = map_box[1] + (map_box[3] - map_box[1]) * i / 5.0;
    const double edges[4][2] = {{fx, map_box[1]}, {fx, map_box[3]}, {map_box[0], fy}, {map_box[2], fy}};

    for (int e = 0; e < 4 && n < 20; e++) {
      points[n * 2] = edges[e][0];
      points[n * 2 + 1] = edges[e][1];
      n++;
    }
  }

  osmium_transform_apply(transform, points, n);
  osmium_transform_free(transform);

  gboolean found = FALSE;

  for (size_t i = 0; i < n; i++) {
    if (!isfinite(points[i * 2]) || !isfinite(points[i * 2 + 1])) {
      continue;
    }
    if (!found) {
      out[0] = out[2] = points[i * 2];
      out[1] = out[3] = points[i * 2 + 1];
      found = TRUE;
    } else {
      out[0] = MIN(out[0], points[i * 2]);
      out[1] = MIN(out[1], points[i * 2 + 1]);
      out[2] = MAX(out[2], points[i * 2]);
      out[3] = MAX(out[3], points[i * 2 + 1]);
    }
  }
}

static void wms_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
  WmsJob *job = task_data;
  OsmiumWmsImage image;
  const char *map_crs = osmium_projection_get_definition(job->projection);
  const double world = 256.0 * ldexp(1.0, (int)job->zoom);
  double left, top, right, bottom;

  osmium_projection_normalized_to_units(job->projection, job->x / world, job->y / world, &left, &top);
  osmium_projection_normalized_to_units(job->projection, (job->x + job->width) / world,
                                        (job->y + job->height) / world, &right, &bottom);

  const double map_box[4] = {MIN(left, right), MIN(top, bottom), MAX(left, right), MAX(top, bottom)};
  const gboolean same = crs_equal(osmium_projection_get_code(job->projection), job->request_crs);
  double request_box[4];

  if (same) {
    memcpy(request_box, map_box, sizeof(request_box));
  } else {
    bounds_in_crs(map_crs, job->request_crs, map_box, request_box);
  }

  OsmiumWmsGetMapParams params = {
      .url = job->url,
      .version = job->version,
      .layers = job->layers,
      .styles = job->styles,
      .crs = job->request_crs,
      .bbox = {request_box[0], request_box[1], request_box[2], request_box[3]},
      .width = job->width,
      .height = job->height,
      .format = job->format,
      .transparent = job->transparent,
  };

  if (g_cancellable_is_cancelled(cancellable)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
    return;
  }

  osmium_error err = osmium_wms_get_map(&params, job->user_agent, &image);
  if (err != OSMIUM_OK) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "GetMap failed (error %d)", err);
    return;
  }

  if (!same) {
    unsigned char *rgba = NULL;

    err = osmium_image_reproject(image.data, image.len, job->request_crs, request_box, map_crs, map_box,
                                 job->width, job->height, &rgba);
    osmium_wms_image_free(&image);
    if (err != OSMIUM_OK) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "could not reproject the map image");
      return;
    }

    g_autoptr(GBytes) pixels = g_bytes_new_with_free_func(rgba, (gsize)job->width * job->height * 4, free, rgba);

    g_task_return_pointer(task,
                          gdk_memory_texture_new(job->width, job->height, GDK_MEMORY_R8G8B8A8, pixels,
                                                 (gsize)job->width * 4),
                          g_object_unref);
    return;
  }

  GBytes *bytes = g_bytes_new_with_free_func(image.data, image.len, free, image.data);
  image.data = NULL;
  osmium_wms_image_free(&image);

  GError *error = NULL;
  GdkTexture *texture = gdk_texture_new_from_bytes(bytes, &error);
  g_bytes_unref(bytes);

  if (texture == NULL) {
    g_task_return_error(task, error);
    return;
  }

  g_task_return_pointer(task, texture, g_object_unref);
}

static void on_image_ready(GObject *source, GAsyncResult *result, gpointer user_data) {
  OsmiumWmsLayer *self = OSMIUM_WMS_LAYER(source);
  WmsJob *job = g_task_get_task_data(G_TASK(result));
  g_autoptr(GError) error = NULL;

  GdkTexture *texture = g_task_propagate_pointer(G_TASK(result), &error);

  if (texture == NULL) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("WMS: %s", error->message);
    }
    return;
  }

  if (self->map == NULL || job->projection != osmium_map_get_projection(self->map) ||
      (self->image != NULL && self->image->id > job->id)) {
    g_object_unref(texture);
    return;
  }

  wms_image_free(self->image);

  self->image = g_new0(WmsImage, 1);
  self->image->texture = texture;
  self->image->id = job->id;
  self->image->zoom = job->zoom;
  self->image->x = job->x;
  self->image->y = job->y;
  self->image->width = job->width;
  self->image->height = job->height;

  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void osmium_wms_layer_request(OsmiumWmsLayer *self) {
  GtkWidget *widget = GTK_WIDGET(self);
  const int width = gtk_widget_get_width(widget);
  const int height = gtk_widget_get_height(widget);

  if (self->map == NULL || self->url == NULL || self->layers == NULL || width <= 0 || height <= 0) {
    return;
  }

  const guint zoom = osmium_map_get_zoom(self->map);
  const double rad = osmium_map_get_bearing(self->map) * G_PI / 180.0;
  const double view_width = width * fabs(cos(rad)) + height * fabs(sin(rad));
  const double view_height = width * fabs(sin(rad)) + height * fabs(cos(rad));

  double center_x, center_y;
  osmium_map_lonlat_to_world(self->map, osmium_map_get_lat(self->map), osmium_map_get_lon(self->map), zoom,
                             &center_x, &center_y);

  WmsJob *job = g_new0(WmsJob, 1);
  job->projection = osmium_projection_ref(osmium_map_get_projection(self->map));
  job->request_crs = choose_request_crs(self, job->projection);
  job->id = ++self->last_request_id;
  job->zoom = zoom;
  job->width = CLAMP((int)ceil(view_width), 1, OSMIUM_WMS_LAYER_MAX_SIZE);
  job->height = CLAMP((int)ceil(view_height), 1, OSMIUM_WMS_LAYER_MAX_SIZE);
  job->x = floor(center_x - job->width / 2.0);
  job->y = floor(center_y - job->height / 2.0);
  job->url = g_strdup(self->url);
  job->version = g_strdup(self->version);
  job->layers = g_strdup(self->layers);
  job->styles = g_strdup(self->styles);
  job->format = g_strdup(self->format);
  job->user_agent = g_strdup(self->user_agent);
  job->transparent = self->transparent;

  GTask *task = g_task_new(self, self->cancellable, on_image_ready, NULL);
  g_task_set_task_data(task, job, wms_job_free);
  g_task_run_in_thread(task, wms_worker);
  g_object_unref(task);
}

static gboolean on_reload_timeout(gpointer user_data) {
  OsmiumWmsLayer *self = OSMIUM_WMS_LAYER(user_data);

  self->reload_source = 0;
  osmium_wms_layer_request(self);
  return G_SOURCE_REMOVE;
}

static void osmium_wms_layer_schedule_reload(OsmiumWmsLayer *self) {
  if (self->reload_source != 0) {
    g_source_remove(self->reload_source);
  }
  self->reload_source =
      g_timeout_add(OSMIUM_WMS_LAYER_RELOAD_DELAY_MS, on_reload_timeout, self);
}

static void osmium_wms_layer_reset(OsmiumWmsLayer *self) {
  g_clear_pointer(&self->image, wms_image_free);
  osmium_wms_layer_schedule_reload(self);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void on_map_crs_changed(GObject *map, GParamSpec *pspec, gpointer user_data) {
  OsmiumWmsLayer *self = OSMIUM_WMS_LAYER(user_data);

  g_clear_pointer(&self->image, wms_image_free);
  osmium_wms_layer_schedule_reload(self);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void on_map_view_changed(OsmiumMap *map, gpointer user_data) {
  OsmiumWmsLayer *self = OSMIUM_WMS_LAYER(user_data);

  osmium_wms_layer_schedule_reload(self);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void osmium_wms_layer_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  OsmiumWmsLayer *self = OSMIUM_WMS_LAYER(widget);

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
  osmium_map_lonlat_to_world(self->map, osmium_map_get_lat(self->map), osmium_map_get_lon(self->map), zoom,
                             &center_x, &center_y);

  const double scale = pow(2.0, (double)zoom - (double)self->image->zoom);

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

static void osmium_wms_layer_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
  osmium_wms_layer_schedule_reload(OSMIUM_WMS_LAYER(widget));
}

OsmiumMap *osmium_wms_layer_get_map(OsmiumWmsLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_WMS_LAYER(self), NULL);

  return self->map;
}

void osmium_wms_layer_set_map(OsmiumWmsLayer *self, OsmiumMap *map) {
  g_return_if_fail(OSMIUM_IS_WMS_LAYER(self));
  g_return_if_fail(map == NULL || OSMIUM_IS_MAP(map));

  if (self->map == map) {
    return;
  }

  if (self->map != NULL) {
    g_signal_handler_disconnect(self->map, self->view_handler);
    g_signal_handler_disconnect(self->map, self->crs_handler);
    self->view_handler = 0;
    self->crs_handler = 0;
    g_clear_weak_pointer(&self->map);
  }

  if (map != NULL) {
    g_set_weak_pointer(&self->map, map);
    self->view_handler = g_signal_connect(map, "view-changed", G_CALLBACK(on_map_view_changed), self);
    self->crs_handler = g_signal_connect(map, "notify::crs", G_CALLBACK(on_map_crs_changed), self);
  }

  osmium_wms_layer_reset(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MAP]);
}

#define DEFINE_STRING_ACCESSORS(field, PROP, reload)                                           \
  const char *osmium_wms_layer_get_##field(OsmiumWmsLayer *self) {                             \
    g_return_val_if_fail(OSMIUM_IS_WMS_LAYER(self), NULL);                                     \
    return self->field;                                                                        \
  }                                                                                            \
  void osmium_wms_layer_set_##field(OsmiumWmsLayer *self, const char *value) {                 \
    g_return_if_fail(OSMIUM_IS_WMS_LAYER(self));                                               \
    if (g_strcmp0(self->field, value) == 0) {                                                  \
      return;                                                                                  \
    }                                                                                          \
    g_free(self->field);                                                                       \
    self->field = g_strdup(value);                                                             \
    if (reload) {                                                                              \
      osmium_wms_layer_reset(self);                                                            \
    }                                                                                          \
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP]);                                \
  }

DEFINE_STRING_ACCESSORS(url, PROP_URL, TRUE)
DEFINE_STRING_ACCESSORS(layers, PROP_LAYERS, TRUE)
DEFINE_STRING_ACCESSORS(styles, PROP_STYLES, TRUE)
DEFINE_STRING_ACCESSORS(format, PROP_FORMAT, TRUE)
DEFINE_STRING_ACCESSORS(version, PROP_VERSION, TRUE)
DEFINE_STRING_ACCESSORS(user_agent, PROP_USER_AGENT, FALSE)

const char *const *osmium_wms_layer_get_supported_crs(OsmiumWmsLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_WMS_LAYER(self), NULL);

  return (const char *const *)self->supported_crs;
}

void osmium_wms_layer_set_supported_crs(OsmiumWmsLayer *self, const char *const *crs) {
  g_return_if_fail(OSMIUM_IS_WMS_LAYER(self));

  g_strfreev(self->supported_crs);
  self->supported_crs = crs != NULL && crs[0] != NULL ? g_strdupv((char **)crs) : NULL;
  osmium_wms_layer_reset(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SUPPORTED_CRS]);
}

gboolean osmium_wms_layer_get_transparent(OsmiumWmsLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_WMS_LAYER(self), FALSE);

  return self->transparent;
}

void osmium_wms_layer_set_transparent(OsmiumWmsLayer *self, gboolean transparent) {
  g_return_if_fail(OSMIUM_IS_WMS_LAYER(self));

  if (self->transparent == transparent) {
    return;
  }

  self->transparent = transparent;
  osmium_wms_layer_reset(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TRANSPARENT]);
}

static void osmium_wms_layer_dispose(GObject *object) {
  OsmiumWmsLayer *self = OSMIUM_WMS_LAYER(object);

  if (self->cancellable != NULL) {
    g_cancellable_cancel(self->cancellable);
  }

  if (self->reload_source != 0) {
    g_source_remove(self->reload_source);
    self->reload_source = 0;
  }

  if (self->map != NULL) {
    g_signal_handler_disconnect(self->map, self->view_handler);
    g_signal_handler_disconnect(self->map, self->crs_handler);
    self->view_handler = 0;
    self->crs_handler = 0;
    g_clear_weak_pointer(&self->map);
  }

  g_clear_object(&self->cancellable);
  g_clear_pointer(&self->image, wms_image_free);
  g_clear_pointer(&self->url, g_free);
  g_clear_pointer(&self->layers, g_free);
  g_clear_pointer(&self->styles, g_free);
  g_clear_pointer(&self->format, g_free);
  g_clear_pointer(&self->version, g_free);
  g_clear_pointer(&self->user_agent, g_free);
  g_clear_pointer(&self->supported_crs, g_strfreev);

  G_OBJECT_CLASS(osmium_wms_layer_parent_class)->dispose(object);
}

static void osmium_wms_layer_set_property(GObject *object,
                                          guint prop_id,
                                          const GValue *value,
                                          GParamSpec *pspec) {
  OsmiumWmsLayer *self = OSMIUM_WMS_LAYER(object);

  switch (prop_id) {
  case PROP_MAP:
    osmium_wms_layer_set_map(self, g_value_get_object(value));
    break;
  case PROP_URL:
    osmium_wms_layer_set_url(self, g_value_get_string(value));
    break;
  case PROP_LAYERS:
    osmium_wms_layer_set_layers(self, g_value_get_string(value));
    break;
  case PROP_STYLES:
    osmium_wms_layer_set_styles(self, g_value_get_string(value));
    break;
  case PROP_FORMAT:
    osmium_wms_layer_set_format(self, g_value_get_string(value));
    break;
  case PROP_VERSION:
    osmium_wms_layer_set_version(self, g_value_get_string(value));
    break;
  case PROP_USER_AGENT:
    osmium_wms_layer_set_user_agent(self, g_value_get_string(value));
    break;
  case PROP_TRANSPARENT:
    osmium_wms_layer_set_transparent(self, g_value_get_boolean(value));
    break;
  case PROP_SUPPORTED_CRS:
    osmium_wms_layer_set_supported_crs(self, g_value_get_boxed(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void osmium_wms_layer_get_property(GObject *object,
                                          guint prop_id,
                                          GValue *value,
                                          GParamSpec *pspec) {
  OsmiumWmsLayer *self = OSMIUM_WMS_LAYER(object);

  switch (prop_id) {
  case PROP_MAP:
    g_value_set_object(value, self->map);
    break;
  case PROP_URL:
    g_value_set_string(value, self->url);
    break;
  case PROP_LAYERS:
    g_value_set_string(value, self->layers);
    break;
  case PROP_STYLES:
    g_value_set_string(value, self->styles);
    break;
  case PROP_FORMAT:
    g_value_set_string(value, self->format);
    break;
  case PROP_VERSION:
    g_value_set_string(value, self->version);
    break;
  case PROP_USER_AGENT:
    g_value_set_string(value, self->user_agent);
    break;
  case PROP_TRANSPARENT:
    g_value_set_boolean(value, self->transparent);
    break;
  case PROP_SUPPORTED_CRS:
    g_value_set_boxed(value, self->supported_crs);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void osmium_wms_layer_class_init(OsmiumWmsLayerClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  const GParamFlags flags = G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;

  curl_global_init(CURL_GLOBAL_DEFAULT);

  object_class->dispose = osmium_wms_layer_dispose;
  object_class->set_property = osmium_wms_layer_set_property;
  object_class->get_property = osmium_wms_layer_get_property;

  properties[PROP_MAP] =
      g_param_spec_object("map", NULL, "The map this layer draws on", OSMIUM_TYPE_MAP, flags);
  properties[PROP_URL] =
      g_param_spec_string("url", NULL, "WMS GetMap endpoint", NULL, flags);
  properties[PROP_LAYERS] =
      g_param_spec_string("layers", NULL, "Comma separated WMS layer names", NULL, flags);
  properties[PROP_STYLES] =
      g_param_spec_string("styles", NULL, "Comma separated WMS styles", NULL, flags);
  properties[PROP_FORMAT] = g_param_spec_string(
      "format", NULL, "Image format requested", OSMIUM_WMS_LAYER_DEFAULT_FORMAT, flags);
  properties[PROP_VERSION] = g_param_spec_string(
      "version", NULL, "WMS version, 1.3.0 or 1.1.1", OSMIUM_WMS_LAYER_DEFAULT_VERSION, flags);
  properties[PROP_USER_AGENT] = g_param_spec_string(
      "user-agent", NULL, "User-Agent sent to the server", OSMIUM_WMS_LAYER_DEFAULT_USER_AGENT,
      flags);
  properties[PROP_SUPPORTED_CRS] = g_param_spec_boxed(
      "supported-crs", NULL, "Coordinate systems the server offers (EPSG:3857 when unset)", G_TYPE_STRV, flags);
  properties[PROP_TRANSPARENT] =
      g_param_spec_boolean("transparent", NULL, "Request a transparent background", TRUE, flags);

  g_object_class_install_properties(object_class, N_PROPS, properties);

  widget_class->snapshot = osmium_wms_layer_snapshot;
  widget_class->size_allocate = osmium_wms_layer_size_allocate;
  gtk_widget_class_set_css_name(widget_class, "osmium-wms-layer");
}

static void osmium_wms_layer_init(OsmiumWmsLayer *self) {
  GtkWidget *widget = GTK_WIDGET(self);

  self->format = g_strdup(OSMIUM_WMS_LAYER_DEFAULT_FORMAT);
  self->version = g_strdup(OSMIUM_WMS_LAYER_DEFAULT_VERSION);
  self->user_agent = g_strdup(OSMIUM_WMS_LAYER_DEFAULT_USER_AGENT);
  self->transparent = TRUE;
  self->cancellable = g_cancellable_new();

  gtk_widget_set_can_target(widget, FALSE);
  gtk_widget_set_can_focus(widget, FALSE);
  gtk_widget_set_overflow(widget, GTK_OVERFLOW_HIDDEN);
}

GtkWidget *osmium_wms_layer_new(OsmiumMap *map) {
  return g_object_new(OSMIUM_TYPE_WMS_LAYER, "map", map, NULL);
}
