/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_vector_layer.h"

#include "vector/vector_data.h"
#include "tms/tms.h"
#include <math.h>
#include <gio/gio.h>

#define OSMIUM_VECTOR_LAYER_DEFAULT_LINE_WIDTH 1.5
#define OSMIUM_VECTOR_LAYER_DEFAULT_POINT_RADIUS 4.0
#define OSMIUM_VECTOR_LAYER_MIN_FEATURE_SIZE 0.5
#define OSMIUM_VECTOR_LAYER_MIN_VERTEX_STEP 0.5

typedef struct {
  char *path;
  char *user_agent;
  guint generation;
} LoadJob;

struct _OsmiumVectorLayer {
  GtkWidget parent_instance;

  OsmiumMap *map; /* weak */
  gulong view_handler;

  char *path;
  char *user_agent;
  GdkRGBA stroke_color;
  GdkRGBA fill_color;
  double line_width;
  double point_radius;

  guint generation;
  gboolean loading;
  OsmiumVectorData *data;
  GCancellable *cancellable;
};

OSMIUM_API GType osmium_vector_layer_get_type(void);

G_DEFINE_TYPE(OsmiumVectorLayer, osmium_vector_layer, GTK_TYPE_WIDGET)

enum {
  PROP_0,
  PROP_MAP,
  PROP_PATH,
  PROP_USER_AGENT,
  PROP_STROKE_COLOR,
  PROP_FILL_COLOR,
  PROP_LINE_WIDTH,
  PROP_POINT_RADIUS,
  PROP_N_FEATURES,
  PROP_LOADING,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

enum { SIGNAL_LOAD_FAILED, N_SIGNALS };

static guint signals[N_SIGNALS];

static void set_loading(OsmiumVectorLayer *self, gboolean loading) {
  if (self->loading == loading) {
    return;
  }

  self->loading = loading;
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LOADING]);
}

static void load_job_free(gpointer data) {
  LoadJob *job = data;

  g_free(job->path);
  g_free(job->user_agent);
  g_free(job);
}

static void load_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
  LoadJob *job = task_data;
  OsmiumVectorData *loaded = NULL;

  osmium_error err = osmium_vector_data_open(job->path, job->user_agent, &loaded);
  if (err == OSMIUM_CRS_ERROR) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "unsupported coordinate system");
    return;
  }
  if (err == OSMIUM_NETWORK_ERROR) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "could not download the file");
    return;
  }
  if (err != OSMIUM_OK) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "could not read the file");
    return;
  }

  g_task_return_pointer(task, loaded, (GDestroyNotify)osmium_vector_data_free);
}

static void on_loaded(GObject *source, GAsyncResult *result, gpointer user_data) {
  OsmiumVectorLayer *self = OSMIUM_VECTOR_LAYER(source);
  LoadJob *job = g_task_get_task_data(G_TASK(result));
  g_autoptr(GError) error = NULL;

  OsmiumVectorData *loaded = g_task_propagate_pointer(G_TASK(result), &error);

  if (job->generation == self->generation) {
    set_loading(self, FALSE);
  }

  if (loaded == NULL) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
        job->generation == self->generation) {
      g_warning("Vector file %s: %s", job->path, error->message);
      g_signal_emit(self, signals[SIGNAL_LOAD_FAILED], 0, error->message);
    }
    return;
  }

  if (job->generation != self->generation) {
    osmium_vector_data_free(loaded);
    return;
  }

  osmium_vector_data_free(self->data);
  self->data = loaded;

  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_N_FEATURES]);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void start_load(OsmiumVectorLayer *self, gboolean keep_data) {
  self->generation++;

  if (!keep_data) {
    g_clear_pointer(&self->data, osmium_vector_data_free);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_N_FEATURES]);
    gtk_widget_queue_draw(GTK_WIDGET(self));
  }

  if (self->path == NULL || *self->path == '\0') {
    set_loading(self, FALSE);
    return;
  }

  set_loading(self, TRUE);

  LoadJob *job = g_new0(LoadJob, 1);
  job->path = g_strdup(self->path);
  job->user_agent = g_strdup(self->user_agent);
  job->generation = self->generation;

  GTask *task = g_task_new(self, self->cancellable, on_loaded, NULL);
  g_task_set_task_data(task, job, load_job_free);
  g_task_run_in_thread(task, load_worker);
  g_object_unref(task);
}

static void on_map_view_changed(OsmiumMap *map, gpointer user_data) {
  gtk_widget_queue_draw(GTK_WIDGET(user_data));
}

static void append_feature(const OsmiumVectorFeature *f,
                           GskPathBuilder *builder,
                           double scale,
                           double cx,
                           double cy,
                           double point_radius) {
  for (size_t p = 0; p < f->n_parts; p++) {
    const size_t start = f->part_start[p];
    const size_t end = f->part_start[p + 1];
    size_t last = end;

    if (f->kind == OSMIUM_VECTOR_POINT) {
      gsk_path_builder_add_circle(
          builder,
          &GRAPHENE_POINT_INIT((float)(f->xy[start * 2] * scale - cx),
                               (float)(f->xy[start * 2 + 1] * scale - cy)),
          (float)point_radius);
      continue;
    }

    if (f->kind == OSMIUM_VECTOR_POLYGON && end - start > 1 &&
        f->xy[start * 2] == f->xy[(end - 1) * 2] &&
        f->xy[start * 2 + 1] == f->xy[(end - 1) * 2 + 1]) {
      last = end - 1;
    }

    double kept_x = 0.0;
    double kept_y = 0.0;

    for (size_t i = start; i < last; i++) {
      const double x = f->xy[i * 2] * scale - cx;
      const double y = f->xy[i * 2 + 1] * scale - cy;

      if (i == start) {
        gsk_path_builder_move_to(builder, (float)x, (float)y);
      } else {
        const double dx = x - kept_x;
        const double dy = y - kept_y;
        const gboolean is_last = i == last - 1;

        if (!is_last && dx * dx + dy * dy < OSMIUM_VECTOR_LAYER_MIN_VERTEX_STEP *
                                                OSMIUM_VECTOR_LAYER_MIN_VERTEX_STEP) {
          continue;
        }
        gsk_path_builder_line_to(builder, (float)x, (float)y);
      }

      kept_x = x;
      kept_y = y;
    }

    if (f->kind == OSMIUM_VECTOR_POLYGON) {
      gsk_path_builder_close(builder);
    }
  }
}

static void osmium_vector_layer_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  OsmiumVectorLayer *self = OSMIUM_VECTOR_LAYER(widget);

  if (self->map == NULL || self->data == NULL || self->data->n_features == 0) {
    return;
  }

  const float width = gtk_widget_get_width(widget);
  const float height = gtk_widget_get_height(widget);
  const guint zoom = osmium_map_get_zoom(self->map);
  const double bearing = osmium_map_get_bearing(self->map);

  double cx, cy;
  osmium_world_pixel(osmium_map_get_lat(self->map), osmium_map_get_lon(self->map), zoom, &cx, &cy);

  const double scale = 256.0 * ldexp(1.0, (int)zoom);
  const double reach = hypot(width, height) / 2.0 + self->line_width + self->point_radius;

  GskPathBuilder *polygons = gsk_path_builder_new();
  GskPathBuilder *lines = gsk_path_builder_new();
  GskPathBuilder *points = gsk_path_builder_new();

  for (size_t i = 0; i < self->data->n_features; i++) {
    const OsmiumVectorFeature *f = &self->data->features[i];
    const double x0 = f->min_x * scale - cx;
    const double x1 = f->max_x * scale - cx;
    const double y0 = f->min_y * scale - cy;
    const double y1 = f->max_y * scale - cy;

    if (x1 < -reach || x0 > reach || y1 < -reach || y0 > reach) {
      continue;
    }

    if (f->kind != OSMIUM_VECTOR_POINT &&
        fmax(x1 - x0, y1 - y0) < OSMIUM_VECTOR_LAYER_MIN_FEATURE_SIZE) {
      continue;
    }

    GskPathBuilder *target = f->kind == OSMIUM_VECTOR_POLYGON ? polygons
                             : f->kind == OSMIUM_VECTOR_LINE  ? lines
                                                           : points;
    append_feature(f, target, scale, cx, cy, self->point_radius);
  }

  GskPath *polygon_path = gsk_path_builder_free_to_path(polygons);
  GskPath *line_path = gsk_path_builder_free_to_path(lines);
  GskPath *point_path = gsk_path_builder_free_to_path(points);

  GskStroke *stroke = gsk_stroke_new((float)self->line_width);
  gsk_stroke_set_line_join(stroke, GSK_LINE_JOIN_ROUND);
  gsk_stroke_set_line_cap(stroke, GSK_LINE_CAP_ROUND);

  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
  gtk_snapshot_push_clip(snapshot, &bounds);
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(width / 2.0f, height / 2.0f));
  gtk_snapshot_rotate(snapshot, -(float)bearing);

  if (!gsk_path_is_empty(polygon_path)) {
    gtk_snapshot_append_fill(snapshot, polygon_path, GSK_FILL_RULE_EVEN_ODD, &self->fill_color);
    gtk_snapshot_append_stroke(snapshot, polygon_path, stroke, &self->stroke_color);
  }
  if (!gsk_path_is_empty(line_path)) {
    gtk_snapshot_append_stroke(snapshot, line_path, stroke, &self->stroke_color);
  }
  if (!gsk_path_is_empty(point_path)) {
    gtk_snapshot_append_fill(snapshot, point_path, GSK_FILL_RULE_WINDING, &self->stroke_color);
  }

  gtk_snapshot_restore(snapshot);
  gtk_snapshot_pop(snapshot);

  gsk_stroke_free(stroke);
  gsk_path_unref(polygon_path);
  gsk_path_unref(line_path);
  gsk_path_unref(point_path);
}

OsmiumMap *osmium_vector_layer_get_map(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  return self->map;
}

void osmium_vector_layer_set_map(OsmiumVectorLayer *self, OsmiumMap *map) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));
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

  gtk_widget_queue_draw(GTK_WIDGET(self));
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MAP]);
}

const char *osmium_vector_layer_get_path(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  return self->path;
}

void osmium_vector_layer_set_path(OsmiumVectorLayer *self, const char *path) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));

  if (g_strcmp0(self->path, path) == 0) {
    return;
  }

  g_free(self->path);
  self->path = g_strdup(path);

  start_load(self, FALSE);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PATH]);
}

const char *osmium_vector_layer_get_user_agent(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  return self->user_agent;
}

void osmium_vector_layer_set_user_agent(OsmiumVectorLayer *self, const char *user_agent) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));

  if (g_strcmp0(self->user_agent, user_agent) == 0) {
    return;
  }

  g_free(self->user_agent);
  self->user_agent = g_strdup(user_agent);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_USER_AGENT]);
}

void osmium_vector_layer_reload(OsmiumVectorLayer *self) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));

  start_load(self, TRUE);
}

gboolean osmium_vector_layer_get_loading(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  return self->loading;
}

const GdkRGBA *osmium_vector_layer_get_stroke_color(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  return &self->stroke_color;
}

void osmium_vector_layer_set_stroke_color(OsmiumVectorLayer *self, const GdkRGBA *color) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));
  g_return_if_fail(color != NULL);

  if (gdk_rgba_equal(&self->stroke_color, color)) {
    return;
  }

  self->stroke_color = *color;
  gtk_widget_queue_draw(GTK_WIDGET(self));
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STROKE_COLOR]);
}

const GdkRGBA *osmium_vector_layer_get_fill_color(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  return &self->fill_color;
}

void osmium_vector_layer_set_fill_color(OsmiumVectorLayer *self, const GdkRGBA *color) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));
  g_return_if_fail(color != NULL);

  if (gdk_rgba_equal(&self->fill_color, color)) {
    return;
  }

  self->fill_color = *color;
  gtk_widget_queue_draw(GTK_WIDGET(self));
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_FILL_COLOR]);
}

double osmium_vector_layer_get_line_width(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), 0.0);

  return self->line_width;
}

void osmium_vector_layer_set_line_width(OsmiumVectorLayer *self, double width) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));

  if (self->line_width == width) {
    return;
  }

  self->line_width = width;
  gtk_widget_queue_draw(GTK_WIDGET(self));
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LINE_WIDTH]);
}

double osmium_vector_layer_get_point_radius(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), 0.0);

  return self->point_radius;
}

void osmium_vector_layer_set_point_radius(OsmiumVectorLayer *self, double radius) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));

  if (self->point_radius == radius) {
    return;
  }

  self->point_radius = radius;
  gtk_widget_queue_draw(GTK_WIDGET(self));
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_POINT_RADIUS]);
}

guint osmium_vector_layer_get_n_features(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), 0);

  return self->data != NULL ? (guint)self->data->n_features : 0;
}

gboolean osmium_vector_layer_get_extent(OsmiumVectorLayer *self,
                                           double *lat_min,
                                           double *lon_min,
                                           double *lat_max,
                                           double *lon_max) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  if (self->data == NULL || self->data->n_features == 0) {
    return FALSE;
  }

  /* normalized y grows southwards */
  const double y_south = self->data->max_y;
  const double y_north = self->data->min_y;

  *lon_min = self->data->min_x * 360.0 - 180.0;
  *lon_max = self->data->max_x * 360.0 - 180.0;
  *lat_min = atan(sinh(G_PI * (1.0 - 2.0 * y_south))) * 180.0 / G_PI;
  *lat_max = atan(sinh(G_PI * (1.0 - 2.0 * y_north))) * 180.0 / G_PI;
  return TRUE;
}

static void osmium_vector_layer_dispose(GObject *object) {
  OsmiumVectorLayer *self = OSMIUM_VECTOR_LAYER(object);

  if (self->cancellable != NULL) {
    g_cancellable_cancel(self->cancellable);
  }

  if (self->map != NULL) {
    g_signal_handler_disconnect(self->map, self->view_handler);
    self->view_handler = 0;
    g_clear_weak_pointer(&self->map);
  }

  g_clear_object(&self->cancellable);
  g_clear_pointer(&self->data, osmium_vector_data_free);
  g_clear_pointer(&self->path, g_free);
  g_clear_pointer(&self->user_agent, g_free);

  G_OBJECT_CLASS(osmium_vector_layer_parent_class)->dispose(object);
}

static void osmium_vector_layer_set_property(GObject *object,
                                                guint prop_id,
                                                const GValue *value,
                                                GParamSpec *pspec) {
  OsmiumVectorLayer *self = OSMIUM_VECTOR_LAYER(object);

  switch (prop_id) {
  case PROP_MAP:
    osmium_vector_layer_set_map(self, g_value_get_object(value));
    break;
  case PROP_PATH:
    osmium_vector_layer_set_path(self, g_value_get_string(value));
    break;
  case PROP_USER_AGENT:
    osmium_vector_layer_set_user_agent(self, g_value_get_string(value));
    break;
  case PROP_STROKE_COLOR:
    if (g_value_get_boxed(value) != NULL) {
      osmium_vector_layer_set_stroke_color(self, g_value_get_boxed(value));
    }
    break;
  case PROP_FILL_COLOR:
    if (g_value_get_boxed(value) != NULL) {
      osmium_vector_layer_set_fill_color(self, g_value_get_boxed(value));
    }
    break;
  case PROP_LINE_WIDTH:
    osmium_vector_layer_set_line_width(self, g_value_get_double(value));
    break;
  case PROP_POINT_RADIUS:
    osmium_vector_layer_set_point_radius(self, g_value_get_double(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void osmium_vector_layer_get_property(GObject *object,
                                                guint prop_id,
                                                GValue *value,
                                                GParamSpec *pspec) {
  OsmiumVectorLayer *self = OSMIUM_VECTOR_LAYER(object);

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
  case PROP_STROKE_COLOR:
    g_value_set_boxed(value, &self->stroke_color);
    break;
  case PROP_FILL_COLOR:
    g_value_set_boxed(value, &self->fill_color);
    break;
  case PROP_LINE_WIDTH:
    g_value_set_double(value, self->line_width);
    break;
  case PROP_POINT_RADIUS:
    g_value_set_double(value, self->point_radius);
    break;
  case PROP_LOADING:
    g_value_set_boolean(value, self->loading);
    break;
  case PROP_N_FEATURES:
    g_value_set_uint(value, osmium_vector_layer_get_n_features(self));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void osmium_vector_layer_class_init(OsmiumVectorLayerClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  const GParamFlags flags = G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;

  object_class->dispose = osmium_vector_layer_dispose;
  object_class->set_property = osmium_vector_layer_set_property;
  object_class->get_property = osmium_vector_layer_get_property;

  properties[PROP_MAP] =
      g_param_spec_object("map", NULL, "The map this layer draws on", OSMIUM_TYPE_MAP, flags);
  properties[PROP_PATH] =
      g_param_spec_string("path", NULL, "File path or http(s) URL of a vector file (shapefile, GeoJSON, ...)", NULL, flags);
  properties[PROP_USER_AGENT] = g_param_spec_string(
      "user-agent", NULL, "User-Agent sent when the file is downloaded from a URL", "Osmium/0.1", flags);
  properties[PROP_STROKE_COLOR] =
      g_param_spec_boxed("stroke-color", NULL, "Outline and line color", GDK_TYPE_RGBA, flags);
  properties[PROP_FILL_COLOR] =
      g_param_spec_boxed("fill-color", NULL, "Polygon fill color", GDK_TYPE_RGBA, flags);
  properties[PROP_LINE_WIDTH] = g_param_spec_double(
      "line-width", NULL, "Outline and line width in pixels", 0.0, 64.0,
      OSMIUM_VECTOR_LAYER_DEFAULT_LINE_WIDTH, flags);
  properties[PROP_POINT_RADIUS] = g_param_spec_double(
      "point-radius", NULL, "Radius of point features in pixels", 0.0, 64.0,
      OSMIUM_VECTOR_LAYER_DEFAULT_POINT_RADIUS, flags);
  properties[PROP_N_FEATURES] = g_param_spec_uint(
      "n-features", NULL, "Number of features loaded", 0, G_MAXUINT, 0,
      G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_LOADING] = g_param_spec_boolean(
      "loading", NULL, "Whether the file is being read", FALSE,
      G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPS, properties);

  signals[SIGNAL_LOAD_FAILED] = g_signal_new("load-failed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                             NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

  widget_class->snapshot = osmium_vector_layer_snapshot;
  gtk_widget_class_set_css_name(widget_class, "osmium-vector-layer");
}

static void osmium_vector_layer_init(OsmiumVectorLayer *self) {
  GtkWidget *widget = GTK_WIDGET(self);

  gdk_rgba_parse(&self->stroke_color, "#1c71d8");
  gdk_rgba_parse(&self->fill_color, "rgba(28, 113, 216, 0.35)");
  self->user_agent = g_strdup("Osmium/0.1");
  self->line_width = OSMIUM_VECTOR_LAYER_DEFAULT_LINE_WIDTH;
  self->point_radius = OSMIUM_VECTOR_LAYER_DEFAULT_POINT_RADIUS;
  self->cancellable = g_cancellable_new();

  gtk_widget_set_can_target(widget, FALSE);
  gtk_widget_set_can_focus(widget, FALSE);
  gtk_widget_set_overflow(widget, GTK_OVERFLOW_HIDDEN);
}

GtkWidget *osmium_vector_layer_new(OsmiumMap *map) {
  return g_object_new(OSMIUM_TYPE_VECTOR_LAYER, "map", map, NULL);
}
