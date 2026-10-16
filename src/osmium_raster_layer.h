/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <gtk/gtk.h>
#include "osmium_map.h"
#include "osmium_vector_layer.h"

G_BEGIN_DECLS

typedef enum {
  OSMIUM_RASTER_MULTIBAND_COLOR,
  OSMIUM_RASTER_SINGLEBAND_GRAY,
  OSMIUM_RASTER_SINGLEBAND_PSEUDOCOLOR
} OsmiumRasterRenderer;

typedef enum {
  OSMIUM_STRETCH_MIN_MAX,
  OSMIUM_STRETCH_PERCENT_CUT,
  OSMIUM_STRETCH_STD_DEV,
  OSMIUM_STRETCH_CUSTOM
} OsmiumRasterStretch;

/* How the bands of a raster become colors. Band numbers start at 1; bands[0] is the red channel
 * (or the only band of the singleband renderers), bands[1] green and bands[2] blue. */
typedef struct {
  OsmiumRasterRenderer renderer;
  int bands[3];
  OsmiumRasterStretch stretch;
  double min[3];
  double max[3];
  double gamma;
  char *ramp;
  gboolean invert;
  gboolean nodata_transparent;
  gboolean custom_nodata;
  double nodata;
  gboolean range_transparent;
  double range_min;
  double range_max;
  gboolean use_alpha_band;
} OsmiumRasterStyle;

typedef struct {
  char *name;
  char *data_type;
  char *interpretation;
  gboolean has_nodata;
  double nodata;
} OsmiumRasterBand;

typedef struct {
  guint width;
  guint height;
  guint n_bands;
  OsmiumRasterBand *bands;
  int alpha_band;
  gboolean palette;
} OsmiumRasterInfo;

OSMIUM_API OsmiumRasterStyle *osmium_raster_style_copy(const OsmiumRasterStyle *style);
OSMIUM_API void osmium_raster_style_free(OsmiumRasterStyle *style);
OSMIUM_API void osmium_raster_info_free(OsmiumRasterInfo *info);

#define OSMIUM_TYPE_RASTER_LAYER (osmium_raster_layer_get_type())

G_DECLARE_FINAL_TYPE(OsmiumRasterLayer, osmium_raster_layer, OSMIUM, RASTER_LAYER, GtkWidget)

OSMIUM_API GtkWidget *osmium_raster_layer_new(OsmiumMap *map);

OSMIUM_API OsmiumMap *osmium_raster_layer_get_map(OsmiumRasterLayer *self);
OSMIUM_API void osmium_raster_layer_set_map(OsmiumRasterLayer *self, OsmiumMap *map);

OSMIUM_API const char *osmium_raster_layer_get_path(OsmiumRasterLayer *self);
OSMIUM_API void osmium_raster_layer_set_path(OsmiumRasterLayer *self, const char *path);

OSMIUM_API const char *osmium_raster_layer_get_user_agent(OsmiumRasterLayer *self);
OSMIUM_API void osmium_raster_layer_set_user_agent(OsmiumRasterLayer *self, const char *user_agent);

OSMIUM_API const char *osmium_raster_layer_get_source_crs(OsmiumRasterLayer *self);
OSMIUM_API void osmium_raster_layer_set_source_crs(OsmiumRasterLayer *self, const char *source_crs);
OSMIUM_API gboolean osmium_raster_layer_get_crs_info(OsmiumRasterLayer *self, OsmiumLayerCrsInfo *info);

/* NULL until the raster is open. The style starts as the default for the raster (RGB for three
 * or more bands, gray otherwise, stretched to the data range) and can be replaced. */
OSMIUM_API OsmiumRasterInfo *osmium_raster_layer_dup_info(OsmiumRasterLayer *self);
OSMIUM_API OsmiumRasterStyle *osmium_raster_layer_dup_style(OsmiumRasterLayer *self);
OSMIUM_API void osmium_raster_layer_set_style(OsmiumRasterLayer *self, const OsmiumRasterStyle *style);
OSMIUM_API gboolean osmium_raster_layer_compute_stretch(OsmiumRasterLayer *self,
                                                        int band,
                                                        OsmiumRasterStretch mode,
                                                        double *min,
                                                        double *max);

OSMIUM_API void osmium_raster_layer_reload(OsmiumRasterLayer *self);
OSMIUM_API gboolean osmium_raster_layer_get_loading(OsmiumRasterLayer *self);

OSMIUM_API gboolean osmium_raster_layer_get_extent(OsmiumRasterLayer *self,
                                                   double *lat_min,
                                                   double *lon_min,
                                                   double *lat_max,
                                                   double *lon_max);

G_END_DECLS
