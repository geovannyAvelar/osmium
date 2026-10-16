/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "raster.h"

#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_alg.h>
#include <gdal_utils.h>
#include <locale.h>
#include <math.h>
#include <ogr_srs_api.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct OsmiumRaster {
  atomic_int refs;
  char *path;
  char *user_agent;
  int color_bands;
  int has_alpha_band;
  int expand_palette;
  int scale;
  double scale_min;
  double scale_max;
  double min_x, min_y, max_x, max_y;
};

static pthread_once_t gdal_once = PTHREAD_ONCE_INIT;

static void gdal_init(void) {
  GDALAllRegister();
  CPLSetConfigOption("GDAL_PAM_ENABLED", "NO");
}

static int is_remote(const char *path) {
  return strncasecmp(path, "http://", 7) == 0 || strncasecmp(path, "https://", 8) == 0;
}

static char *gdal_path(const char *path) {
  if (!is_remote(path)) {
    return strdup(path);
  }

  size_t len = strlen(path) + sizeof("/vsicurl/");
  char *out = malloc(len);
  if (out) {
    snprintf(out, len, "/vsicurl/%s", path);
  }
  return out;
}

static GDALDatasetH open_dataset(const OsmiumRaster *raster) {
  CPLSetThreadLocalConfigOption("GDAL_HTTP_USERAGENT", raster->user_agent);
  CPLSetThreadLocalConfigOption("GDAL_HTTP_TIMEOUT", "30");
  CPLSetThreadLocalConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "EMPTY_DIR");
  return GDALOpenEx(raster->path, GDAL_OF_RASTER | GDAL_OF_READONLY, NULL, NULL, NULL);
}

static char *format_double(double value) {
  char text[64];
  locale_t c_loc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  locale_t old = c_loc ? uselocale(c_loc) : (locale_t)0;

  snprintf(text, sizeof(text), "%.17g", value);

  if (c_loc) {
    uselocale(old);
    freelocale(c_loc);
  }
  return strdup(text);
}

static int compute_extent(GDALDatasetH dataset, OsmiumRaster *raster) {
  OGRSpatialReferenceH target = OSRNewSpatialReference(NULL);
  char *target_wkt = NULL;
  double geotransform[6];
  int width = 0, height = 0;
  double extent[4];

  OSRImportFromEPSG(target, 3857);
  OSRExportToWkt(target, &target_wkt);
  OSRDestroySpatialReference(target);

  void *transformer = GDALCreateGenImgProjTransformer(dataset, NULL, NULL, target_wkt, FALSE, 0.0, 0);
  CPLFree(target_wkt);
  if (!transformer) {
    return 0;
  }

  CPLErr err = GDALSuggestedWarpOutput2(dataset, GDALGenImgProjTransform, transformer, geotransform,
                                        &width, &height, extent, 0);
  GDALDestroyGenImgProjTransformer(transformer);
  if (err != CE_None || !(extent[2] > extent[0]) || !(extent[3] > extent[1])) {
    return 0;
  }

  raster->min_x = extent[0];
  raster->min_y = extent[1];
  raster->max_x = extent[2];
  raster->max_y = extent[3];
  return 1;
}

static void plan_bands(GDALDatasetH dataset, OsmiumRaster *raster) {
  const int count = GDALGetRasterCount(dataset);
  GDALRasterBandH first = GDALGetRasterBand(dataset, 1);

  if (GDALGetRasterColorTable(first) != NULL) {
    raster->expand_palette = 1;
    raster->color_bands = 3;
    return;
  }

  if (count >= 4 && GDALGetRasterColorInterpretation(GDALGetRasterBand(dataset, 4)) == GCI_AlphaBand &&
      GDALGetRasterColorInterpretation(GDALGetRasterBand(dataset, 3)) != GCI_AlphaBand) {
    raster->color_bands = 3;
    raster->has_alpha_band = 1;
  } else if (count == 2 &&
             GDALGetRasterColorInterpretation(GDALGetRasterBand(dataset, 2)) == GCI_AlphaBand) {
    raster->color_bands = 1;
    raster->has_alpha_band = 1;
  } else if (count >= 3) {
    raster->color_bands = 3;
  } else {
    raster->color_bands = 1;
  }

  if (GDALGetRasterDataType(first) != GDT_Byte) {
    double lo = INFINITY;
    double hi = -INFINITY;

    for (int b = 1; b <= raster->color_bands; b++) {
      double minmax[2];

      GDALComputeRasterMinMax(GDALGetRasterBand(dataset, b), TRUE, minmax);
      lo = fmin(lo, minmax[0]);
      hi = fmax(hi, minmax[1]);
    }

    if (!isfinite(lo) || !isfinite(hi)) {
      lo = 0.0;
      hi = 1.0;
    }
    if (hi <= lo) {
      hi = lo + 1.0;
    }

    raster->scale = 1;
    raster->scale_min = lo;
    raster->scale_max = hi;
  }
}

osmium_error osmium_raster_open(const char *path, const char *user_agent, OsmiumRaster **out) {
  if (!path || !out) {
    return OSMIUM_NULL_POINTER;
  }
  *out = NULL;

  pthread_once(&gdal_once, gdal_init);

  OsmiumRaster *raster = calloc(1, sizeof(*raster));
  if (!raster) {
    return OSMIUM_OUT_OF_MEMORY;
  }
  atomic_init(&raster->refs, 1);

  raster->path = gdal_path(path);
  raster->user_agent = strdup(user_agent ? user_agent : "Osmium/0.1");
  if (!raster->path || !raster->user_agent) {
    osmium_raster_unref(raster);
    return OSMIUM_OUT_OF_MEMORY;
  }

  GDALDatasetH dataset = open_dataset(raster);
  if (!dataset || GDALGetRasterCount(dataset) == 0) {
    if (dataset) {
      GDALClose(dataset);
    }
    osmium_raster_unref(raster);
    return OSMIUM_FILE_ERROR;
  }

  if (!compute_extent(dataset, raster)) {
    GDALClose(dataset);
    osmium_raster_unref(raster);
    return OSMIUM_CRS_ERROR;
  }

  plan_bands(dataset, raster);
  GDALClose(dataset);

  *out = raster;
  return OSMIUM_OK;
}

OsmiumRaster *osmium_raster_ref(OsmiumRaster *raster) {
  atomic_fetch_add(&raster->refs, 1);
  return raster;
}

void osmium_raster_unref(OsmiumRaster *raster) {
  if (!raster || atomic_fetch_sub(&raster->refs, 1) != 1) {
    return;
  }
  free(raster->path);
  free(raster->user_agent);
  free(raster);
}

void osmium_raster_get_extent(const OsmiumRaster *raster,
                              double *min_x,
                              double *min_y,
                              double *max_x,
                              double *max_y) {
  *min_x = raster->min_x;
  *min_y = raster->min_y;
  *max_x = raster->max_x;
  *max_y = raster->max_y;
}

static GDALDatasetH make_source_vrt(const OsmiumRaster *raster, GDALDatasetH source) {
  char **argv = NULL;

  argv = CSLAddString(argv, "-of");
  argv = CSLAddString(argv, "VRT");

  if (raster->expand_palette) {
    argv = CSLAddString(argv, "-expand");
    argv = CSLAddString(argv, "rgb");
  } else {
    for (int b = 1; b <= raster->color_bands; b++) {
      char band[16];

      snprintf(band, sizeof(band), "%d", b);
      argv = CSLAddString(argv, "-b");
      argv = CSLAddString(argv, band);
    }
    if (raster->has_alpha_band) {
      char band[16];

      snprintf(band, sizeof(band), "%d", raster->color_bands + 1);
      argv = CSLAddString(argv, "-b");
      argv = CSLAddString(argv, band);
    }
  }

  GDALTranslateOptions *options = GDALTranslateOptionsNew(argv, NULL);
  CSLDestroy(argv);
  if (!options) {
    return NULL;
  }

  GDALDatasetH vrt = GDALTranslate("", source, options, NULL);
  GDALTranslateOptionsFree(options);
  return vrt;
}

static GDALDatasetH warp_to_view(const OsmiumRaster *raster,
                                 GDALDatasetH vrt,
                                 double min_x,
                                 double min_y,
                                 double max_x,
                                 double max_y,
                                 int width,
                                 int height) {
  char **argv = NULL;
  char *values[4] = {format_double(min_x), format_double(min_y), format_double(max_x),
                     format_double(max_y)};
  char size[2][16];

  snprintf(size[0], sizeof(size[0]), "%d", width);
  snprintf(size[1], sizeof(size[1]), "%d", height);

  argv = CSLAddString(argv, "-of");
  argv = CSLAddString(argv, "MEM");
  argv = CSLAddString(argv, "-t_srs");
  argv = CSLAddString(argv, "EPSG:3857");
  argv = CSLAddString(argv, "-te");
  for (int i = 0; i < 4; i++) {
    argv = CSLAddString(argv, values[i] ? values[i] : "0");
    free(values[i]);
  }
  argv = CSLAddString(argv, "-ts");
  argv = CSLAddString(argv, size[0]);
  argv = CSLAddString(argv, size[1]);
  argv = CSLAddString(argv, "-r");
  argv = CSLAddString(argv, raster->expand_palette ? "near" : "bilinear");
  argv = CSLAddString(argv, "-dstalpha");
  argv = CSLAddString(argv, "-ot");
  argv = CSLAddString(argv, raster->scale ? "Float32" : "Byte");

  GDALWarpAppOptions *options = GDALWarpAppOptionsNew(argv, NULL);
  CSLDestroy(argv);
  if (!options) {
    return NULL;
  }

  GDALDatasetH warped = GDALWarp("", NULL, 1, &vrt, options, NULL);
  GDALWarpAppOptionsFree(options);
  return warped;
}

static unsigned char to_byte(float value, double lo, double span) {
  double scaled = (value - lo) / span * 255.0;

  if (!(scaled > 0.0)) {
    return 0;
  }
  return scaled >= 255.0 ? 255 : (unsigned char)(scaled + 0.5);
}

osmium_error osmium_raster_render(const OsmiumRaster *raster,
                                  double min_x,
                                  double min_y,
                                  double max_x,
                                  double max_y,
                                  int width,
                                  int height,
                                  unsigned char **rgba) {
  if (!raster || !rgba || width <= 0 || height <= 0) {
    return OSMIUM_NULL_POINTER;
  }
  *rgba = NULL;

  GDALDatasetH source = open_dataset(raster);
  if (!source) {
    return OSMIUM_FILE_ERROR;
  }

  GDALDatasetH vrt = make_source_vrt(raster, source);
  if (!vrt) {
    GDALClose(source);
    return OSMIUM_FILE_ERROR;
  }

  GDALDatasetH warped = warp_to_view(raster, vrt, min_x, min_y, max_x, max_y, width, height);
  GDALClose(vrt);
  GDALClose(source);
  if (!warped) {
    return OSMIUM_FILE_ERROR;
  }

  const int colors = raster->color_bands;
  const size_t pixels = (size_t)width * (size_t)height;

  if (GDALGetRasterCount(warped) != colors + 1) {
    GDALClose(warped);
    return OSMIUM_FILE_ERROR;
  }

  unsigned char *out = malloc(pixels * 4);
  float *plane = malloc(pixels * sizeof(float));
  if (!out || !plane) {
    free(out);
    free(plane);
    GDALClose(warped);
    return OSMIUM_OUT_OF_MEMORY;
  }

  const double lo = raster->scale ? raster->scale_min : 0.0;
  const double span = raster->scale ? raster->scale_max - raster->scale_min : 255.0;
  osmium_error result = OSMIUM_OK;

  for (int c = 0; c < colors && result == OSMIUM_OK; c++) {
    if (GDALRasterIO(GDALGetRasterBand(warped, c + 1), GF_Read, 0, 0, width, height, plane, width,
                     height, GDT_Float32, 0, 0) != CE_None) {
      result = OSMIUM_FILE_ERROR;
      break;
    }

    for (size_t i = 0; i < pixels; i++) {
      const unsigned char v = to_byte(plane[i], lo, span);

      if (colors == 1) {
        out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = v;
      } else {
        out[i * 4 + c] = v;
      }
    }
  }

  if (result == OSMIUM_OK &&
      GDALRasterIO(GDALGetRasterBand(warped, colors + 1), GF_Read, 0, 0, width, height, plane, width,
                   height, GDT_Float32, 0, 0) != CE_None) {
    result = OSMIUM_FILE_ERROR;
  }

  if (result == OSMIUM_OK) {
    for (size_t i = 0; i < pixels; i++) {
      out[i * 4 + 3] = to_byte(plane[i], 0.0, 255.0);
    }
  }

  free(plane);
  GDALClose(warped);

  if (result != OSMIUM_OK) {
    free(out);
    return result;
  }

  *rgba = out;
  return OSMIUM_OK;
}
