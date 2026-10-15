#pragma once

#include <gtk/gtk.h>
#include "osmium_map.h"

G_BEGIN_DECLS

#define OSMIUM_TYPE_ZOOM_CONTROL (osmium_zoom_control_get_type())

G_DECLARE_FINAL_TYPE(OsmiumZoomControl, osmium_zoom_control, OSMIUM, ZOOM_CONTROL, GtkWidget)

/* "+" and "-" buttons bound to a map. When added with osmium_map_add_overlay() (or as
 * <child type="overlay">) the map property is set for you. */
OSMIUM_API GtkWidget *osmium_zoom_control_new(OsmiumMap *map);

OSMIUM_API OsmiumMap *osmium_zoom_control_get_map(OsmiumZoomControl *self);
OSMIUM_API void osmium_zoom_control_set_map(OsmiumZoomControl *self, OsmiumMap *map);

G_END_DECLS
