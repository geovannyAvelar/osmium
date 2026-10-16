/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_vector_layer.h"

#include "osmium_vector_layer_private.h"
#include "vector/vector_data.h"
#include "tms/tms.h"
#include <math.h>
#include <gio/gio.h>

#define OSMIUM_VECTOR_LAYER_DEFAULT_LINE_WIDTH 1.5
#define OSMIUM_VECTOR_LAYER_DEFAULT_POINT_RADIUS 4.0
#define OSMIUM_VECTOR_LAYER_RENDER_DELAY_MS 120
#define OSMIUM_VECTOR_LAYER_MAX_SIZE 4096
#define OSMIUM_VECTOR_LAYER_MARGIN 1.5
#define OSMIUM_VECTOR_LAYER_MAX_THREADS 8
#define OSMIUM_VECTOR_LAYER_MAX_HITS 50
#define OSMIUM_VECTOR_LAYER_MIN_BAND_ROWS 64
#define OSMIUM_VECTOR_LAYER_MIN_FEATURE_SIZE 0.5
#define OSMIUM_VECTOR_LAYER_MIN_VERTEX_STEP 0.5

typedef struct {
  char *path;
  char *user_agent;
  char *source_crs;
  OsmiumProjection *projection;
  guint generation;
} LoadJob;

typedef struct {
  OsmiumVectorData *data;
  OsmiumProjection *projection;
  guint id;
  guint zoom;
  double x;
  double y;
  int width;
  int height;
  GdkRGBA stroke_color;
  GdkRGBA fill_color;
  double line_width;
  double point_radius;
} RenderJob;

OSMIUM_API GType osmium_vector_layer_get_type(void);

G_DEFINE_TYPE(OsmiumVectorLayer, osmium_vector_layer, GTK_TYPE_WIDGET)

enum {
  PROP_0,
  PROP_MAP,
  PROP_PATH,
  PROP_USER_AGENT,
  PROP_SOURCE_CRS,
  PROP_STROKE_COLOR,
  PROP_FILL_COLOR,
  PROP_LINE_WIDTH,
  PROP_POINT_RADIUS,
  PROP_N_FEATURES,
  PROP_LOADING,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

enum { SIGNAL_LOAD_FAILED, SIGNAL_EDITING_CHANGED, N_SIGNALS };

static guint signals[N_SIGNALS];

static void render_now(OsmiumVectorLayer *self);
static void rebuild_selected_indices(OsmiumVectorLayer *self);
static void image_free(gpointer data);

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
  g_free(job->source_crs);
  g_clear_pointer(&job->projection, osmium_projection_unref);
  g_free(job);
}

static void load_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
  LoadJob *job = task_data;
  OsmiumVectorData *loaded = NULL;

  osmium_error err = osmium_vector_data_open(job->path, job->user_agent, job->source_crs, job->projection, &loaded);
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
  if (self->session != NULL) {
    osmium_edit_session_free(self->session);
    self->session = osmium_edit_session_new(self->data);
  }
  rebuild_selected_indices(self);

  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_N_FEATURES]);
  render_now(self);
}

static void start_load(OsmiumVectorLayer *self, gboolean keep_data) {
  self->generation++;

  if (!keep_data) {
    g_clear_pointer(&self->image, image_free);
    g_clear_pointer(&self->data, osmium_vector_data_free);
    g_array_set_size(self->selected_indices, 0);
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
  job->source_crs = g_strdup(self->source_crs);
  job->projection = self->map != NULL ? osmium_projection_ref(osmium_map_get_projection(self->map))
                                      : osmium_projection_new("EPSG:3857", NULL, 0);
  job->generation = self->generation;

  GTask *task = g_task_new(self, self->cancellable, on_loaded, NULL);
  g_task_set_task_data(task, job, load_job_free);
  g_task_run_in_thread(task, load_worker);
  g_object_unref(task);
}

static void schedule_render(OsmiumVectorLayer *self);

static void on_map_crs_changed(GObject *map, GParamSpec *pspec, gpointer user_data) {
  OsmiumVectorLayer *self = OSMIUM_VECTOR_LAYER(user_data);

  g_clear_pointer(&self->image, image_free);
  schedule_render(self);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void on_map_view_changed(OsmiumMap *map, gpointer user_data) {
  schedule_render(OSMIUM_VECTOR_LAYER(user_data));
  gtk_widget_queue_draw(GTK_WIDGET(user_data));
}

static void render_job_free(gpointer data) {
  RenderJob *job = data;

  osmium_vector_data_free(job->data);
  g_clear_pointer(&job->projection, osmium_projection_unref);
  g_free(job);
}

static void image_free(gpointer data) {
  LayerImage *image = data;

  if (image == NULL) {
    return;
  }
  g_clear_object(&image->texture);
  g_free(image);
}

static void trace_feature(cairo_t *cr,
                          const OsmiumVectorFeature *f,
                          double scale,
                          double origin_x,
                          double origin_y,
                          double point_radius) {
  for (size_t p = 0; p < f->n_parts; p++) {
    const size_t start = f->part_start[p];
    const size_t end = f->part_start[p + 1];
    size_t last = end;

    if (f->kind == OSMIUM_VECTOR_POINT) {
      cairo_new_sub_path(cr);
      cairo_arc(cr, f->xy[start * 2] * scale - origin_x, f->xy[start * 2 + 1] * scale - origin_y,
                point_radius, 0.0, 2.0 * G_PI);
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
      const double x = f->xy[i * 2] * scale - origin_x;
      const double y = f->xy[i * 2 + 1] * scale - origin_y;

      if (i == start) {
        cairo_move_to(cr, x, y);
      } else {
        const double dx = x - kept_x;
        const double dy = y - kept_y;

        if (i != last - 1 && dx * dx + dy * dy < OSMIUM_VECTOR_LAYER_MIN_VERTEX_STEP *
                                                    OSMIUM_VECTOR_LAYER_MIN_VERTEX_STEP) {
          continue;
        }
        cairo_line_to(cr, x, y);
      }

      kept_x = x;
      kept_y = y;
    }

    if (f->kind == OSMIUM_VECTOR_POLYGON) {
      cairo_close_path(cr);
    }
  }
}

static void trace_kind(cairo_t *cr,
                       const RenderJob *job,
                       OsmiumVectorKind kind,
                       double scale,
                       double origin_x,
                       double origin_y,
                       int height) {
  const double reach = job->line_width + job->point_radius;

  for (size_t i = 0; i < job->data->n_features; i++) {
    const OsmiumVectorFeature *f = &job->data->features[i];

    if (f->kind != kind || f->deleted) {
      continue;
    }

    const double x0 = f->min_x * scale - origin_x;
    const double x1 = f->max_x * scale - origin_x;
    const double y0 = f->min_y * scale - origin_y;
    const double y1 = f->max_y * scale - origin_y;

    if (x1 < -reach || x0 > job->width + reach || y1 < -reach || y0 > height + reach) {
      continue;
    }

    if (kind != OSMIUM_VECTOR_POINT &&
        fmax(x1 - x0, y1 - y0) < OSMIUM_VECTOR_LAYER_MIN_FEATURE_SIZE) {
      continue;
    }

    trace_feature(cr, f, scale, origin_x, origin_y, job->point_radius);
  }
}

static void set_source(cairo_t *cr, const GdkRGBA *color) {
  cairo_set_source_rgba(cr, color->red, color->green, color->blue, color->alpha);
}

typedef struct {
  const RenderJob *job;
  unsigned char *pixels;
  int stride;
  int first_row;
  int rows;
} Band;

static gpointer render_band(gpointer data) {
  Band *band = data;
  const RenderJob *job = band->job;
  const double scale = 256.0 * ldexp(1.0, (int)job->zoom);
  const double origin_y = job->y + band->first_row;

  cairo_surface_t *surface = cairo_image_surface_create_for_data(
      band->pixels + (gsize)band->first_row * (gsize)band->stride, CAIRO_FORMAT_ARGB32, job->width,
      band->rows, band->stride);
  cairo_t *cr = cairo_create(surface);

  cairo_set_line_width(cr, job->line_width);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  trace_kind(cr, job, OSMIUM_VECTOR_POLYGON, scale, job->x, origin_y, band->rows);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  set_source(cr, &job->fill_color);
  cairo_fill_preserve(cr);
  set_source(cr, &job->stroke_color);
  cairo_stroke(cr);

  trace_kind(cr, job, OSMIUM_VECTOR_LINE, scale, job->x, origin_y, band->rows);
  set_source(cr, &job->stroke_color);
  cairo_stroke(cr);

  trace_kind(cr, job, OSMIUM_VECTOR_POINT, scale, job->x, origin_y, band->rows);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
  cairo_fill(cr);

  cairo_destroy(cr);
  cairo_surface_finish(surface);
  cairo_surface_destroy(surface);
  return NULL;
}

static void render_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
  RenderJob *job = task_data;

  if (g_cancellable_is_cancelled(cancellable)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
    return;
  }

  if (!osmium_vector_data_begin_read(job->data, job->projection)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "could not project the features");
    return;
  }

  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, job->width);
  const gsize size = (gsize)stride * (gsize)job->height;
  unsigned char *pixels = g_malloc0(size);

  /* horizontal bands render in parallel; they share nothing but the read-only data */
  const int n_bands = CLAMP((int)g_get_num_processors(), 1, OSMIUM_VECTOR_LAYER_MAX_THREADS);
  const int rows_per_band = (job->height + n_bands - 1) / n_bands;
  Band bands[OSMIUM_VECTOR_LAYER_MAX_THREADS];
  GThread *threads[OSMIUM_VECTOR_LAYER_MAX_THREADS] = {0};
  int used = 0;

  for (int first = 0; first < job->height; first += rows_per_band) {
    Band *band = &bands[used];

    band->job = job;
    band->pixels = pixels;
    band->stride = stride;
    band->first_row = first;
    band->rows = MIN(rows_per_band, job->height - first);

    if (used + 1 < n_bands && job->height > 2 * OSMIUM_VECTOR_LAYER_MIN_BAND_ROWS) {
      threads[used] = g_thread_new("osmium-vector-band", render_band, band);
    } else {
      render_band(band);
    }
    used++;
  }

  for (int i = 0; i < used; i++) {
    if (threads[i] != NULL) {
      g_thread_join(threads[i]);
    }
  }
  osmium_vector_data_end_read(job->data);

  g_autoptr(GBytes) bytes = g_bytes_new_take(pixels, size);
  GdkTexture *texture = gdk_memory_texture_new(
      job->width, job->height,
      G_BYTE_ORDER == G_LITTLE_ENDIAN ? GDK_MEMORY_B8G8R8A8_PREMULTIPLIED
                                      : GDK_MEMORY_A8R8G8B8_PREMULTIPLIED,
      bytes, (gsize)stride);

  g_task_return_pointer(task, texture, g_object_unref);
}

static void on_rendered(GObject *source, GAsyncResult *result, gpointer user_data) {
  OsmiumVectorLayer *self = OSMIUM_VECTOR_LAYER(source);
  RenderJob *job = g_task_get_task_data(G_TASK(result));
  g_autoptr(GError) error = NULL;
  GdkTexture *texture = g_task_propagate_pointer(G_TASK(result), &error);

  if (texture == NULL) {
    return;
  }

  if (self->map == NULL || job->data != self->data ||
      job->projection != osmium_map_get_projection(self->map) ||
      (self->image != NULL && self->image->id > job->id)) {
    g_object_unref(texture);
    return;
  }

  image_free(self->image);
  self->image = g_new0(LayerImage, 1);
  self->image->texture = texture;
  self->image->id = job->id;
  self->image->zoom = job->zoom;
  self->image->x = job->x;
  self->image->y = job->y;
  self->image->width = job->width;
  self->image->height = job->height;

  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void render_now(OsmiumVectorLayer *self) {
  GtkWidget *widget = GTK_WIDGET(self);
  const int width = gtk_widget_get_width(widget);
  const int height = gtk_widget_get_height(widget);

  if (self->render_source != 0) {
    g_source_remove(self->render_source);
    self->render_source = 0;
  }

  if (self->map == NULL || self->data == NULL || self->data->n_features == 0 || width <= 0 ||
      height <= 0) {
    return;
  }

  const guint zoom = osmium_map_get_zoom(self->map);
  const double rad = osmium_map_get_bearing(self->map) * G_PI / 180.0;
  const double view_width = width * fabs(cos(rad)) + height * fabs(sin(rad));
  const double view_height = width * fabs(sin(rad)) + height * fabs(cos(rad));

  double center_x, center_y;
  osmium_map_lonlat_to_world(self->map, osmium_map_get_lat(self->map), osmium_map_get_lon(self->map), zoom,
                             &center_x, &center_y);

  RenderJob *job = g_new0(RenderJob, 1);
  job->data = osmium_vector_data_ref(self->data);
  job->projection = osmium_projection_ref(osmium_map_get_projection(self->map));
  job->id = ++self->last_request_id;
  job->zoom = zoom;
  job->width = CLAMP((int)ceil(view_width * OSMIUM_VECTOR_LAYER_MARGIN), 1, OSMIUM_VECTOR_LAYER_MAX_SIZE);
  job->height = CLAMP((int)ceil(view_height * OSMIUM_VECTOR_LAYER_MARGIN), 1, OSMIUM_VECTOR_LAYER_MAX_SIZE);
  job->x = floor(center_x - job->width / 2.0);
  job->y = floor(center_y - job->height / 2.0);
  job->stroke_color = self->stroke_color;
  job->fill_color = self->fill_color;
  job->line_width = self->line_width;
  job->point_radius = self->point_radius;

  GTask *task = g_task_new(self, self->cancellable, on_rendered, NULL);
  g_task_set_task_data(task, job, render_job_free);
  g_task_run_in_thread(task, render_worker);
  g_object_unref(task);
}

static gboolean on_render_timeout(gpointer user_data) {
  OsmiumVectorLayer *self = OSMIUM_VECTOR_LAYER(user_data);

  self->render_source = 0;
  render_now(self);
  return G_SOURCE_REMOVE;
}

static void schedule_render(OsmiumVectorLayer *self) {
  if (self->render_source != 0) {
    g_source_remove(self->render_source);
  }
  self->render_source = g_timeout_add(OSMIUM_VECTOR_LAYER_RENDER_DELAY_MS, on_render_timeout, self);
}

static void append_selected_feature(GskPathBuilder *builder,
                                   const OsmiumVectorFeature *f,
                                   double scale,
                                   double cx,
                                   double cy,
                                   double point_radius) {
  for (size_t p = 0; p < f->n_parts; p++) {
    const size_t start = f->part_start[p];
    const size_t end = f->part_start[p + 1];

    if (f->kind == OSMIUM_VECTOR_POINT) {
      gsk_path_builder_add_circle(
          builder,
          &GRAPHENE_POINT_INIT((float)(f->xy[start * 2] * scale - cx),
                               (float)(f->xy[start * 2 + 1] * scale - cy)),
          (float)point_radius);
      continue;
    }

    for (size_t i = start; i < end; i++) {
      const float x = (float)(f->xy[i * 2] * scale - cx);
      const float y = (float)(f->xy[i * 2 + 1] * scale - cy);

      if (i == start) {
        gsk_path_builder_move_to(builder, x, y);
      } else {
        gsk_path_builder_line_to(builder, x, y);
      }
    }

    if (f->kind == OSMIUM_VECTOR_POLYGON) {
      gsk_path_builder_close(builder);
    }
  }
}

static void draw_selection(OsmiumVectorLayer *self,
                           GtkSnapshot *snapshot,
                           float width,
                           float height,
                           double center_x,
                           double center_y) {
  static const GdkRGBA outline = {1.0f, 0.68f, 0.0f, 1.0f};
  static const GdkRGBA fill = {1.0f, 0.68f, 0.0f, 0.35f};

  if (self->data == NULL || self->selected_indices->len == 0 ||
      !osmium_vector_data_try_begin_read(self->data, osmium_map_get_projection(self->map))) {
    return;
  }

  const double scale = 256.0 * ldexp(1.0, (int)osmium_map_get_zoom(self->map));
  GskPathBuilder *areas = gsk_path_builder_new();
  GskPathBuilder *lines = gsk_path_builder_new();
  GskPathBuilder *points = gsk_path_builder_new();

  for (guint i = 0; i < self->selected_indices->len; i++) {
    const OsmiumVectorFeature *f = &self->data->features[g_array_index(self->selected_indices, size_t, i)];
    GskPathBuilder *target = f->kind == OSMIUM_VECTOR_POLYGON ? areas
                             : f->kind == OSMIUM_VECTOR_LINE  ? lines
                                                              : points;

    append_selected_feature(target, f, scale, center_x, center_y, self->point_radius + 2.0);
  }

  GskPath *area_path = gsk_path_builder_free_to_path(areas);
  GskPath *line_path = gsk_path_builder_free_to_path(lines);
  GskPath *point_path = gsk_path_builder_free_to_path(points);
  GskStroke *stroke = gsk_stroke_new(MAX((float)self->line_width + 1.5f, 3.0f));

  gsk_stroke_set_line_join(stroke, GSK_LINE_JOIN_ROUND);
  gsk_stroke_set_line_cap(stroke, GSK_LINE_CAP_ROUND);

  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(width / 2.0f, height / 2.0f));
  gtk_snapshot_rotate(snapshot, -(float)osmium_map_get_bearing(self->map));

  if (!gsk_path_is_empty(area_path)) {
    gtk_snapshot_append_fill(snapshot, area_path, GSK_FILL_RULE_EVEN_ODD, &fill);
    gtk_snapshot_append_stroke(snapshot, area_path, stroke, &outline);
  }
  if (!gsk_path_is_empty(line_path)) {
    gtk_snapshot_append_stroke(snapshot, line_path, stroke, &outline);
  }
  if (!gsk_path_is_empty(point_path)) {
    gtk_snapshot_append_fill(snapshot, point_path, GSK_FILL_RULE_WINDING, &outline);
  }

  gtk_snapshot_restore(snapshot);

  gsk_stroke_free(stroke);
  gsk_path_unref(area_path);
  gsk_path_unref(line_path);
  gsk_path_unref(point_path);
  osmium_vector_data_end_read(self->data);
}

static void osmium_vector_layer_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  OsmiumVectorLayer *self = OSMIUM_VECTOR_LAYER(widget);

  if (self->map == NULL || (self->image == NULL && self->selected_indices->len == 0 && self->session == NULL)) {
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

  if (self->image != NULL) {
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
  }

  draw_selection(self, snapshot, width, height, center_x, center_y);
  osmium_editor_draw(self, snapshot, width, height, center_x, center_y);
  gtk_snapshot_pop(snapshot);
}

static void osmium_vector_layer_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
  schedule_render(OSMIUM_VECTOR_LAYER(widget));
}

void osmium_feature_field_free(OsmiumFeatureField *field) {
  if (field == NULL) {
    return;
  }
  g_free(field->name);
  g_free(field->value);
  g_free(field);
}

void osmium_vector_layer_rebuild_selection(OsmiumVectorLayer *self) {
  rebuild_selected_indices(self);
}

void osmium_vector_layer_emit_editing_changed(OsmiumVectorLayer *self) {
  g_signal_emit(self, signals[SIGNAL_EDITING_CHANGED], 0);
}

void osmium_vector_layer_refresh(OsmiumVectorLayer *self) {
  rebuild_selected_indices(self);
  render_now(self);
  gtk_widget_queue_draw(GTK_WIDGET(self));
  osmium_vector_layer_emit_editing_changed(self);
}

static void rebuild_selected_indices(OsmiumVectorLayer *self) {
  g_array_set_size(self->selected_indices, 0);

  if (self->data == NULL || self->selection->len == 0) {
    return;
  }

  for (size_t i = 0; i < self->data->n_features; i++) {
    for (guint s = 0; s < self->selection->len; s++) {
      if (!self->data->features[i].deleted && self->data->features[i].fid == g_array_index(self->selection, gint64, s)) {
        g_array_append_val(self->selected_indices, i);
        break;
      }
    }
  }
}

GArray *osmium_vector_layer_identify(OsmiumVectorLayer *self,
                                     double lat,
                                     double lon,
                                     double tolerance_px) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  GArray *result = g_array_new(FALSE, FALSE, sizeof(gint64));
  long long fids[OSMIUM_VECTOR_LAYER_MAX_HITS];

  if (self->map == NULL || self->data == NULL) {
    return result;
  }

  double x, y;
  OsmiumProjection *projection = osmium_map_get_projection(self->map);

  if (!osmium_projection_forward(projection, lon, lat, &x, &y) ||
      !osmium_vector_data_begin_read(self->data, projection)) {
    return result;
  }

  const double tolerance = tolerance_px / (256.0 * ldexp(1.0, (int)osmium_map_get_zoom(self->map)));

  const size_t count = osmium_vector_data_hit_test(self->data, x, y, tolerance, fids,
                                                   OSMIUM_VECTOR_LAYER_MAX_HITS);
  osmium_vector_data_end_read(self->data);
  for (size_t i = 0; i < count; i++) {
    gint64 fid = fids[i];

    g_array_append_val(result, fid);
  }
  return result;
}

GPtrArray *osmium_vector_layer_get_feature_attributes(OsmiumVectorLayer *self, gint64 fid) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  OsmiumVectorField *fields = NULL;
  size_t n_fields = 0;

  if (self->data == NULL) {
    return NULL;
  }

  const ptrdiff_t index = osmium_vector_data_find_fid(self->data, fid);

  if (index < 0 || self->data->features[index].deleted) {
    return NULL;
  }

  /* features created in an edit session have no row in the file yet */
  const osmium_error err = fid < 0 ? osmium_vector_data_get_fields(self->data, &fields, &n_fields)
                                   : osmium_vector_data_get_attributes(self->data, fid, &fields, &n_fields);

  if (err != OSMIUM_OK) {
    return NULL;
  }

  GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)osmium_feature_field_free);
  for (size_t i = 0; i < n_fields; i++) {
    OsmiumFeatureField *field = g_new0(OsmiumFeatureField, 1);
    const char *edited = self->session != NULL ? osmium_edit_get_override(self->session, fid, fields[i].name) : NULL;

    field->name = g_strdup(fields[i].name);
    field->value = g_strdup(edited != NULL ? edited : fields[i].value);
    field->type = (OsmiumFeatureFieldType)fields[i].type;
    field->width = fields[i].width;
    g_ptr_array_add(result, field);
  }

  osmium_vector_fields_free(fields, n_fields);
  return result;
}

gboolean osmium_vector_layer_is_editable(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  return self->data != NULL && osmium_vector_data_is_editable(self->data);
}

GPtrArray *osmium_vector_layer_get_fields(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  OsmiumVectorField *fields = NULL;
  size_t n_fields = 0;

  if (self->data == NULL ||
      osmium_vector_data_get_fields(self->data, &fields, &n_fields) != OSMIUM_OK) {
    return NULL;
  }

  GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)osmium_feature_field_free);
  for (size_t i = 0; i < n_fields; i++) {
    OsmiumFeatureField *field = g_new0(OsmiumFeatureField, 1);

    field->name = g_strdup(fields[i].name);
    field->value = g_strdup("");
    field->type = (OsmiumFeatureFieldType)fields[i].type;
    field->width = fields[i].width;
    g_ptr_array_add(result, field);
  }

  osmium_vector_fields_free(fields, n_fields);
  return result;
}

static gboolean check_writable(OsmiumVectorLayer *self, GError **error) {
  if (self->session != NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Save or discard the layer's edits first");
    return FALSE;
  }
  if (self->data == NULL || !osmium_vector_data_is_editable(self->data)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_READ_ONLY,
                        "This layer is read-only (remote or not writable file)");
    return FALSE;
  }
  return TRUE;
}

static gboolean finish_write(OsmiumVectorLayer *self, osmium_error err, const char *message, GError **error) {
  osmium_vector_data_release(self->data);

  if (err != OSMIUM_OK) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, *message != '\0' ? message : "Could not write the file");
    return FALSE;
  }
  return TRUE;
}

gboolean osmium_vector_layer_set_attributes(OsmiumVectorLayer *self,
                                            const gint64 *fids,
                                            const char *const *fields,
                                            const char *const *values,
                                            guint n_edits,
                                            GError **error) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  char message[256] = "";

  if (!check_writable(self, error)) {
    return FALSE;
  }

  OsmiumVectorEdit *edits = g_new0(OsmiumVectorEdit, n_edits);
  for (guint i = 0; i < n_edits; i++) {
    edits[i].fid = fids[i];
    edits[i].field = fields[i];
    edits[i].value = values[i];
  }

  osmium_vector_data_release(self->data);
  osmium_error err = osmium_vector_write_attributes(self->data->source_path, edits, n_edits, message, sizeof(message));
  g_free(edits);
  return finish_write(self, err, message, error);
}

gboolean osmium_vector_layer_add_field(OsmiumVectorLayer *self,
                                       const char *name,
                                       OsmiumFeatureFieldType type,
                                       GError **error) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  char message[256] = "";

  if (!check_writable(self, error)) {
    return FALSE;
  }

  osmium_vector_data_release(self->data);
  osmium_error err = osmium_vector_add_field(self->data->source_path, name, (OsmiumFieldType)type, message, sizeof(message));
  return finish_write(self, err, message, error);
}

gboolean osmium_vector_layer_rename_field(OsmiumVectorLayer *self,
                                          const char *old_name,
                                          const char *new_name,
                                          GError **error) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  char message[256] = "";

  if (!check_writable(self, error)) {
    return FALSE;
  }

  osmium_vector_data_release(self->data);
  osmium_error err = osmium_vector_rename_field(self->data->source_path, old_name, new_name, message, sizeof(message));
  return finish_write(self, err, message, error);
}

gboolean osmium_vector_layer_delete_field(OsmiumVectorLayer *self, const char *name, GError **error) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  char message[256] = "";

  if (!check_writable(self, error)) {
    return FALSE;
  }

  osmium_vector_data_release(self->data);
  osmium_error err = osmium_vector_delete_field(self->data->source_path, name, message, sizeof(message));
  return finish_write(self, err, message, error);
}

void osmium_vector_layer_set_selection(OsmiumVectorLayer *self, const gint64 *fids, guint n_fids) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));

  g_array_set_size(self->selection, 0);
  if (n_fids > 0) {
    g_array_append_vals(self->selection, fids, n_fids);
  }

  rebuild_selected_indices(self);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

GArray *osmium_vector_layer_get_selection(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  return g_array_ref(self->selection);
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
    osmium_editor_detach(self, self->map);
    g_signal_handler_disconnect(self->map, self->crs_handler);
    self->view_handler = 0;
    self->crs_handler = 0;
    g_clear_weak_pointer(&self->map);
  }

  if (map != NULL) {
    g_set_weak_pointer(&self->map, map);
    self->view_handler = g_signal_connect(map, "view-changed", G_CALLBACK(on_map_view_changed), self);
    self->crs_handler = g_signal_connect(map, "notify::crs", G_CALLBACK(on_map_crs_changed), self);
    osmium_editor_attach(self, map);
  }

  g_clear_pointer(&self->image, image_free);
  schedule_render(self);
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

const char *osmium_vector_layer_get_source_crs(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  return self->source_crs;
}

void osmium_vector_layer_set_source_crs(OsmiumVectorLayer *self, const char *source_crs) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));

  const char *wanted = source_crs != NULL && *source_crs != '\0' ? source_crs : NULL;

  if (g_strcmp0(self->source_crs, wanted) == 0) {
    return;
  }

  g_free(self->source_crs);
  self->source_crs = g_strdup(wanted);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SOURCE_CRS]);

  if (self->path != NULL) {
    start_load(self, TRUE);
  }
}

gboolean osmium_vector_layer_get_crs_info(OsmiumVectorLayer *self, OsmiumLayerCrsInfo *info) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  if (self->data == NULL) {
    return FALSE;
  }

  info->name = g_strdup(self->data->crs_name);
  info->code = g_strdup(self->data->crs_code);
  info->assumed = self->data->crs_assumed;
  info->ballpark = self->data->crs_ballpark;
  info->accuracy = self->data->crs_accuracy;
  return TRUE;
}

void osmium_layer_crs_info_clear(OsmiumLayerCrsInfo *info) {
  g_clear_pointer(&info->name, g_free);
  g_clear_pointer(&info->code, g_free);
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
  render_now(self);
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
  render_now(self);
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
  render_now(self);
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
  render_now(self);
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

  *lon_min = self->data->lon_min;
  *lon_max = self->data->lon_max;
  *lat_min = self->data->lat_min;
  *lat_max = self->data->lat_max;
  return TRUE;
}

static void osmium_vector_layer_dispose(GObject *object) {
  OsmiumVectorLayer *self = OSMIUM_VECTOR_LAYER(object);

  if (self->cancellable != NULL) {
    g_cancellable_cancel(self->cancellable);
  }

  if (self->map != NULL) {
    g_signal_handler_disconnect(self->map, self->view_handler);
    g_signal_handler_disconnect(self->map, self->crs_handler);
    self->view_handler = 0;
    self->crs_handler = 0;
    g_clear_weak_pointer(&self->map);
  }

  if (self->render_source != 0) {
    g_source_remove(self->render_source);
    self->render_source = 0;
  }

  g_clear_object(&self->cancellable);
  g_clear_pointer(&self->image, image_free);
  g_clear_pointer(&self->session, osmium_edit_session_free);
  osmium_editor_finalize(self);
  g_clear_pointer(&self->selection, g_array_unref);
  g_clear_pointer(&self->selected_indices, g_array_unref);
  g_clear_pointer(&self->data, osmium_vector_data_free);
  g_clear_pointer(&self->path, g_free);
  g_clear_pointer(&self->user_agent, g_free);
  g_clear_pointer(&self->source_crs, g_free);

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
  case PROP_SOURCE_CRS:
    osmium_vector_layer_set_source_crs(self, g_value_get_string(value));
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
  case PROP_SOURCE_CRS:
    g_value_set_string(value, self->source_crs);
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
  properties[PROP_SOURCE_CRS] = g_param_spec_string(
      "source-crs", NULL, "Coordinate system of the file when it is missing or wrong (empty = use the file's)", NULL, flags);
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

  signals[SIGNAL_EDITING_CHANGED] = g_signal_new("editing-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_LOAD_FAILED] = g_signal_new("load-failed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                             NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

  widget_class->snapshot = osmium_vector_layer_snapshot;
  widget_class->size_allocate = osmium_vector_layer_size_allocate;
  gtk_widget_class_set_css_name(widget_class, "osmium-vector-layer");
}

static void osmium_vector_layer_init(OsmiumVectorLayer *self) {
  GtkWidget *widget = GTK_WIDGET(self);

  gdk_rgba_parse(&self->stroke_color, "#1c71d8");
  gdk_rgba_parse(&self->fill_color, "rgba(28, 113, 216, 0.35)");
  self->user_agent = g_strdup("Osmium/0.1");
  osmium_editor_init(self);
  self->selection = g_array_new(FALSE, FALSE, sizeof(gint64));
  self->selected_indices = g_array_new(FALSE, FALSE, sizeof(size_t));
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
