/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "osmium_analysis.h"
#include "osmium_compass.h"
#include "osmium_crs.h"
#include "osmium_expression.h"
#include "osmium_georef.h"
#include "osmium_geodesy.h"
#include "osmium_map.h"
#include "osmium_vector_layer.h"
#include "osmium_vector_style.h"
#include "osmium_raster_layer.h"
#include "osmium_scale_bar.h"
#include "osmium_tile_layer.h"
#include "osmium_wms_layer.h"
#include "osmium_zoom_control.h"

typedef enum {
  OSMIUM_OK = 1,
  OSMIUM_NULL_POINTER = -1,
  OSMIUM_INVALID_BBOX = -2,
  OSMIUM_URL_ERROR = -3,
  OSMIUM_CACHE_ERROR = -4,
  OSMIUM_NETWORK_ERROR = -5,
  OSMIUM_PARSE_ERROR = -6,
  OSMIUM_WMS_EXCEPTION = -7,
  OSMIUM_OUT_OF_MEMORY = -8,
  OSMIUM_FILE_ERROR = -9,
  OSMIUM_CRS_ERROR = -10
} osmium_error;

#include "wms/wms.h"
