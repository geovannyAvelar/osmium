
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
OSMIUM_API void osmium_map_add_overlay(OsmiumMap *self, GtkWidget *child);
OSMIUM_API void osmium_map_remove_overlay(OsmiumMap *self, GtkWidget *child);

G_END_DECLS

