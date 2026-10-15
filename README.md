# Osmium

GTK4 slippy-map widget (`OsmiumMap`). The map holds the view; what it shows comes from
layers added as overlays. `OsmiumTileLayer` draws OpenStreetMap-style (TMS/XYZ) tiles:
downloaded on a small thread pool, cached on disk and drawn as they arrive.

## Build

    meson setup build
    ninja -C build
    ninja -C build install   # installs libosmium, headers in include/osmium/ and osmium.pc

Needs GTK 4.10+ and libcurl.

## Use

Link with `pkg-config --libs osmium`, or as a meson subproject:

    osmium_dep = dependency('osmium', fallback: ['osmium', 'osmium_dep'])

```c
#include <osmium.h>

g_type_ensure(OSMIUM_TYPE_MAP);         /* before building a template that uses them */
g_type_ensure(OSMIUM_TYPE_TILE_LAYER);
```

```xml
<object class="OsmiumMap">
  <property name="lat">-14.235</property>
  <property name="lon">-51.925</property>
  <property name="zoom">5</property>
  <child type="overlay">
    <object class="OsmiumTileLayer">
      <property name="user-agent">MyApp/1.0 (contact: me@example.com)</property>
    </object>
  </child>
</object>
```

| Property | Default | Notes |
|---|---|---|
| `lat`, `lon` | 0, 0 | view center, clamped to ±85 / ±180 |
| `zoom` | 4 | 0 to 19 |
| `bearing` | 0 | rotation in degrees; the map is drawn rotated by `-bearing`, so bearing 90 puts east at the top |

Controls: drag or arrow keys to pan (Shift = faster), mouse wheel or `+`/`-` to zoom,
`[` and `]` to rotate. The map emits `view-changed` after every change of `lat`, `lon`, `zoom` or `bearing`.

## Overlay widgets

`OsmiumMap` is a container: widgets added with `osmium_map_add_overlay()` (or
`<child type="overlay">`) are drawn on top and placed by their `halign`, `valign` and margins.

```xml
<object class="OsmiumMap">
  <child type="overlay"><object class="OsmiumTileLayer"/></child>
  <child type="overlay"><object class="OsmiumCompass"/></child>
  <child type="overlay"><object class="OsmiumZoomControl"/></child>
</object>
```

Overlays draw in the order added: put layers first, controls after.

* `OsmiumTileLayer`: a TMS/XYZ tile set. Never takes input; the map handles it.

  | Property | Default | Notes |
  |---|---|---|
  | `template-url` | OSM standard | `{z}`, `{x}`, `{y}` placeholders |
  | `user-agent` | `Osmium/0.1` | **set this**: the OSM tile policy requires an identifying agent |
  | `cache-folder` | `~/.cache/osmium/tiles` | tiles stored as `<z>/<x>/<y>.png` |

* `OsmiumZoomControl`: "+" / "-" buttons, disabled at the zoom limits (bottom right by default).
* `OsmiumCompass`: needle follows the bearing; click to point north, drag around it to rotate the map (top right by default).

All three have a `map` property that is set automatically when they are added as overlays.

Respect the tile server's usage policy. The OSM one allows at most 2 connections;
the pool size is `OSMIUM_TILE_LAYER_DOWNLOAD_THREADS` in `src/osmium_tile_layer.c`.

## License

LGPL-2.1-or-later. See `COPYING`.
