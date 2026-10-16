/* SPDX-License-Identifier: LGPL-2.1-or-later */

#pragma once

#include <gtk/gtk.h>

#define OSMIUM_API __attribute__((visibility("default")))

G_BEGIN_DECLS

#define OSMIUM_MAP_MAX_ZOOM 19 /* highest zoom served by tile.openstreetmap.org */

#define OSMIUM_TYPE_MAP (osmium_map_get_type())

G_DECLARE_FINAL_TYPE(
    OsmiumMap,
    osmium_map,
    OSMIUM,
    MAP,
    GtkWidget
)

OSMIUM_API GtkWidget *osmium_map_new(void);

OSMIUM_API void osmium_map_set_view(OsmiumMap *self, double lat, double lon, guint zoom);

OSMIUM_API double osmium_map_get_lat(OsmiumMap *self);
OSMIUM_API void osmium_map_set_lat(OsmiumMap *self, double lat);

OSMIUM_API double osmium_map_get_lon(OsmiumMap *self);
OSMIUM_API void osmium_map_set_lon(OsmiumMap *self, double lon);

OSMIUM_API guint osmium_map_get_zoom(OsmiumMap *self);
OSMIUM_API void osmium_map_set_zoom(OsmiumMap *self, guint zoom);

OSMIUM_API double osmium_map_get_bearing(OsmiumMap *self);
OSMIUM_API void osmium_map_set_bearing(OsmiumMap *self, double bearing);

/* Overlays draw in the order added: add layers before controls.
 * Widgets with a "map" property get bound to it. Emits "view-changed" after each view change. */
/* Coordinate system the map is drawn in. Any definition PROJ accepts (EPSG:4326, EPSG:31983, WKT...).
 * Returns FALSE and keeps the old one when the definition is not valid. */
OSMIUM_API const char *osmium_map_get_crs(OsmiumMap *self);
OSMIUM_API gboolean osmium_map_set_crs(OsmiumMap *self, const char *crs);
OSMIUM_API const char *osmium_map_get_crs_name(OsmiumMap *self);
OSMIUM_API gboolean osmium_map_crs_is_web_mercator(OsmiumMap *self);

/* World pixels of a position at a zoom level, in the map's coordinate system. */
OSMIUM_API void osmium_map_lonlat_to_world(OsmiumMap *self, double lat, double lon, guint zoom, double *px, double *py);
OSMIUM_API void osmium_map_world_to_lonlat(OsmiumMap *self, double px, double py, guint zoom, double *lat, double *lon);

/* Normalized map coordinates (0..1 over the nominal world) to CRS units. */
OSMIUM_API void osmium_map_normalized_to_units(OsmiumMap *self, double nx, double ny, double *x, double *y);

OSMIUM_API void osmium_map_widget_to_latlon(OsmiumMap *self, double x, double y, double *lat, double *lon);

OSMIUM_API void osmium_map_add_overlay(OsmiumMap *self, GtkWidget *child);
OSMIUM_API void osmium_map_insert_overlay(OsmiumMap *self, GtkWidget *child, int position);
OSMIUM_API void osmium_map_move_overlay(OsmiumMap *self, GtkWidget *child, int position);
OSMIUM_API void osmium_map_remove_overlay(OsmiumMap *self, GtkWidget *child);

G_END_DECLS

