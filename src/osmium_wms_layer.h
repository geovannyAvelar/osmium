#pragma once

#include <gtk/gtk.h>
#include "osmium_map.h"

G_BEGIN_DECLS

#define OSMIUM_TYPE_WMS_LAYER (osmium_wms_layer_get_type())

G_DECLARE_FINAL_TYPE(OsmiumWmsLayer, osmium_wms_layer, OSMIUM, WMS_LAYER, GtkWidget)

OSMIUM_API GtkWidget *osmium_wms_layer_new(OsmiumMap *map);

OSMIUM_API OsmiumMap *osmium_wms_layer_get_map(OsmiumWmsLayer *self);
OSMIUM_API void osmium_wms_layer_set_map(OsmiumWmsLayer *self, OsmiumMap *map);

OSMIUM_API const char *osmium_wms_layer_get_url(OsmiumWmsLayer *self);
OSMIUM_API void osmium_wms_layer_set_url(OsmiumWmsLayer *self, const char *url);

OSMIUM_API const char *osmium_wms_layer_get_layers(OsmiumWmsLayer *self);
OSMIUM_API void osmium_wms_layer_set_layers(OsmiumWmsLayer *self, const char *layers);

OSMIUM_API const char *osmium_wms_layer_get_styles(OsmiumWmsLayer *self);
OSMIUM_API void osmium_wms_layer_set_styles(OsmiumWmsLayer *self, const char *styles);

OSMIUM_API const char *osmium_wms_layer_get_format(OsmiumWmsLayer *self);
OSMIUM_API void osmium_wms_layer_set_format(OsmiumWmsLayer *self, const char *format);

OSMIUM_API const char *osmium_wms_layer_get_version(OsmiumWmsLayer *self);
OSMIUM_API void osmium_wms_layer_set_version(OsmiumWmsLayer *self, const char *version);

OSMIUM_API const char *osmium_wms_layer_get_user_agent(OsmiumWmsLayer *self);
OSMIUM_API void osmium_wms_layer_set_user_agent(OsmiumWmsLayer *self, const char *user_agent);

OSMIUM_API gboolean osmium_wms_layer_get_transparent(OsmiumWmsLayer *self);
OSMIUM_API void osmium_wms_layer_set_transparent(OsmiumWmsLayer *self, gboolean transparent);

G_END_DECLS
