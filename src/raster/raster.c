/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "raster.h"

#include "../proj/projection.h"

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
  double lon_min, lat_min, lon_max, lat_max;
  char *source_override;
  char *crs_definition;
  char *crs_name;
  char *crs_code;
  int crs_assumed;
  int crs_ballpark;
  double crs_accuracy;
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

static char *srs_to_wkt(OGRSpatialReferenceH srs) {
  char *wkt = NULL;
  char *copy;

  if (OSRExportToWkt(srs, &wkt) != OGRERR_NONE || !wkt) {
    return NULL;
  }
  copy = strdup(wkt);
  CPLFree(wkt);
  return copy;
}

static char *srs_to_definition(OGRSpatialReferenceH srs) {
  const char *authority = OSRGetAuthorityName(srs, NULL);
  const char *code = OSRGetAuthorityCode(srs, NULL);

  if (authority && code) {
    size_t len = strlen(authority) + strlen(code) + 2;
    char *out = malloc(len);

    if (out) {
      snprintf(out, len, "%s:%s", authority, code);
    }
    return out;
  }
  return srs_to_wkt(srs);
}

static int resolve_source_crs(GDALDatasetH dataset, OsmiumRaster *raster) {
  OGRSpatialReferenceH srs = NULL;
  int own = 0;

  if (raster->source_override && *raster->source_override) {
    srs = OSRNewSpatialReference(NULL);
    own = 1;
    if (OSRSetFromUserInput(srs, raster->source_override) != OGRERR_NONE) {
      OSRDestroySpatialReference(srs);
      return 0;
    }
  } else {
    srs = (OGRSpatialReferenceH)GDALGetSpatialRef(dataset);
    if (!srs) {
      srs = OSRNewSpatialReference(NULL);
      own = 1;
      OSRImportFromEPSG(srs, 4326);
      raster->crs_assumed = 1;
    }
  }

  OSRSetAxisMappingStrategy(srs, OAMS_TRADITIONAL_GIS_ORDER);
  raster->crs_definition = srs_to_definition(srs);
  if (own) {
    OSRDestroySpatialReference(srs);
  }
  return raster->crs_definition != NULL;
}

static int has_georeferencing(GDALDatasetH dataset) {
  double geotransform[6];

  return GDALGetGeoTransform(dataset, geotransform) == CE_None || GDALGetGCPCount(dataset) > 0;
}

static int compute_extent(GDALDatasetH dataset, OsmiumRaster *raster) {
  OGRSpatialReferenceH source = OSRNewSpatialReference(NULL);
  OGRSpatialReferenceH target = OSRNewSpatialReference(NULL);
  char *source_wkt = NULL;
  char *target_wkt = NULL;
  double geotransform[6];
  int width = 0, height = 0;
  double extent[4];

  if (OSRSetFromUserInput(source, raster->crs_definition) != OGRERR_NONE) {
    OSRDestroySpatialReference(source);
    OSRDestroySpatialReference(target);
    return 0;
  }
  OSRImportFromEPSG(target, 4326);
  OSRExportToWkt(source, &source_wkt);
  OSRExportToWkt(target, &target_wkt);
  OSRDestroySpatialReference(source);
  OSRDestroySpatialReference(target);

  void *transformer = GDALCreateGenImgProjTransformer(dataset, source_wkt, NULL, target_wkt, FALSE, 0.0, 0);
  CPLFree(source_wkt);
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

  raster->lon_min = extent[0];
  raster->lat_min = extent[1];
  raster->lon_max = extent[2];
  raster->lat_max = extent[3];
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

osmium_error osmium_raster_open(const char *path,
                                const char *user_agent,
                                const char *source_crs,
                                OsmiumRaster **out) {
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
  raster->source_override = source_crs && *source_crs ? strdup(source_crs) : NULL;
  raster->crs_accuracy = -1.0;
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

  if (!has_georeferencing(dataset) || !resolve_source_crs(dataset, raster) ||
      !compute_extent(dataset, raster)) {
    GDALClose(dataset);
    osmium_raster_unref(raster);
    return OSMIUM_CRS_ERROR;
  }

  {
    OsmiumCrsInfo info;
    const double area[4] = {raster->lon_min, raster->lat_min, raster->lon_max, raster->lat_max};
    OsmiumTransform *transform = osmium_transform_new(raster->crs_definition, "EPSG:4326", area, NULL, 0);

    if (osmium_crs_describe_internal(raster->crs_definition, &info)) {
      raster->crs_name = info.name;
      raster->crs_code = info.code;
    }
    if (transform) {
      raster->crs_ballpark = osmium_transform_is_ballpark(transform);
      raster->crs_accuracy = osmium_transform_accuracy(transform);
      osmium_transform_free(transform);
    }
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
  free(raster->source_override);
  free(raster->crs_definition);
  free(raster->crs_name);
  free(raster->crs_code);
  free(raster);
}

void osmium_raster_get_extent(const OsmiumRaster *raster,
                              double *lon_min,
                              double *lat_min,
                              double *lon_max,
                              double *lat_max) {
  *lon_min = raster->lon_min;
  *lat_min = raster->lat_min;
  *lon_max = raster->lon_max;
  *lat_max = raster->lat_max;
}

void osmium_raster_get_crs_info(const OsmiumRaster *raster,
                                const char **name,
                                const char **code,
                                int *assumed,
                                int *ballpark,
                                double *accuracy) {
  *name = raster->crs_name;
  *code = raster->crs_code;
  *assumed = raster->crs_assumed;
  *ballpark = raster->crs_ballpark;
  *accuracy = raster->crs_accuracy;
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
                                 const char *target_crs,
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
  argv = CSLAddString(argv, "-s_srs");
  argv = CSLAddString(argv, raster->crs_definition);
  argv = CSLAddString(argv, "-t_srs");
  argv = CSLAddString(argv, target_crs);
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
                                  const char *target_crs,
                                  double min_x,
                                  double min_y,
                                  double max_x,
                                  double max_y,
                                  int width,
                                  int height,
                                  unsigned char **rgba) {
  if (!raster || !rgba || !target_crs || width <= 0 || height <= 0) {
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

  GDALDatasetH warped = warp_to_view(raster, target_crs, vrt, min_x, min_y, max_x, max_y, width, height);
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

osmium_error osmium_image_reproject(const unsigned char *data,
                                    size_t len,
                                    const char *source_crs,
                                    const double *source_box,
                                    const char *target_crs,
                                    const double *target_box,
                                    int width,
                                    int height,
                                    unsigned char **rgba) {
  static atomic_uint counter;
  char name[64];

  if (!data || !source_crs || !source_box || !target_crs || !target_box || !rgba || width <= 0 || height <= 0) {
    return OSMIUM_NULL_POINTER;
  }
  *rgba = NULL;

  pthread_once(&gdal_once, gdal_init);

  snprintf(name, sizeof(name), "/vsimem/osmium-image-%u", atomic_fetch_add(&counter, 1u));

  VSILFILE *file = VSIFileFromMemBuffer(name, (GByte *)data, len, FALSE);
  if (!file) {
    return OSMIUM_OUT_OF_MEMORY;
  }
  VSIFCloseL(file);

  osmium_error result = OSMIUM_FILE_ERROR;
  GDALDatasetH source = GDALOpenEx(name, GDAL_OF_RASTER | GDAL_OF_READONLY, NULL, NULL, NULL);
  GDALDatasetH vrt = NULL;
  GDALDatasetH warped = NULL;
  unsigned char *out = NULL;

  if (source) {
    char *values[8];
    char **argv = NULL;
    const int palette = GDALGetRasterColorTable(GDALGetRasterBand(source, 1)) != NULL;
    const int bands = GDALGetRasterCount(source);
    const int has_alpha = palette || bands == 4 || bands == 2;

    values[0] = format_double(source_box[0]);
    values[1] = format_double(source_box[3]);
    values[2] = format_double(source_box[2]);
    values[3] = format_double(source_box[1]);

    argv = CSLAddString(argv, "-of");
    argv = CSLAddString(argv, "VRT");
    argv = CSLAddString(argv, "-a_srs");
    argv = CSLAddString(argv, source_crs);
    argv = CSLAddString(argv, "-a_ullr");
    for (int i = 0; i < 4; i++) {
      argv = CSLAddString(argv, values[i] ? values[i] : "0");
      free(values[i]);
    }
    if (palette) {
      argv = CSLAddString(argv, "-expand");
      argv = CSLAddString(argv, "rgba");
    }

    GDALTranslateOptions *translate = GDALTranslateOptionsNew(argv, NULL);
    CSLDestroy(argv);
    vrt = translate ? GDALTranslate("", source, translate, NULL) : NULL;
    if (translate) {
      GDALTranslateOptionsFree(translate);
    }

    if (vrt) {
      char *t_values[4] = {format_double(target_box[0]), format_double(target_box[1]),
                           format_double(target_box[2]), format_double(target_box[3])};
      char size[2][16];

      snprintf(size[0], sizeof(size[0]), "%d", width);
      snprintf(size[1], sizeof(size[1]), "%d", height);

      argv = NULL;
      argv = CSLAddString(argv, "-of");
      argv = CSLAddString(argv, "MEM");
      argv = CSLAddString(argv, "-t_srs");
      argv = CSLAddString(argv, target_crs);
      argv = CSLAddString(argv, "-te");
      for (int i = 0; i < 4; i++) {
        argv = CSLAddString(argv, t_values[i] ? t_values[i] : "0");
        free(t_values[i]);
      }
      argv = CSLAddString(argv, "-ts");
      argv = CSLAddString(argv, size[0]);
      argv = CSLAddString(argv, size[1]);
      argv = CSLAddString(argv, "-r");
      argv = CSLAddString(argv, palette ? "near" : "bilinear");
      if (!has_alpha) {
        argv = CSLAddString(argv, "-dstalpha");
      }

      GDALWarpAppOptions *warp = GDALWarpAppOptionsNew(argv, NULL);
      CSLDestroy(argv);
      warped = warp ? GDALWarp("", NULL, 1, &vrt, warp, NULL) : NULL;
      if (warp) {
        GDALWarpAppOptionsFree(warp);
      }
    }
  }

  if (warped) {
    const size_t pixels = (size_t)width * (size_t)height;
    const int count = GDALGetRasterCount(warped);
    unsigned char *plane = malloc(pixels);

    out = malloc(pixels * 4);
    if (out && plane) {
      memset(out, 255, pixels * 4);
      result = OSMIUM_OK;

      for (int b = 1; b <= count && b <= 4 && result == OSMIUM_OK; b++) {
        int channel = b - 1;

        if (count == 2) {
          channel = b == 1 ? 0 : 3;
        } else if (count == 1) {
          channel = 0;
        }

        if (GDALRasterIO(GDALGetRasterBand(warped, b), GF_Read, 0, 0, width, height, plane, width, height,
                         GDT_Byte, 0, 0) != CE_None) {
          result = OSMIUM_FILE_ERROR;
          break;
        }
        for (size_t i = 0; i < pixels; i++) {
          out[i * 4 + channel] = plane[i];
          if (count <= 2 && channel == 0) {
            out[i * 4 + 1] = out[i * 4 + 2] = plane[i];
          }
        }
      }
    } else {
      result = OSMIUM_OUT_OF_MEMORY;
    }
    free(plane);
  }

  if (warped) {
    GDALClose(warped);
  }
  if (vrt) {
    GDALClose(vrt);
  }
  if (source) {
    GDALClose(source);
  }
  VSIUnlink(name);

  if (result != OSMIUM_OK) {
    free(out);
    return result;
  }
  *rgba = out;
  return OSMIUM_OK;
}
