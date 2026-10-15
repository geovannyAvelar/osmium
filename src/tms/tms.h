/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdint.h>
#include <curl/curl.h>

#include "../osmium.h"

#define osmium_is_error(err) ((err) <= 0)

typedef struct {
  uint32_t x;
  uint32_t y;
  uint32_t z;
  char *url;
} osmium_tile;

typedef struct {
  double lat_min;
  double lon_min;
  double lat_max;
  double lon_max;
} osmium_bbox;

void osmium_world_pixel(double lat, double lon, uint32_t zoom, double *px, double *py);
void osmium_world_pixel_to_latlon(double px, double py, uint32_t zoom, double *lat, double *lon);

int osmium_bbox_for_view(
    double lat, double lon, uint32_t zoom, double width, double height, osmium_bbox *out);
size_t
tile_count_for_bbox(double lat_min, double lon_min, double lat_max, double lon_max, uint32_t z);
size_t tiles_for_bbox(double lat_min,
                      double lon_min,
                      double lat_max,
                      double lon_max,
                      uint32_t z,
                      osmium_tile **out_tiles);

CURLcode download_tile(const osmium_tile *tile,
                       const char *url_template,
                       const char *path,
                       const char *user_agent);
int osmium_ensure_dir(const char *path);

char *osmium_tile_cache_path(const char *cache_folder, const osmium_tile *tile);

osmium_error get_tiles(const char *url_template,
                                       const char *cache_folder,
                                       double lat_min,
                                       double lon_min,
                                       double lat_max,
                                       double lon_max,
                                       uint32_t zoom,
                                       osmium_tile **tiles);
