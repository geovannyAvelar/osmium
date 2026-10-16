/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_georef.h"
#include "gdal_setup.h"

#include "proj/projection.h"

#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_alg.h>
#include <gdal_utils.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

guint osmium_georef_minimum_points(OsmiumGeorefMethod method) {
  switch (method) {
  case OSMIUM_GEOREF_LINEAR:
    return 3;
  case OSMIUM_GEOREF_POLYNOMIAL_2:
    return 6;
  case OSMIUM_GEOREF_POLYNOMIAL_3:
    return 10;
  case OSMIUM_GEOREF_THIN_PLATE_SPLINE:
  default:
    return 3;
  }
}

gboolean osmium_georef_image_size(const char *path, guint *width, guint *height, GError **error) {
  osmium_gdal_init();

  GDALDatasetH dataset = GDALOpen(path, GA_ReadOnly);

  if (dataset == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not read the image %s", path);
    return FALSE;
  }
  *width = (guint)GDALGetRasterXSize(dataset);
  *height = (guint)GDALGetRasterYSize(dataset);
  GDALClose(dataset);
  return *width > 0 && *height > 0;
}

static GDAL_GCP *build_gcps(const OsmiumGcp *points, guint n_points, guint *count) {
  GDAL_GCP *gcps = g_new0(GDAL_GCP, MAX(n_points, 1));
  guint used = 0;

  for (guint i = 0; i < n_points; i++) {
    if (!points[i].enabled) {
      continue;
    }
    gcps[used].pszId = (char *)"";
    gcps[used].pszInfo = (char *)"";
    gcps[used].dfGCPPixel = points[i].col;
    gcps[used].dfGCPLine = points[i].row;
    gcps[used].dfGCPX = points[i].x;
    gcps[used].dfGCPY = points[i].y;
    gcps[used].dfGCPZ = 0.0;
    used++;
  }
  *count = used;
  return gcps;
}

gboolean osmium_georef_residuals(const OsmiumGcp *points, guint n_points, OsmiumGeorefMethod method,
                                 double *residuals, double *rms, GError **error) {
  guint count;
  GDAL_GCP *gcps = build_gcps(points, n_points, &count);
  void *transformer = NULL;
  double sum = 0.0;

  osmium_gdal_init();
  for (guint i = 0; i < n_points; i++) {
    residuals[i] = -1.0;
  }
  *rms = 0.0;

  if (count < osmium_georef_minimum_points(method)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "At least %u points are needed", osmium_georef_minimum_points(method));
    g_free(gcps);
    return FALSE;
  }

  if (method == OSMIUM_GEOREF_THIN_PLATE_SPLINE) {
    transformer = GDALCreateTPSTransformer((int)count, gcps, FALSE);
  } else {
    transformer = GDALCreateGCPTransformer((int)count, gcps, method == OSMIUM_GEOREF_LINEAR ? 1 : (method == OSMIUM_GEOREF_POLYNOMIAL_2 ? 2 : 3), FALSE);
  }
  if (transformer == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "The points can't define a transformation (are some of them in a line or repeated?)");
    g_free(gcps);
    return FALSE;
  }

  {
    guint k = 0;

    for (guint i = 0; i < n_points; i++) {
      double x, y, z = 0.0;
      int ok = 0;

      if (!points[i].enabled) {
        continue;
      }
      x = points[i].col;
      y = points[i].row;
      if (method == OSMIUM_GEOREF_THIN_PLATE_SPLINE) {
        GDALTPSTransform(transformer, FALSE, 1, &x, &y, &z, &ok);
      } else {
        GDALGCPTransform(transformer, FALSE, 1, &x, &y, &z, &ok);
      }
      residuals[i] = ok ? hypot(x - points[i].x, y - points[i].y) : -1.0;
      sum += ok ? residuals[i] * residuals[i] : 0.0;
      k++;
    }
    *rms = k > 0 ? sqrt(sum / k) : 0.0;
  }

  if (method == OSMIUM_GEOREF_THIN_PLATE_SPLINE) {
    GDALDestroyTPSTransformer(transformer);
  } else {
    GDALDestroyGCPTransformer(transformer);
  }
  g_free(gcps);
  return TRUE;
}

static char *format_double(double value) {
  char buffer[G_ASCII_DTOSTR_BUF_SIZE];

  g_ascii_formatd(buffer, sizeof(buffer), "%.15g", value);
  return g_strdup(buffer);
}

gboolean osmium_georef_run(const char *input,
                           const char *output,
                           const OsmiumGcp *points,
                           guint n_points,
                           OsmiumGeorefMethod method,
                           const char *target_crs,
                           OsmiumGeorefResampling resampling,
                           GError **error) {
  osmium_gdal_init();

  guint count = 0;
  GDAL_GCP *gcps = build_gcps(points, n_points, &count);
  GDALDatasetH source = NULL;
  GDALDatasetH vrt = NULL;
  GDALDatasetH warped = NULL;
  char **argv = NULL;
  gboolean ok = FALSE;
  int usage_error = 0;

  g_free(gcps);

  if (count < osmium_georef_minimum_points(method)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "At least %u points are needed for this transformation",
                osmium_georef_minimum_points(method));
    return FALSE;
  }

  source = GDALOpen(input, GA_ReadOnly);
  if (source == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not read the image %s", input);
    return FALSE;
  }

  argv = CSLAddString(argv, "-of");
  argv = CSLAddString(argv, "VRT");
  argv = CSLAddString(argv, "-a_srs");
  argv = CSLAddString(argv, target_crs);
  for (guint i = 0; i < n_points; i++) {
    g_autofree char *col = NULL;
    g_autofree char *row = NULL;
    g_autofree char *x = NULL;
    g_autofree char *y = NULL;

    if (!points[i].enabled) {
      continue;
    }
    col = format_double(points[i].col);
    row = format_double(points[i].row);
    x = format_double(points[i].x);
    y = format_double(points[i].y);
    argv = CSLAddString(argv, "-gcp");
    argv = CSLAddString(argv, col);
    argv = CSLAddString(argv, row);
    argv = CSLAddString(argv, x);
    argv = CSLAddString(argv, y);
  }

  {
    GDALTranslateOptions *options = GDALTranslateOptionsNew(argv, NULL);

    CSLDestroy(argv);
    argv = NULL;
    if (options == NULL) {
      GDALClose(source);
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not set up the control points");
      return FALSE;
    }
    vrt = GDALTranslate("", source, options, &usage_error);
    GDALTranslateOptionsFree(options);
  }
  if (vrt == NULL) {
    GDALClose(source);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not attach the control points to the image");
    return FALSE;
  }

  argv = CSLAddString(argv, "-overwrite");
  argv = CSLAddString(argv, "-of");
  argv = CSLAddString(argv, "GTiff");
  argv = CSLAddString(argv, "-t_srs");
  argv = CSLAddString(argv, target_crs);
  argv = CSLAddString(argv, "-r");
  argv = CSLAddString(argv, resampling == OSMIUM_GEOREF_NEAREST ? "near" : (resampling == OSMIUM_GEOREF_BILINEAR ? "bilinear" : "cubic"));
  if (method == OSMIUM_GEOREF_THIN_PLATE_SPLINE) {
    argv = CSLAddString(argv, "-tps");
  } else {
    argv = CSLAddString(argv, "-order");
    argv = CSLAddString(argv, method == OSMIUM_GEOREF_LINEAR ? "1" : (method == OSMIUM_GEOREF_POLYNOMIAL_2 ? "2" : "3"));
  }
  argv = CSLAddString(argv, "-dstalpha");
  argv = CSLAddString(argv, "-co");
  argv = CSLAddString(argv, "COMPRESS=DEFLATE");
  argv = CSLAddString(argv, "-co");
  argv = CSLAddString(argv, "TILED=YES");

  {
    GDALWarpAppOptions *options = GDALWarpAppOptionsNew(argv, NULL);

    CSLDestroy(argv);
    if (options != NULL) {
      warped = GDALWarp(output, NULL, 1, &vrt, options, &usage_error);
      GDALWarpAppOptionsFree(options);
    }
  }

  if (warped != NULL) {
    GDALClose(warped);
    ok = TRUE;
  } else {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not write %s", output);
  }

  GDALClose(vrt);
  GDALClose(source);
  return ok;
}

gboolean osmium_georef_save_points(const char *path, const OsmiumGcp *points, guint n_points, const char *crs,
                                   GError **error) {
  GString *text = g_string_new(NULL);
  gboolean ok;

  if (crs != NULL && crs[0] != '\0') {
    g_string_append_printf(text, "#CRS: %s\n", crs);
  }
  g_string_append(text, "mapX,mapY,sourceX,sourceY,enable,dX,dY,residual\n");
  for (guint i = 0; i < n_points; i++) {
    g_autofree char *x = format_double(points[i].x);
    g_autofree char *y = format_double(points[i].y);
    g_autofree char *col = format_double(points[i].col);
    g_autofree char *row = format_double(points[i].row == 0.0 ? 0.0 : -points[i].row);

    g_string_append_printf(text, "%s,%s,%s,%s,%d,0,0,0\n", x, y, col, row, points[i].enabled ? 1 : 0);
  }
  ok = g_file_set_contents(path, text->str, (gssize)text->len, error);
  g_string_free(text, TRUE);
  return ok;
}

GArray *osmium_georef_load_points(const char *path, char **crs, GError **error) {
  g_autofree char *contents = NULL;
  g_auto(GStrv) lines = NULL;
  GArray *points = g_array_new(FALSE, FALSE, sizeof(OsmiumGcp));

  if (crs != NULL) {
    *crs = NULL;
  }
  if (!g_file_get_contents(path, &contents, NULL, error)) {
    g_array_unref(points);
    return NULL;
  }

  lines = g_strsplit(contents, "\n", -1);
  for (guint i = 0; lines[i] != NULL; i++) {
    g_auto(GStrv) fields = NULL;
    char *end;
    OsmiumGcp point = {.enabled = TRUE};

    g_strstrip(lines[i]);
    if (lines[i][0] == '\0') {
      continue;
    }
    if (lines[i][0] == '#') {
      if (crs != NULL && g_str_has_prefix(lines[i], "#CRS:")) {
        *crs = g_strstrip(g_strdup(lines[i] + 5));
      }
      continue;
    }
    fields = g_strsplit(lines[i], ",", -1);
    if (g_strv_length(fields) < 4) {
      continue;
    }
    point.x = g_ascii_strtod(fields[0], &end);
    if (end == fields[0]) {
      continue;
    }
    point.y = g_ascii_strtod(fields[1], NULL);
    point.col = g_ascii_strtod(fields[2], NULL);
    point.row = -g_ascii_strtod(fields[3], NULL);
    if (g_strv_length(fields) >= 5) {
      point.enabled = g_ascii_strtod(fields[4], NULL) != 0.0;
    }
    g_array_append_val(points, point);
  }
  return points;
}

gboolean osmium_crs_transform_point(const char *source, const char *target, double x, double y, double *out_x,
                                    double *out_y) {
  OsmiumTransform *transform = osmium_transform_new(source, target, NULL, NULL, 0);
  double xy[2] = {x, y};
  gboolean ok;

  if (transform == NULL) {
    return FALSE;
  }
  ok = osmium_transform_apply(transform, xy, 1) == 0 && isfinite(xy[0]) && isfinite(xy[1]);
  osmium_transform_free(transform);
  if (ok) {
    *out_x = xy[0];
    *out_y = xy[1];
  }
  return ok;
}
