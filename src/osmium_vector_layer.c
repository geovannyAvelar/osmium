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
  OsmiumSymbol *symbols;
  guint n_symbols;
  guint16 *index;
  size_t n_index;
  GArray **buckets;
  double reach;
} RenderJob;

#define NO_SYMBOL 0xFFFF

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

enum { SIGNAL_LOAD_FAILED, SIGNAL_EDITING_CHANGED, SIGNAL_SELECTION_CHANGED, SIGNAL_FEATURE_ADDED, N_SIGNALS };

static guint signals[N_SIGNALS];

static void render_now(OsmiumVectorLayer *self);
static void invalidate_columns(OsmiumVectorLayer *self);
static GPtrArray *column_for(OsmiumVectorLayer *self, const char *field);
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
  osmium_join_rebuild(self);
  invalidate_columns(self);
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
  for (guint i = 0; job->buckets != NULL && i < job->n_symbols; i++) {
    g_clear_pointer(&job->buckets[i], g_array_unref);
  }
  g_free(job->buckets);
  g_free(job->symbols);
  g_free(job->index);
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

static void trace_marker(cairo_t *cr, OsmiumPointShape shape, double x, double y, double r) {
  switch (shape) {
  case OSMIUM_POINT_SQUARE:
    cairo_rectangle(cr, x - r, y - r, 2 * r, 2 * r);
    break;
  case OSMIUM_POINT_DIAMOND:
    cairo_move_to(cr, x, y - r * 1.3);
    cairo_line_to(cr, x + r * 1.3, y);
    cairo_line_to(cr, x, y + r * 1.3);
    cairo_line_to(cr, x - r * 1.3, y);
    cairo_close_path(cr);
    break;
  case OSMIUM_POINT_TRIANGLE:
    cairo_move_to(cr, x, y - r * 1.2);
    cairo_line_to(cr, x + r * 1.1, y + r * 0.9);
    cairo_line_to(cr, x - r * 1.1, y + r * 0.9);
    cairo_close_path(cr);
    break;
  case OSMIUM_POINT_CROSS:
    cairo_move_to(cr, x - r, y);
    cairo_line_to(cr, x + r, y);
    cairo_move_to(cr, x, y - r);
    cairo_line_to(cr, x, y + r);
    break;
  case OSMIUM_POINT_CIRCLE:
  default:
    cairo_new_sub_path(cr);
    cairo_arc(cr, x, y, r, 0.0, 2.0 * G_PI);
    break;
  }
}

static void trace_feature(cairo_t *cr,
                          const OsmiumVectorFeature *f,
                          double scale,
                          double origin_x,
                          double origin_y,
                          const OsmiumSymbol *symbol) {
  for (size_t p = 0; p < f->n_parts; p++) {
    const size_t start = f->part_start[p];
    const size_t end = f->part_start[p + 1];
    size_t last = end;

    if (f->kind == OSMIUM_VECTOR_POINT) {
      trace_marker(cr, symbol->shape, f->xy[start * 2] * scale - origin_x, f->xy[start * 2 + 1] * scale - origin_y,
                   symbol->point_radius);
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

static gboolean trace_bucket(cairo_t *cr,
                             const RenderJob *job,
                             guint symbol,
                             OsmiumVectorKind kind,
                             double scale,
                             double origin_x,
                             double origin_y,
                             int height) {
  const GArray *bucket = job->buckets[symbol];
  const OsmiumSymbol *style = &job->symbols[symbol];
  const double reach = job->reach;
  gboolean traced = FALSE;

  for (guint k = 0; k < bucket->len; k++) {
    const OsmiumVectorFeature *f = &job->data->features[g_array_index(bucket, size_t, k)];

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

    trace_feature(cr, f, scale, origin_x, origin_y, style);
    traced = TRUE;
  }
  return traced;
}

static void set_source(cairo_t *cr, const GdkRGBA *color) {
  cairo_set_source_rgba(cr, color->red, color->green, color->blue, color->alpha);
}

static void set_line(cairo_t *cr, const OsmiumSymbol *symbol) {
  const double w = MAX(symbol->line_width, 1.0);
  double pattern[4];
  int n = 0;

  switch (symbol->line_style) {
  case OSMIUM_LINE_DASH:
    pattern[0] = 5 * w;
    pattern[1] = 3 * w;
    n = 2;
    break;
  case OSMIUM_LINE_DOT:
    pattern[0] = w;
    pattern[1] = 2 * w;
    n = 2;
    break;
  case OSMIUM_LINE_DASH_DOT:
    pattern[0] = 5 * w;
    pattern[1] = 2 * w;
    pattern[2] = w;
    pattern[3] = 2 * w;
    n = 4;
    break;
  case OSMIUM_LINE_SOLID:
  default:
    break;
  }

  cairo_set_line_width(cr, symbol->line_width);
  cairo_set_dash(cr, pattern, n, 0.0);
  cairo_set_line_cap(cr, n > 0 ? CAIRO_LINE_CAP_BUTT : CAIRO_LINE_CAP_ROUND);
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

  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

  for (guint s = 0; s < job->n_symbols; s++) {
    const OsmiumSymbol *symbol = &job->symbols[s];

    if (trace_bucket(cr, job, s, OSMIUM_VECTOR_POLYGON, scale, job->x, origin_y, band->rows)) {
      cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
      set_source(cr, &symbol->fill);
      cairo_fill_preserve(cr);
      set_line(cr, symbol);
      set_source(cr, &symbol->stroke);
      cairo_stroke(cr);
    }
  }

  for (guint s = 0; s < job->n_symbols; s++) {
    const OsmiumSymbol *symbol = &job->symbols[s];

    if (trace_bucket(cr, job, s, OSMIUM_VECTOR_LINE, scale, job->x, origin_y, band->rows)) {
      set_line(cr, symbol);
      set_source(cr, &symbol->stroke);
      cairo_stroke(cr);
    }
  }

  for (guint s = 0; s < job->n_symbols; s++) {
    const OsmiumSymbol *symbol = &job->symbols[s];

    if (trace_bucket(cr, job, s, OSMIUM_VECTOR_POINT, scale, job->x, origin_y, band->rows)) {
      cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
      if (symbol->shape == OSMIUM_POINT_CROSS) {
        set_line(cr, symbol);
        set_source(cr, &symbol->stroke);
        cairo_stroke(cr);
      } else {
        set_source(cr, &symbol->fill);
        cairo_fill_preserve(cr);
        set_line(cr, symbol);
        set_source(cr, &symbol->stroke);
        cairo_stroke(cr);
      }
    }
  }

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

  job->buckets = g_new0(GArray *, job->n_symbols);
  for (guint s = 0; s < job->n_symbols; s++) {
    job->buckets[s] = g_array_new(FALSE, FALSE, sizeof(size_t));
  }
  for (size_t i = 0; i < job->data->n_features; i++) {
    const guint symbol = i < job->n_index && job->index != NULL ? job->index[i] : (job->index == NULL ? 0 : NO_SYMBOL);

    if (symbol < job->n_symbols) {
      g_array_append_val(job->buckets[symbol], i);
    }
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

void osmium_vector_layer_invalidate_columns(OsmiumVectorLayer *self) {
  invalidate_columns(self);
}

GPtrArray *osmium_vector_layer_column(OsmiumVectorLayer *self, const char *field) {
  return column_for(self, field);
}

static void invalidate_columns(OsmiumVectorLayer *self) {
  if (self->columns != NULL) {
    g_hash_table_remove_all(self->columns);
  }
}

static void free_value(gpointer data) {
  g_free(data);
}

/* One string per feature (in memory order) for the field, with unsaved edits applied. */
static GPtrArray *column_for(OsmiumVectorLayer *self, const char *field) {
  GPtrArray *column;

  if (self->data == NULL || field == NULL || field[0] == '\0') {
    return NULL;
  }

  column = g_hash_table_lookup(self->columns, field);
  if (column != NULL && column->len == self->data->n_features) {
    return column;
  }

  if (osmium_vector_layer_is_joined_field(self, field)) {
    column = osmium_join_column(self, field);
    if (column != NULL) {
      g_hash_table_insert(self->columns, g_strdup(field), column);
    }
    return column;
  }

  long long *fids = NULL;
  char **values = NULL;
  size_t n = 0;

  if (osmium_vector_data_read_column(self->data, field, &fids, &values, &n) != OSMIUM_OK) {
    return NULL;
  }

  GHashTable *by_fid = g_hash_table_new(g_int64_hash, g_int64_equal);

  for (size_t i = 0; i < n; i++) {
    g_hash_table_insert(by_fid, &fids[i], values[i]);
  }

  column = g_ptr_array_new_with_free_func(free_value);
  for (size_t i = 0; i < self->data->n_features; i++) {
    const long long fid = self->data->features[i].fid;
    const char *edited = self->session != NULL ? osmium_edit_get_override(self->session, fid, field) : NULL;
    const char *value = edited != NULL ? edited : g_hash_table_lookup(by_fid, &fid);

    g_ptr_array_add(column, g_strdup(value != NULL ? value : ""));
  }

  g_hash_table_destroy(by_fid);
  for (size_t i = 0; i < n; i++) {
    free(values[i]);
  }
  free(values);
  free(fids);

  g_hash_table_insert(self->columns, g_strdup(field), column);
  return column;
}

static gboolean parse_number(const char *text, double *value) {
  char *end = NULL;

  if (text == NULL || text[0] == '\0') {
    return FALSE;
  }
  *value = g_ascii_strtod(text, &end);
  return end != text && *end == '\0' && isfinite(*value);
}

static void assign_symbols(OsmiumVectorLayer *self, RenderJob *job) {
  const OsmiumStyle *style = self->style;
  GPtrArray *column = style->kind == OSMIUM_STYLE_SINGLE ? NULL : column_for(self, style->field);

  if (column == NULL) {
    job->n_symbols = 1;
    job->symbols = g_new(OsmiumSymbol, 1);
    job->symbols[0] = style->symbol;
    job->index = NULL;
  } else if (style->kind == OSMIUM_STYLE_CATEGORIZED) {
    GHashTable *lookup = g_hash_table_new(g_str_hash, g_str_equal);
    guint other = NO_SYMBOL;

    job->n_symbols = style->categories->len;
    job->symbols = g_new(OsmiumSymbol, MAX(job->n_symbols, 1));
    for (guint i = 0; i < style->categories->len; i++) {
      const OsmiumStyleCategory *c = &g_array_index(style->categories, OsmiumStyleCategory, i);

      job->symbols[i] = c->symbol;
      if (c->value == NULL) {
        other = c->visible ? i : NO_SYMBOL;
      } else if (c->visible) {
        g_hash_table_insert(lookup, c->value, GUINT_TO_POINTER(i + 1));
      } else {
        g_hash_table_insert(lookup, c->value, GUINT_TO_POINTER(NO_SYMBOL + 1));
      }
    }

    job->n_index = column->len;
    job->index = g_new(guint16, MAX(column->len, 1));
    for (guint i = 0; i < column->len; i++) {
      const guint hit = GPOINTER_TO_UINT(g_hash_table_lookup(lookup, g_ptr_array_index(column, i)));

      job->index[i] = hit == 0 ? other : (guint16)(hit - 1);
    }
    g_hash_table_destroy(lookup);
  } else {
    job->n_symbols = style->classes->len;
    job->symbols = g_new(OsmiumSymbol, MAX(job->n_symbols, 1));
    for (guint i = 0; i < style->classes->len; i++) {
      job->symbols[i] = g_array_index(style->classes, OsmiumStyleClass, i).symbol;
    }

    job->n_index = column->len;
    job->index = g_new(guint16, MAX(column->len, 1));
    for (guint i = 0; i < column->len; i++) {
      double value;
      guint16 symbol = NO_SYMBOL;

      if (parse_number(g_ptr_array_index(column, i), &value)) {
        for (guint c = 0; c < style->classes->len; c++) {
          const OsmiumStyleClass *cls = &g_array_index(style->classes, OsmiumStyleClass, c);

          if (value >= cls->lower && (value < cls->upper || (c == style->classes->len - 1 && value <= cls->upper))) {
            symbol = cls->visible ? (guint16)c : NO_SYMBOL;
            break;
          }
        }
      }
      job->index[i] = symbol;
    }
  }

  job->reach = 0.0;
  for (guint i = 0; i < job->n_symbols; i++) {
    job->reach = MAX(job->reach, job->symbols[i].line_width + job->symbols[i].point_radius * 1.3);
  }
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
  assign_symbols(self, job);

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

    append_selected_feature(target, f, scale, center_x, center_y, self->style->symbol.point_radius + 2.0);
  }

  GskPath *area_path = gsk_path_builder_free_to_path(areas);
  GskPath *line_path = gsk_path_builder_free_to_path(lines);
  GskPath *point_path = gsk_path_builder_free_to_path(points);
  GskStroke *stroke = gsk_stroke_new(MAX((float)self->style->symbol.line_width + 1.5f, 3.0f));

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

typedef struct {
  float x;
  float y;
  float w;
  float h;
} LabelBox;

static gboolean label_anchor(const OsmiumVectorFeature *f, double *x, double *y) {
  if (f->n_parts == 0) {
    return FALSE;
  }

  if (f->kind == OSMIUM_VECTOR_POINT) {
    *x = f->xy[f->part_start[0] * 2];
    *y = f->xy[f->part_start[0] * 2 + 1];
    return TRUE;
  }

  if (f->kind == OSMIUM_VECTOR_LINE) {
    size_t best = 0;
    size_t best_len = 0;

    for (size_t p = 0; p < f->n_parts; p++) {
      if (f->part_start[p + 1] - f->part_start[p] > best_len) {
        best_len = f->part_start[p + 1] - f->part_start[p];
        best = p;
      }
    }
    if (best_len == 0) {
      return FALSE;
    }
    const size_t mid = f->part_start[best] + best_len / 2;

    *x = f->xy[mid * 2];
    *y = f->xy[mid * 2 + 1];
    return TRUE;
  }

  *x = (f->min_x + f->max_x) / 2.0;
  *y = (f->min_y + f->max_y) / 2.0;
  return TRUE;
}

static void draw_labels(OsmiumVectorLayer *self,
                        GtkSnapshot *snapshot,
                        float width,
                        float height,
                        double center_x,
                        double center_y) {
  const OsmiumStyleLabels *labels = &self->style->labels;
  GPtrArray *column;

  if (!labels->enabled || self->data == NULL) {
    return;
  }

  column = column_for(self, labels->field);
  if (column == NULL || !osmium_vector_data_try_begin_read(self->data, osmium_map_get_projection(self->map))) {
    return;
  }

  if (self->label_layout == NULL) {
    self->label_layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), NULL);
  }

  PangoFontDescription *font = pango_font_description_new();

  if (labels->font_family != NULL && labels->font_family[0] != '\0') {
    pango_font_description_set_family(font, labels->font_family);
  }
  pango_font_description_set_absolute_size(font, labels->size * PANGO_SCALE);
  pango_layout_set_font_description(self->label_layout, font);
  pango_font_description_free(font);

  const double scale = 256.0 * ldexp(1.0, (int)osmium_map_get_zoom(self->map));
  const double rad = osmium_map_get_bearing(self->map) * G_PI / 180.0;
  const double c = cos(rad);
  const double s = sin(rad);
  GArray *placed = g_array_new(FALSE, FALSE, sizeof(LabelBox));
  guint scanned = 0;

  const OsmiumVectorFeature *features = self->data->features;

  for (size_t i = 0; i < self->data->n_features && placed->len < 400 && scanned < 6000 && i < column->len; i++) {
    const OsmiumVectorFeature *f = &features[i];
    const char *text = g_ptr_array_index(column, i);
    double ax, ay;

    if (f->deleted || text[0] == '\0' || !label_anchor(f, &ax, &ay)) {
      continue;
    }

    const double dx = ax * scale - center_x;
    const double dy = ay * scale - center_y;
    const float sx = (float)(width / 2.0 + dx * c + dy * s);
    const float sy = (float)(height / 2.0 - dx * s + dy * c);

    if (sx < -20 || sx > width + 20 || sy < -20 || sy > height + 20) {
      continue;
    }
    scanned++;

    int tw, th;

    pango_layout_set_text(self->label_layout, text, -1);
    pango_layout_get_pixel_size(self->label_layout, &tw, &th);

    LabelBox box = {
        .x = sx - tw / 2.0f + (f->kind == OSMIUM_VECTOR_POINT ? 0.0f : 0.0f),
        .y = sy - th / 2.0f - (f->kind == OSMIUM_VECTOR_POINT ? self->style->symbol.point_radius + th / 2.0f : 0.0f),
        .w = (float)tw,
        .h = (float)th,
    };
    gboolean clash = FALSE;

    for (guint k = 0; k < placed->len && !clash; k++) {
      const LabelBox *o = &g_array_index(placed, LabelBox, k);

      clash = box.x < o->x + o->w + 2 && box.x + box.w + 2 > o->x && box.y < o->y + o->h && box.y + box.h > o->y;
    }
    if (clash) {
      continue;
    }
    g_array_append_val(placed, box);

    if (labels->halo_width > 0.0 && labels->halo_color.alpha > 0.0) {
      const float r = (float)labels->halo_width;
      static const float offsets[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-0.7f, -0.7f}, {0.7f, -0.7f}, {-0.7f, 0.7f}, {0.7f, 0.7f}};

      for (guint k = 0; k < 8; k++) {
        gtk_snapshot_save(snapshot);
        gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(box.x + offsets[k][0] * r, box.y + offsets[k][1] * r));
        gtk_snapshot_append_layout(snapshot, self->label_layout, &labels->halo_color);
        gtk_snapshot_restore(snapshot);
      }
    }

    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(box.x, box.y));
    gtk_snapshot_append_layout(snapshot, self->label_layout, &labels->color);
    gtk_snapshot_restore(snapshot);
  }

  g_array_unref(placed);
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
  draw_labels(self, snapshot, width, height, center_x, center_y);
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

void osmium_vector_layer_emit_feature_added(OsmiumVectorLayer *self, gint64 fid) {
  g_signal_emit(self, signals[SIGNAL_FEATURE_ADDED], 0, fid);
}

void osmium_vector_layer_emit_editing_changed(OsmiumVectorLayer *self) {
  g_signal_emit(self, signals[SIGNAL_EDITING_CHANGED], 0);
}

void osmium_vector_layer_refresh(OsmiumVectorLayer *self) {
  invalidate_columns(self);
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

  for (guint i = 0; i < osmium_join_count_fields(self); i++) {
    OsmiumFeatureFieldType type = OSMIUM_FEATURE_FIELD_STRING;
    const char *name = osmium_join_field_name(self, i, &type);
    GPtrArray *column = column_for(self, name);
    OsmiumFeatureField *field = g_new0(OsmiumFeatureField, 1);

    field->name = g_strdup(name);
    field->value = g_strdup(column != NULL && (guint)index < column->len ? g_ptr_array_index(column, index) : "");
    field->type = type;
    field->virtual_field = TRUE;
    g_ptr_array_add(result, field);
  }
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

  for (guint i = 0; i < n_edits; i++) {
    if (osmium_vector_layer_is_joined_field(self, fields[i])) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_READ_ONLY, "“%s” is joined from another table and can't be edited", fields[i]);
      return FALSE;
    }
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
  g_signal_emit(self, signals[SIGNAL_SELECTION_CHANGED], 0);
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

  return &self->style->symbol.stroke;
}

void osmium_vector_layer_set_stroke_color(OsmiumVectorLayer *self, const GdkRGBA *color) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));
  g_return_if_fail(color != NULL);

  if (gdk_rgba_equal(&self->style->symbol.stroke, color)) {
    return;
  }

  self->style->symbol.stroke = *color;
  render_now(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STROKE_COLOR]);
}

const GdkRGBA *osmium_vector_layer_get_fill_color(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  return &self->style->symbol.fill;
}

void osmium_vector_layer_set_fill_color(OsmiumVectorLayer *self, const GdkRGBA *color) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));
  g_return_if_fail(color != NULL);

  if (gdk_rgba_equal(&self->style->symbol.fill, color)) {
    return;
  }

  self->style->symbol.fill = *color;
  render_now(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_FILL_COLOR]);
}

double osmium_vector_layer_get_line_width(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), 0.0);

  return self->style->symbol.line_width;
}

void osmium_vector_layer_set_line_width(OsmiumVectorLayer *self, double width) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));

  if (self->style->symbol.line_width == width) {
    return;
  }

  self->style->symbol.line_width = width;
  render_now(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LINE_WIDTH]);
}

double osmium_vector_layer_get_point_radius(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), 0.0);

  return self->style->symbol.point_radius;
}

void osmium_vector_layer_set_point_radius(OsmiumVectorLayer *self, double radius) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));

  if (self->style->symbol.point_radius == radius) {
    return;
  }

  self->style->symbol.point_radius = radius;
  render_now(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_POINT_RADIUS]);
}

OsmiumStyle *osmium_vector_layer_dup_style(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  return osmium_style_copy(self->style);
}

void osmium_vector_layer_set_style(OsmiumVectorLayer *self, const OsmiumStyle *style) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));
  g_return_if_fail(style != NULL);

  OsmiumStyle *copy = osmium_style_copy(style);

  osmium_style_free(self->style);
  self->style = copy;
  invalidate_columns(self);
  render_now(self);
  gtk_widget_queue_draw(GTK_WIDGET(self));
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STROKE_COLOR]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_FILL_COLOR]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LINE_WIDTH]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_POINT_RADIUS]);
}

typedef struct {
  char *value;
  guint count;
} ValueCount;

static int compare_counts(gconstpointer a, gconstpointer b) {
  const ValueCount *x = a;
  const ValueCount *y = b;

  if (x->count != y->count) {
    return x->count < y->count ? 1 : -1;
  }
  return g_strcmp0(x->value, y->value);
}

char **osmium_vector_layer_dup_unique_values(OsmiumVectorLayer *self, const char *field, guint limit, gboolean *truncated) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  GPtrArray *column = column_for(self, field);
  GHashTable *counts = g_hash_table_new(g_str_hash, g_str_equal);
  GArray *entries = g_array_new(FALSE, FALSE, sizeof(ValueCount));

  if (truncated != NULL) {
    *truncated = FALSE;
  }

  for (guint i = 0; column != NULL && i < column->len && i < self->data->n_features; i++) {
    const char *value = g_ptr_array_index(column, i);
    gpointer found = g_hash_table_lookup(counts, value);

    if (self->data->features[i].deleted) {
      continue;
    }
    if (found != NULL) {
      g_array_index(entries, ValueCount, GPOINTER_TO_UINT(found) - 1).count++;
    } else {
      ValueCount entry = {(char *)value, 1};

      g_array_append_val(entries, entry);
      g_hash_table_insert(counts, (gpointer)value, GUINT_TO_POINTER(entries->len));
    }
  }

  g_array_sort(entries, compare_counts);

  const guint n = limit > 0 ? MIN(limit, entries->len) : entries->len;
  char **result = g_new0(char *, n + 1);

  if (truncated != NULL) {
    *truncated = n < entries->len;
  }
  for (guint i = 0; i < n; i++) {
    result[i] = g_strdup(g_array_index(entries, ValueCount, i).value);
  }

  g_hash_table_destroy(counts);
  g_array_unref(entries);
  return result;
}

GArray *osmium_vector_layer_dup_numeric_values(OsmiumVectorLayer *self, const char *field) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  GPtrArray *column = column_for(self, field);
  GArray *values = g_array_new(FALSE, FALSE, sizeof(double));

  for (guint i = 0; column != NULL && i < column->len && i < self->data->n_features; i++) {
    double value;

    if (!self->data->features[i].deleted && parse_number(g_ptr_array_index(column, i), &value)) {
      g_array_append_val(values, value);
    }
  }
  return values;
}

void osmium_attribute_table_free(OsmiumAttributeTable *table) {
  if (table == NULL) {
    return;
  }
  g_strfreev(table->names);
  g_free(table->types);
  g_free(table->fids);
  for (gsize i = 0; i < (gsize)table->n_rows * table->n_fields; i++) {
    g_free(table->cells[i]);
  }
  g_free(table->cells);
  g_free(table);
}

OsmiumAttributeTable *osmium_vector_layer_dup_table(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  if (self->data == NULL) {
    return NULL;
  }

  g_autoptr(GPtrArray) fields = osmium_vector_layer_get_fields(self);
  long long *file_fids = NULL;
  char **file_cells = NULL;
  size_t file_rows = 0, file_columns = 0;

  if (fields == NULL) {
    return NULL;
  }
  if (osmium_vector_data_read_table(self->data, &file_fids, &file_cells, &file_rows, &file_columns) != OSMIUM_OK) {
    file_fids = NULL;
    file_cells = NULL;
    file_rows = file_columns = 0;
  }

  OsmiumAttributeTable *table = g_new0(OsmiumAttributeTable, 1);
  GHashTable *by_fid = g_hash_table_new(g_int64_hash, g_int64_equal);
  guint live = 0;

  const guint real_fields = fields->len;
  const guint joined_fields = osmium_join_count_fields(self);

  table->n_fields = real_fields + joined_fields;
  table->names = g_new0(char *, table->n_fields + 1);
  table->types = g_new0(OsmiumFeatureFieldType, MAX(table->n_fields, 1));
  for (guint i = 0; i < fields->len; i++) {
    const OsmiumFeatureField *field = g_ptr_array_index(fields, i);

    table->names[i] = g_strdup(field->name);
    table->types[i] = field->type;
  }
  for (guint j = 0; j < joined_fields; j++) {
    OsmiumFeatureFieldType type = OSMIUM_FEATURE_FIELD_STRING;

    table->names[real_fields + j] = g_strdup(osmium_join_field_name(self, j, &type));
    table->types[real_fields + j] = type;
  }

  for (size_t r = 0; r < file_rows; r++) {
    g_hash_table_insert(by_fid, &file_fids[r], GSIZE_TO_POINTER(r + 1));
  }

  for (size_t i = 0; i < self->data->n_features; i++) {
    live += self->data->features[i].deleted ? 0 : 1;
  }

  table->n_rows = live;
  table->fids = g_new(gint64, MAX(live, 1));
  table->cells = g_new0(char *, MAX((gsize)live * table->n_fields, 1));

  guint row = 0;

  for (size_t i = 0; i < self->data->n_features; i++) {
    const OsmiumVectorFeature *f = &self->data->features[i];
    const gsize source = GPOINTER_TO_SIZE(g_hash_table_lookup(by_fid, &f->fid));

    if (f->deleted) {
      continue;
    }
    table->fids[row] = f->fid;
    for (guint c = 0; c < real_fields; c++) {
      const char *edited = self->session != NULL ? osmium_edit_get_override(self->session, f->fid, table->names[c]) : NULL;
      const char *value = edited;

      if (value == NULL && source != 0 && c < file_columns) {
        value = file_cells[(source - 1) * file_columns + c];
      }
      table->cells[(gsize)row * table->n_fields + c] = g_strdup(value != NULL ? value : "");
    }
    for (guint j = 0; j < joined_fields; j++) {
      GPtrArray *column = column_for(self, table->names[real_fields + j]);

      table->cells[(gsize)row * table->n_fields + real_fields + j] =
          g_strdup(column != NULL && i < column->len ? g_ptr_array_index(column, i) : "");
    }
    row++;
  }

  g_hash_table_destroy(by_fid);
  for (size_t i = 0; i < file_rows * file_columns; i++) {
    free(file_cells[i]);
  }
  free(file_cells);
  free(file_fids);
  return table;
}

gboolean osmium_vector_layer_get_selection_extent(OsmiumVectorLayer *self,
                                                  double *lat_min,
                                                  double *lon_min,
                                                  double *lat_max,
                                                  double *lon_max) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  gboolean found = FALSE;
  double min_lat = 90, max_lat = -90, min_lon = 180, max_lon = -180;

  for (guint s = 0; self->data != NULL && s < self->selected_indices->len; s++) {
    const OsmiumVectorFeature *f = &self->data->features[g_array_index(self->selected_indices, size_t, s)];

    if (f->deleted) {
      continue;
    }
    const size_t n = f->part_start[f->n_parts];

    for (size_t v = 0; v < n; v++) {
      min_lon = MIN(min_lon, f->ll[v * 2]);
      max_lon = MAX(max_lon, f->ll[v * 2]);
      min_lat = MIN(min_lat, f->ll[v * 2 + 1]);
      max_lat = MAX(max_lat, f->ll[v * 2 + 1]);
      found = TRUE;
    }
  }

  if (found) {
    *lat_min = min_lat;
    *lat_max = max_lat;
    *lon_min = min_lon;
    *lon_max = max_lon;
  }
  return found;
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
  g_clear_pointer(&self->style, osmium_style_free);
  g_clear_pointer(&self->columns, g_hash_table_unref);
  g_clear_pointer(&self->join_configs, g_ptr_array_unref);
  g_clear_pointer(&self->join_states, g_ptr_array_unref);
  g_clear_object(&self->label_layout);
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
    g_value_set_boxed(value, &self->style->symbol.stroke);
    break;
  case PROP_FILL_COLOR:
    g_value_set_boxed(value, &self->style->symbol.fill);
    break;
  case PROP_LINE_WIDTH:
    g_value_set_double(value, self->style->symbol.line_width);
    break;
  case PROP_POINT_RADIUS:
    g_value_set_double(value, self->style->symbol.point_radius);
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
  signals[SIGNAL_FEATURE_ADDED] = g_signal_new("feature-added", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                               NULL, G_TYPE_NONE, 1, G_TYPE_INT64);
  signals[SIGNAL_SELECTION_CHANGED] = g_signal_new("selection-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                                   NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_LOAD_FAILED] = g_signal_new("load-failed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                             NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

  widget_class->snapshot = osmium_vector_layer_snapshot;
  widget_class->size_allocate = osmium_vector_layer_size_allocate;
  gtk_widget_class_set_css_name(widget_class, "osmium-vector-layer");
}

static void osmium_vector_layer_init(OsmiumVectorLayer *self) {
  self->style = osmium_style_new();
  self->columns = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);

  GtkWidget *widget = GTK_WIDGET(self);

  gdk_rgba_parse(&self->style->symbol.stroke, "#1c71d8");
  gdk_rgba_parse(&self->style->symbol.fill, "rgba(28, 113, 216, 0.35)");
  self->user_agent = g_strdup("Osmium/0.1");
  osmium_editor_init(self);
  self->selection = g_array_new(FALSE, FALSE, sizeof(gint64));
  self->selected_indices = g_array_new(FALSE, FALSE, sizeof(size_t));
  self->style->symbol.line_width = OSMIUM_VECTOR_LAYER_DEFAULT_LINE_WIDTH;
  self->style->symbol.point_radius = OSMIUM_VECTOR_LAYER_DEFAULT_POINT_RADIUS;
  self->cancellable = g_cancellable_new();

  gtk_widget_set_can_target(widget, FALSE);
  gtk_widget_set_can_focus(widget, FALSE);
  gtk_widget_set_overflow(widget, GTK_OVERFLOW_HIDDEN);
}

GtkWidget *osmium_vector_layer_new(OsmiumMap *map) {
  return g_object_new(OSMIUM_TYPE_VECTOR_LAYER, "map", map, NULL);
}
