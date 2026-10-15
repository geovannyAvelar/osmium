/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stddef.h>

#include "../osmium.h"

typedef struct OsmiumWmsCapsLayer OsmiumWmsCapsLayer;

struct OsmiumWmsCapsLayer {
  char *name;
  char *title;
  char *abstract;
  char **crs;
  size_t n_crs;
  int has_bbox;
  double bbox[4];
  int queryable;
  OsmiumWmsCapsLayer **children;
  size_t n_children;
};

typedef struct {
  char *version;
  char *service_title;
  char *getmap_url;
  char **formats;
  size_t n_formats;
  OsmiumWmsCapsLayer *root;
} OsmiumWmsCapabilities;

typedef struct {
  const char *url;
  const char *version;
  const char *layers;
  const char *styles;
  const char *crs;
  double bbox[4];
  int width, height;
  const char *format;
  int transparent;
} OsmiumWmsGetMapParams;

typedef struct {
  unsigned char *data;
  size_t len;
  char *content_type;
} OsmiumWmsImage;

osmium_error osmium_wms_get_map(const OsmiumWmsGetMapParams *params,
                                const char *user_agent,
                                OsmiumWmsImage *out);
osmium_error osmium_wms_get_capabilities(const char *server_url,
                                         const char *user_agent,
                                         OsmiumWmsCapabilities **out);
void osmium_wms_capabilities_free(OsmiumWmsCapabilities *caps);
void osmium_wms_image_free(OsmiumWmsImage *image);
