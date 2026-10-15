#include "tms.h"

#include <errno.h>
#include <unistd.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_MERCATOR_LAT 85.0511287798066

static uint32_t tile_x(double lon, uint32_t z) {
  double n = ldexp(1.0, (int)z);
  double x = floor((lon + 180.0) / 360.0 * n);

  if (x < 0.0)
    return 0;
  if (x >= n)
    return (uint32_t)n - 1;

  return (uint32_t)x;
}

static uint32_t tile_y(double lat, uint32_t z) {
  if (lat > MAX_MERCATOR_LAT)
    lat = MAX_MERCATOR_LAT;
  if (lat < -MAX_MERCATOR_LAT)
    lat = -MAX_MERCATOR_LAT;

  double lat_rad = lat * M_PI / 180.0;
  double n = ldexp(1.0, (int)z);

  double y = floor((1.0 - asinh(tan(lat_rad)) / M_PI) / 2.0 * n);

  if (y < 0.0)
    return 0;
  if (y >= n)
    return (uint32_t)n - 1;

  return (uint32_t)y;
}

#define OSMIUM_TILE_SIZE 256.0

static double clamp_double(double v, double lo, double hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

void osmium_world_pixel(double lat, double lon, uint32_t zoom, double *px, double *py) {
  double n = OSMIUM_TILE_SIZE * ldexp(1.0, (int)zoom);

  lat = clamp_double(lat, -MAX_MERCATOR_LAT, MAX_MERCATOR_LAT);

  *px = (lon + 180.0) / 360.0 * n;
  *py = (1.0 - asinh(tan(lat * M_PI / 180.0)) / M_PI) / 2.0 * n;
}

void osmium_world_pixel_to_latlon(double px, double py, uint32_t zoom, double *lat, double *lon) {
  double n = OSMIUM_TILE_SIZE * ldexp(1.0, (int)zoom);

  *lon = px / n * 360.0 - 180.0;
  *lat = atan(sinh(M_PI * (1.0 - 2.0 * py / n))) * 180.0 / M_PI;
}

int osmium_bbox_for_view(
    double lat, double lon, uint32_t zoom, double width, double height, osmium_bbox *out) {
  if (out == NULL || zoom > 22 || width <= 0.0 || height <= 0.0 || lat < -90.0 || lat > 90.0 ||
      lon < -180.0 || lon > 180.0) {
    return 0;
  }

  double n = OSMIUM_TILE_SIZE * ldexp(1.0, (int)zoom);
  double cx, cy;
  osmium_world_pixel(lat, lon, zoom, &cx, &cy);

  double left = clamp_double(cx - width / 2.0, 0.0, n);
  double right = clamp_double(cx + width / 2.0, 0.0, n);
  double top = clamp_double(cy - height / 2.0, 0.0, n);
  double bottom = clamp_double(cy + height / 2.0, 0.0, n);

  double lat_max, lat_min, lon_min, lon_max;
  osmium_world_pixel_to_latlon(left, top, zoom, &lat_max, &lon_min);
  osmium_world_pixel_to_latlon(right, bottom, zoom, &lat_min, &lon_max);

  out->lat_min = lat_min;
  out->lon_min = clamp_double(lon_min, -180.0, 180.0);
  out->lat_max = lat_max;
  out->lon_max = clamp_double(lon_max, -180.0, 180.0);

  return 1;
}

static int bbox_tile_range(double lat_min,
                           double lon_min,
                           double lat_max,
                           double lon_max,
                           uint32_t z,
                           uint32_t *x_min,
                           uint32_t *x_max,
                           uint32_t *y_min,
                           uint32_t *y_max) {
  if (z > 22 || lat_min > lat_max || lat_min < -90.0 || lat_max > 90.0 || lon_min < -180.0 ||
      lon_max > 180.0 || lon_min > lon_max) {
    return 0;
  }

  *x_min = tile_x(lon_min, z);
  *x_max = tile_x(lon_max, z);
  *y_min = tile_y(lat_max, z);
  *y_max = tile_y(lat_min, z);

  return 1;
}

size_t
tile_count_for_bbox(double lat_min, double lon_min, double lat_max, double lon_max, uint32_t z) {
  uint32_t x_min, x_max, y_min, y_max;

  if (!bbox_tile_range(lat_min, lon_min, lat_max, lon_max, z, &x_min, &x_max, &y_min, &y_max)) {
    return 0;
  }

  size_t cols = (size_t)x_max - x_min + 1;
  size_t rows = (size_t)y_max - y_min + 1;

  if (rows > SIZE_MAX / cols) {
    return 0;
  }

  size_t count = rows * cols;

  if (count > SIZE_MAX / sizeof(osmium_tile)) {
    return 0;
  }

  return count;
}

size_t tiles_for_bbox(double lat_min,
                      double lon_min,
                      double lat_max,
                      double lon_max,
                      uint32_t z,
                      osmium_tile **out_tiles) {
  if (!out_tiles) {
    return 0;
  }

  *out_tiles = NULL;

  size_t count = tile_count_for_bbox(lat_min, lon_min, lat_max, lon_max, z);
  if (count == 0) {
    return 0;
  }

  uint32_t x_min, x_max, y_min, y_max;
  bbox_tile_range(lat_min, lon_min, lat_max, lon_max, z, &x_min, &x_max, &y_min, &y_max);

  osmium_tile *tiles = malloc(count * sizeof(osmium_tile));
  if (!tiles) {
    return 0;
  }

  size_t i = 0;

  for (uint32_t x = x_min; x <= x_max; x++) {
    for (uint32_t y = y_min; y <= y_max; y++) {
      tiles[i++] = (osmium_tile){.x = x, .y = y, .z = z};
    }
  }

  *out_tiles = tiles;
  return count;
}

static char *expand_tile_url(const char *template, uint32_t x, uint32_t y, uint32_t z) {
  if (template == NULL) {
    return NULL;
  }

  char char_z[16], char_x[16], char_y[16];

  snprintf(char_z, sizeof(char_z), "%u", z);
  snprintf(char_x, sizeof(char_x), "%u", x);
  snprintf(char_y, sizeof(char_y), "%u", y);

  /* each 3-char placeholder expands to at most 10 digits */
  size_t capacity = strlen(template);

  if (capacity > (SIZE_MAX - 1) / 4)
    return NULL;

  capacity = capacity * 4 + 1;

  char *url = malloc(capacity);
  if (url == NULL) {
    return NULL;
  }

  const char *p = template;
  char *out = url;

  while (*p != '\0') {
    const char *replacement = NULL;

    if (strncmp(p, "{z}", 3) == 0) {
      replacement = char_z;
      p += 3;
    } else if (strncmp(p, "{x}", 3) == 0) {
      replacement = char_x;
      p += 3;
    } else if (strncmp(p, "{y}", 3) == 0) {
      replacement = char_y;
      p += 3;
    } else {
      *out++ = *p++;
      continue;
    }

    size_t len = strlen(replacement);
    memcpy(out, replacement, len);
    out += len;
  }

  *out = '\0';
  return url;
}

CURLcode download_tile(const osmium_tile *tile,
                       const char *url_template,
                       const char *path,
                       const char *user_agent) {
  if (!tile || !path) {
    return CURLE_BAD_FUNCTION_ARGUMENT;
  }

  char *url = expand_tile_url(url_template, tile->x, tile->y, tile->z);

  if (url == NULL) {
    return CURLE_URL_MALFORMAT;
  }

  char *dir = strdup(path);
  if (dir == NULL) {
    free(url);
    return CURLE_OUT_OF_MEMORY;
  }

  char *slash = strrchr(dir, '/');
  if (slash != NULL && slash != dir) {
    *slash = '\0';
    if (osmium_ensure_dir(dir) != 0) {
      free(dir);
      free(url);
      return CURLE_WRITE_ERROR;
    }
  }
  free(dir);

  size_t tmp_len = strlen(path) + sizeof(".XXXXXX");
  char *tmp_path = malloc(tmp_len);
  if (tmp_path == NULL) {
    free(url);
    return CURLE_OUT_OF_MEMORY;
  }
  snprintf(tmp_path, tmp_len, "%s.XXXXXX", path);

  int fd = mkstemp(tmp_path);
  FILE *file = fd >= 0 ? fdopen(fd, "wb") : NULL;
  if (!file) {
    if (fd >= 0) {
      close(fd);
      remove(tmp_path);
    }
    free(tmp_path);
    free(url);
    return CURLE_WRITE_ERROR;
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    free(url);
    fclose(file);
    remove(tmp_path);
    free(tmp_path);
    return CURLE_FAILED_INIT;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent ? user_agent : "Osmium/0.1");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

  CURLcode result = curl_easy_perform(curl);

  long status = 0;
  if (result == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) {
      result = CURLE_HTTP_RETURNED_ERROR;
    }
  }

  curl_easy_cleanup(curl);
  free(url);

  if (fclose(file) != 0 && result == CURLE_OK) {
    result = CURLE_WRITE_ERROR;
  }

  if (result == CURLE_OK && rename(tmp_path, path) != 0) {
    result = CURLE_WRITE_ERROR;
  }

  if (result != CURLE_OK) {
    remove(tmp_path);
  }

  free(tmp_path);
  return result;
}

int osmium_ensure_dir(const char *path) {
  size_t len = strlen(path);
  if (len == 0) {
    return -1;
  }

  char *buf = malloc(len + 1);
  if (buf == NULL) {
    return -1;
  }
  memcpy(buf, path, len + 1);

  while (len > 1 && buf[len - 1] == '/') {
    buf[--len] = '\0';
  }

  for (char *p = buf + 1; *p != '\0'; p++) {
    if (*p != '/') {
      continue;
    }

    *p = '\0';
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
      free(buf);
      return -1;
    }
    *p = '/';
  }

  int result = 0;
  if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
    result = -1;
  } else {
    struct stat st;
    if (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) {
      result = -1;
    }
  }

  free(buf);
  return result;
}

char *osmium_tile_cache_path(const char *cache_folder, const osmium_tile *tile) {
  int len = snprintf(NULL, 0, "%s/%u/%u/%u.png", cache_folder, tile->z, tile->x, tile->y);
  if (len < 0) {
    return NULL;
  }

  char *path = malloc((size_t)len + 1);
  if (path == NULL) {
    return NULL;
  }

  snprintf(path, (size_t)len + 1, "%s/%u/%u/%u.png", cache_folder, tile->z, tile->x, tile->y);
  return path;
}

osmium_error get_tiles(const char *url_template,
                                       const char *cache_folder,
                                       double lat_min,
                                       double lon_min,
                                       double lat_max,
                                       double lon_max,
                                       uint32_t zoom,
                                       osmium_tile **tiles) {

  if (url_template == NULL || cache_folder == NULL || tiles == NULL) {
    return OSMIUM_NULL_POINTER;
  }

  size_t count = tiles_for_bbox(lat_min, lon_min, lat_max, lon_max, zoom, tiles);

  if (count == 0) {
    return OSMIUM_INVALID_BBOX;
  }

  if (osmium_ensure_dir(cache_folder) != 0) {
    free(*tiles);
    *tiles = NULL;
    return OSMIUM_CACHE_ERROR;
  }

  osmium_error error = OSMIUM_OK;

  for (size_t i = 0; i < count; i++) {
    osmium_tile *tile = &(*tiles)[i];

    char *path = osmium_tile_cache_path(cache_folder, tile);
    if (path == NULL) {
      continue;
    }

    download_tile(tile, url_template, path, NULL);
    free(path);
  }

  return error;
}
