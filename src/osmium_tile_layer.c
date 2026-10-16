/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_tile_layer.h"

#include "tms/tms.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#define OSMIUM_TILE_LAYER_DOWNLOAD_THREADS 2
#define OSMIUM_TILE_LAYER_DEFAULT_TEMPLATE_URL "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
#define OSMIUM_TILE_LAYER_DEFAULT_USER_AGENT "Osmium/0.1"
#define OSMIUM_TILE_LAYER_KEEP_ZOOM_LEVELS 2
#define OSMIUM_TILE_LAYER_IDLE_KEEP_LEVELS 1
#define OSMIUM_TILE_LAYER_MAX_TILES 512

typedef struct {
  GdkTexture *texture;
  uint32_t x;
  uint32_t y;
  uint32_t z;
} OsmiumLayerTile;

typedef struct {
  osmium_tile tile;
  char *url_template;
  char *user_agent;
  char *path;
  gboolean prefetch;
} TileJob;

struct _OsmiumTileLayer {
  GtkWidget parent_instance;

  OsmiumMap *map; /* weak */
  gulong view_handler;
  gulong crs_handler;

  char *template_url;
  char *user_agent;
  char *cache_folder;

  guint loaded_zoom;
  guint pending;
  GHashTable *requested;
  GPtrArray *tiles;
  GThreadPool *pool;
  GCancellable *cancellable;
};

static void osmium_tile_layer_prefetch(OsmiumTileLayer *self);

OSMIUM_API GType osmium_tile_layer_get_type(void);

G_DEFINE_TYPE(OsmiumTileLayer, osmium_tile_layer, GTK_TYPE_WIDGET)

enum { PROP_0, PROP_MAP, PROP_TEMPLATE_URL, PROP_USER_AGENT, PROP_CACHE_FOLDER, PROP_COMPATIBLE, N_PROPS };

static GParamSpec *properties[N_PROPS];

static void tile_job_free(gpointer data) {
  TileJob *job = data;

  g_free(job->url_template);
  g_free(job->user_agent);
  g_free(job->path);
  g_free(job);
}

static gboolean file_is_cached(const char *path) {
  GStatBuf st;

  return g_stat(path, &st) == 0 && st.st_size > 0;
}

static void tile_worker(gpointer data, gpointer user_data) {
  GTask *task = data;
  TileJob *job = g_task_get_task_data(task);
  GError *error = NULL;

  if (g_cancellable_set_error_if_cancelled(g_task_get_cancellable(task), &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }

  gboolean was_cached = file_is_cached(job->path);

  for (;;) {
    if (!was_cached) {
      CURLcode result = download_tile(&job->tile, job->url_template, job->path, job->user_agent);

      if (result != CURLE_OK) {
        g_task_return_new_error(
            task, G_IO_ERROR, G_IO_ERROR_FAILED, "download failed: %s", curl_easy_strerror(result));
        g_object_unref(task);
        return;
      }
    }

    GdkTexture *texture = gdk_texture_new_from_filename(job->path, &error);

    if (texture != NULL) {
      g_task_return_pointer(task, texture, g_object_unref);
      break;
    }

    /* corrupt cache entry (e.g. truncated by an earlier crash): refetch once */
    if (!was_cached) {
      g_task_return_error(task, error);
      break;
    }

    g_clear_error(&error);
    g_remove(job->path);
    was_cached = FALSE;
  }

  g_object_unref(task);
}

static void osmium_layer_tile_free(gpointer data) {
  OsmiumLayerTile *tile = data;

  g_clear_object(&tile->texture);
  g_free(tile);
}

static void osmium_tile_layer_forget(OsmiumTileLayer *self, const OsmiumLayerTile *tile) {
  char *key = g_strdup_printf("%u/%u/%u", tile->z, tile->x, tile->y);

  g_hash_table_remove(self->requested, key);
  g_free(key);
}

static void osmium_tile_layer_evict(OsmiumTileLayer *self, int max_distance) {
  const int zoom = (int)osmium_map_get_zoom(self->map);

  for (guint i = self->tiles->len; i > 0; i--) {
    OsmiumLayerTile *tile = g_ptr_array_index(self->tiles, i - 1);

    if (abs((int)tile->z - zoom) > max_distance) {
      osmium_tile_layer_forget(self, tile);
      g_ptr_array_remove_index_fast(self->tiles, i - 1);
    }
  }
}

static void osmium_tile_layer_trim(OsmiumTileLayer *self) {
  const int keep = self->tiles->len > OSMIUM_TILE_LAYER_MAX_TILES ? 0 : OSMIUM_TILE_LAYER_IDLE_KEEP_LEVELS;

  osmium_tile_layer_evict(self, keep);
}

static void on_tile_ready(GObject *source, GAsyncResult *result, gpointer user_data) {
  OsmiumTileLayer *self = OSMIUM_TILE_LAYER(source);
  TileJob *job = g_task_get_task_data(G_TASK(result));
  g_autoptr(GError) error = NULL;

  GdkTexture *texture = g_task_propagate_pointer(G_TASK(result), &error);

  if (!job->prefetch && self->pending > 0) {
    self->pending--;
  }

  if (texture == NULL) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("Tile %u/%u/%u: %s", job->tile.z, job->tile.x, job->tile.y, error->message);
    }
    return;
  }

  if (self->tiles == NULL || self->map == NULL) {
    g_object_unref(texture);
    return;
  }

  OsmiumLayerTile *tile = g_new0(OsmiumLayerTile, 1);
  tile->texture = texture;
  tile->x = job->tile.x;
  tile->y = job->tile.y;
  tile->z = job->tile.z;

  if (abs((int)tile->z - (int)osmium_map_get_zoom(self->map)) > OSMIUM_TILE_LAYER_KEEP_ZOOM_LEVELS) {
    osmium_tile_layer_forget(self, tile);
    osmium_layer_tile_free(tile);
  } else {
    g_ptr_array_add(self->tiles, tile);
  }

  if (self->pending == 0) {
    osmium_tile_layer_trim(self);

    if (!job->prefetch) {
      osmium_tile_layer_prefetch(self);
    }
  }

  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void osmium_tile_layer_clear_tiles(OsmiumTileLayer *self) {
  g_ptr_array_set_size(self->tiles, 0);
  g_hash_table_remove_all(self->requested);
}

static void osmium_tile_layer_request(OsmiumTileLayer *self, const osmium_tile *tile, gboolean prefetch) {
  char *key = g_strdup_printf("%u/%u/%u", tile->z, tile->x, tile->y);
  if (!g_hash_table_add(self->requested, key)) {
    return;
  }

  char *path = osmium_tile_cache_path(self->cache_folder, tile);
  if (path == NULL) {
    return;
  }

  TileJob *job = g_new0(TileJob, 1);
  job->tile = *tile;
  job->prefetch = prefetch;
  job->url_template = g_strdup(self->template_url);
  job->user_agent = g_strdup(self->user_agent);
  job->path = g_strdup(path);
  free(path);

  GTask *task = g_task_new(self, self->cancellable, on_tile_ready, NULL);
  g_task_set_task_data(task, job, tile_job_free);

  g_autoptr(GError) error = NULL;
  if (!g_thread_pool_push(self->pool, task, &error)) {
    g_warning("Could not queue tile: %s", error->message);
    g_object_unref(task);
  } else if (!prefetch) {
    self->pending++;
  }
}

static size_t osmium_tile_layer_view_tiles(OsmiumTileLayer *self, guint zoom, osmium_tile **tiles) {
  GtkWidget *widget = GTK_WIDGET(self);
  osmium_bbox bbox;

  const int width = gtk_widget_get_width(widget);
  const int height = gtk_widget_get_height(widget);

  if (self->map == NULL || self->template_url == NULL || self->cache_folder == NULL ||
      width <= 0 || height <= 0) {
    return 0;
  }

  const double lat = osmium_map_get_lat(self->map);
  const double lon = osmium_map_get_lon(self->map);
  const double bearing = osmium_map_get_bearing(self->map);

  /* the rotated viewport needs the tiles under its axis-aligned bounding box */
  const double rad = bearing * G_PI / 180.0;
  const double view_width = width * fabs(cos(rad)) + height * fabs(sin(rad));
  const double view_height = width * fabs(sin(rad)) + height * fabs(cos(rad));

  if (!osmium_bbox_for_view(lat, lon, zoom, view_width, view_height, &bbox)) {
    g_warning("Invalid view: lat=%f lon=%f zoom=%u %dx%d", lat, lon, zoom, width, height);
    return 0;
  }

  *tiles = NULL;
  return tiles_for_bbox(bbox.lat_min, bbox.lon_min, bbox.lat_max, bbox.lon_max, zoom, tiles);
}

static void osmium_tile_layer_prefetch(OsmiumTileLayer *self) {
  if (self->map == NULL) {
    return;
  }

  const guint zoom = osmium_map_get_zoom(self->map);
  osmium_tile *tiles = NULL;

  if (zoom == 0) {
    return;
  }

  size_t count = osmium_tile_layer_view_tiles(self, zoom - 1, &tiles);

  for (size_t i = 0; i < count; i++) {
    osmium_tile_layer_request(self, &tiles[i], TRUE);
  }

  free(tiles);
}

static void osmium_tile_layer_load_tiles(OsmiumTileLayer *self) {
  if (self->map != NULL && !osmium_map_crs_is_web_mercator(self->map)) {
    return;
  }

  const guint zoom = self->map != NULL ? osmium_map_get_zoom(self->map) : 0;
  osmium_tile *tiles = NULL;

  size_t count = osmium_tile_layer_view_tiles(self, zoom, &tiles);
  if (count == 0) {
    return;
  }

  if (osmium_ensure_dir(self->cache_folder) != 0) {
    g_warning("Could not create cache folder %s", self->cache_folder);
    free(tiles);
    return;
  }

  for (size_t i = 0; i < count; i++) {
    if (tiles[i].z == 0) {
      continue;
    }

    char *key = g_strdup_printf("%u/%u/%u", tiles[i].z, tiles[i].x, tiles[i].y);
    const gboolean missing = !g_hash_table_contains(self->requested, key);
    g_free(key);

    if (missing) {
      osmium_tile parent = {.x = tiles[i].x / 2, .y = tiles[i].y / 2, .z = tiles[i].z - 1};
      osmium_tile_layer_request(self, &parent, FALSE);
    }
  }

  for (size_t i = 0; i < count; i++) {
    osmium_tile_layer_request(self, &tiles[i], FALSE);
  }

  free(tiles);
}

static void osmium_tile_layer_reload(OsmiumTileLayer *self);

static void on_map_crs_changed(GObject *map, GParamSpec *pspec, gpointer user_data) {
  OsmiumTileLayer *self = OSMIUM_TILE_LAYER(user_data);

  osmium_tile_layer_reload(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_COMPATIBLE]);
}

static void on_map_view_changed(OsmiumMap *map, gpointer user_data) {
  OsmiumTileLayer *self = OSMIUM_TILE_LAYER(user_data);
  const guint zoom = osmium_map_get_zoom(map);

  if (zoom != self->loaded_zoom) {
    self->loaded_zoom = zoom;
    osmium_tile_layer_evict(self, OSMIUM_TILE_LAYER_KEEP_ZOOM_LEVELS);
  }

  osmium_tile_layer_load_tiles(self);

  if (self->pending == 0) {
    osmium_tile_layer_trim(self);
    osmium_tile_layer_prefetch(self);
  }

  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void osmium_tile_layer_reload(OsmiumTileLayer *self) {
  osmium_tile_layer_clear_tiles(self);

  if (self->map != NULL) {
    self->loaded_zoom = osmium_map_get_zoom(self->map);
  }

  osmium_tile_layer_load_tiles(self);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static gint compare_job_priority(gconstpointer a, gconstpointer b, gpointer user_data) {
  const TileJob *job_a = g_task_get_task_data((GTask *)a);
  const TileJob *job_b = g_task_get_task_data((GTask *)b);

  return (gint)job_a->prefetch - (gint)job_b->prefetch;
}

static void osmium_tile_layer_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  OsmiumTileLayer *self = OSMIUM_TILE_LAYER(widget);

  if (self->map == NULL || !osmium_map_crs_is_web_mercator(self->map)) {
    return;
  }

  const float width = gtk_widget_get_width(widget);
  const float height = gtk_widget_get_height(widget);

  const double bearing = osmium_map_get_bearing(self->map);
  const guint zoom = osmium_map_get_zoom(self->map);

  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
  gtk_snapshot_push_clip(snapshot, &bounds);

  double center_x, center_y;
  osmium_world_pixel(
      osmium_map_get_lat(self->map), osmium_map_get_lon(self->map), zoom, &center_x, &center_y);

  /* world pixel under the widget center, snapped so tile edges stay on whole pixels */
  const double origin_x = round(center_x - width / 2.0) + width / 2.0;
  const double origin_y = round(center_y - height / 2.0) + height / 2.0;

  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(width / 2.0f, height / 2.0f));
  gtk_snapshot_rotate(snapshot, -(float)bearing);

  /* other zoom levels are only placeholders while tiles load; they stay cached for the next zoom */
  const int max_distance = self->pending > 0 ? OSMIUM_TILE_LAYER_KEEP_ZOOM_LEVELS : 0;

  for (int distance = max_distance; distance >= 0; distance--) {
    for (guint i = 0; i < self->tiles->len; i++) {
      OsmiumLayerTile *tile = g_ptr_array_index(self->tiles, i);

      if (tile->texture == NULL || abs((int)tile->z - (int)zoom) != distance) {
        continue;
      }

      const double scale = ldexp(1.0, (int)zoom - (int)tile->z);
      const double size = 256.0 * scale;
      /* rotated or scaled tiles need a 1px overlap, or anti-aliasing leaves seams */
      const double draw_size = (bearing == 0.0 && scale == 1.0) ? size : size + 1.0;

      graphene_rect_t rect = GRAPHENE_RECT_INIT(
          (double)tile->x * size - origin_x, (double)tile->y * size - origin_y, draw_size, draw_size);

      gtk_snapshot_append_texture(snapshot, tile->texture, &rect);
    }
  }

  gtk_snapshot_restore(snapshot);
  gtk_snapshot_pop(snapshot);
}

static void osmium_tile_layer_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
  osmium_tile_layer_load_tiles(OSMIUM_TILE_LAYER(widget));
}

OsmiumMap *osmium_tile_layer_get_map(OsmiumTileLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_TILE_LAYER(self), NULL);

  return self->map;
}

void osmium_tile_layer_set_map(OsmiumTileLayer *self, OsmiumMap *map) {
  g_return_if_fail(OSMIUM_IS_TILE_LAYER(self));
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

  osmium_tile_layer_reload(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MAP]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_COMPATIBLE]);
}

const char *osmium_tile_layer_get_template_url(OsmiumTileLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_TILE_LAYER(self), NULL);

  return self->template_url;
}

void osmium_tile_layer_set_template_url(OsmiumTileLayer *self, const char *template_url) {
  g_return_if_fail(OSMIUM_IS_TILE_LAYER(self));
  g_return_if_fail(template_url != NULL);

  if (g_strcmp0(self->template_url, template_url) == 0) {
    return;
  }

  g_free(self->template_url);
  self->template_url = g_strdup(template_url);

  osmium_tile_layer_reload(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TEMPLATE_URL]);
}

const char *osmium_tile_layer_get_user_agent(OsmiumTileLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_TILE_LAYER(self), NULL);

  return self->user_agent;
}

void osmium_tile_layer_set_user_agent(OsmiumTileLayer *self, const char *user_agent) {
  g_return_if_fail(OSMIUM_IS_TILE_LAYER(self));
  g_return_if_fail(user_agent != NULL);

  if (g_strcmp0(self->user_agent, user_agent) == 0) {
    return;
  }

  g_free(self->user_agent);
  self->user_agent = g_strdup(user_agent);

  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_USER_AGENT]);
}

const char *osmium_tile_layer_get_cache_folder(OsmiumTileLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_TILE_LAYER(self), NULL);

  return self->cache_folder;
}

void osmium_tile_layer_set_cache_folder(OsmiumTileLayer *self, const char *cache_folder) {
  g_return_if_fail(OSMIUM_IS_TILE_LAYER(self));
  g_return_if_fail(cache_folder != NULL);

  if (g_strcmp0(self->cache_folder, cache_folder) == 0) {
    return;
  }

  g_free(self->cache_folder);
  self->cache_folder = g_strdup(cache_folder);

  osmium_tile_layer_reload(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_CACHE_FOLDER]);
}

static void osmium_tile_layer_dispose(GObject *object) {
  OsmiumTileLayer *self = OSMIUM_TILE_LAYER(object);

  if (self->cancellable != NULL) {
    g_cancellable_cancel(self->cancellable);
  }

  if (self->pool != NULL) {
    g_thread_pool_free(self->pool, FALSE, FALSE);
    self->pool = NULL;
  }

  if (self->map != NULL) {
    g_signal_handler_disconnect(self->map, self->view_handler);
    g_signal_handler_disconnect(self->map, self->crs_handler);
    self->view_handler = 0;
    self->crs_handler = 0;
    g_clear_weak_pointer(&self->map);
  }

  g_clear_object(&self->cancellable);
  g_clear_pointer(&self->requested, g_hash_table_unref);
  g_clear_pointer(&self->tiles, g_ptr_array_unref);
  g_clear_pointer(&self->template_url, g_free);
  g_clear_pointer(&self->user_agent, g_free);
  g_clear_pointer(&self->cache_folder, g_free);

  G_OBJECT_CLASS(osmium_tile_layer_parent_class)->dispose(object);
}

static void osmium_tile_layer_set_property(GObject *object,
                                           guint prop_id,
                                           const GValue *value,
                                           GParamSpec *pspec) {
  OsmiumTileLayer *self = OSMIUM_TILE_LAYER(object);

  switch (prop_id) {
  case PROP_MAP:
    osmium_tile_layer_set_map(self, g_value_get_object(value));
    break;
  case PROP_TEMPLATE_URL:
    osmium_tile_layer_set_template_url(self, g_value_get_string(value));
    break;
  case PROP_USER_AGENT:
    osmium_tile_layer_set_user_agent(self, g_value_get_string(value));
    break;
  case PROP_CACHE_FOLDER:
    osmium_tile_layer_set_cache_folder(self, g_value_get_string(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void osmium_tile_layer_get_property(GObject *object,
                                           guint prop_id,
                                           GValue *value,
                                           GParamSpec *pspec) {
  OsmiumTileLayer *self = OSMIUM_TILE_LAYER(object);

  switch (prop_id) {
  case PROP_MAP:
    g_value_set_object(value, self->map);
    break;
  case PROP_TEMPLATE_URL:
    g_value_set_string(value, self->template_url);
    break;
  case PROP_USER_AGENT:
    g_value_set_string(value, self->user_agent);
    break;
  case PROP_CACHE_FOLDER:
    g_value_set_string(value, self->cache_folder);
    break;
  case PROP_COMPATIBLE:
    g_value_set_boolean(value, self->map == NULL || osmium_map_crs_is_web_mercator(self->map));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void osmium_tile_layer_class_init(OsmiumTileLayerClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  curl_global_init(CURL_GLOBAL_DEFAULT);

  object_class->dispose = osmium_tile_layer_dispose;
  object_class->set_property = osmium_tile_layer_set_property;
  object_class->get_property = osmium_tile_layer_get_property;

  properties[PROP_MAP] = g_param_spec_object(
      "map", NULL, "The map this layer draws on", OSMIUM_TYPE_MAP,
      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  properties[PROP_TEMPLATE_URL] = g_param_spec_string(
      "template-url", NULL, "Tile URL template with {z}, {x} and {y}",
      OSMIUM_TILE_LAYER_DEFAULT_TEMPLATE_URL,
      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  properties[PROP_USER_AGENT] = g_param_spec_string(
      "user-agent", NULL, "User-Agent sent to the tile server (OSM policy: identify your app)",
      OSMIUM_TILE_LAYER_DEFAULT_USER_AGENT,
      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  properties[PROP_CACHE_FOLDER] = g_param_spec_string(
      "cache-folder", NULL, "Folder where downloaded tiles are cached", NULL,
      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_COMPATIBLE] = g_param_spec_boolean(
      "compatible", NULL, "Whether the map coordinate system can show tiles (Web Mercator only)", TRUE,
      G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPS, properties);

  widget_class->snapshot = osmium_tile_layer_snapshot;
  widget_class->size_allocate = osmium_tile_layer_size_allocate;
  gtk_widget_class_set_css_name(widget_class, "osmium-tile-layer");
}

static void osmium_tile_layer_init(OsmiumTileLayer *self) {
  GtkWidget *widget = GTK_WIDGET(self);

  self->template_url = g_strdup(OSMIUM_TILE_LAYER_DEFAULT_TEMPLATE_URL);
  self->user_agent = g_strdup(OSMIUM_TILE_LAYER_DEFAULT_USER_AGENT);
  self->cache_folder = g_build_filename(g_get_user_cache_dir(), "osmium", "tiles", NULL);

  self->tiles = g_ptr_array_new_with_free_func(osmium_layer_tile_free);
  self->requested = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->cancellable = g_cancellable_new();
  self->pool =
      g_thread_pool_new(tile_worker, NULL, OSMIUM_TILE_LAYER_DOWNLOAD_THREADS, FALSE, NULL);
  g_thread_pool_set_sort_function(self->pool, compare_job_priority, NULL);

  gtk_widget_set_can_target(widget, FALSE);
  gtk_widget_set_can_focus(widget, FALSE);
  gtk_widget_set_overflow(widget, GTK_OVERFLOW_HIDDEN);
}

GtkWidget *osmium_tile_layer_new(OsmiumMap *map) {
  return g_object_new(OSMIUM_TYPE_TILE_LAYER, "map", map, NULL);
}
