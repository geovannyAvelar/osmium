/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <gtk/gtk.h>
#include "osmium_map.h"
#include "osmium_vector_layer.h"

G_BEGIN_DECLS

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

OSMIUM_API void osmium_raster_layer_reload(OsmiumRasterLayer *self);
OSMIUM_API gboolean osmium_raster_layer_get_loading(OsmiumRasterLayer *self);

OSMIUM_API gboolean osmium_raster_layer_get_extent(OsmiumRasterLayer *self,
                                                   double *lat_min,
                                                   double *lon_min,
                                                   double *lat_max,
                                                   double *lon_max);

G_END_DECLS
